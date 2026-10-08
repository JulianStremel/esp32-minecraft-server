#include "mc/world/light.h"
#include <stdlib.h>
#include <string.h>
#include "mc/limits.h"
#include "mc/platform.h"
#include "mc/registry.h"
#include "mc/world/world.h"

namespace mc {

// Queue entries and emitters hold packed grid positions: y << 12 | z << 6 | x.
static inline uint32_t pack(int x, int y, int z) { return ((uint32_t)y << 12) | ((uint32_t)z << 6) | (uint32_t)x; }

ChunkLight::~ChunkLight() {
    plat::bigFree(sky_);
    plat::bigFree(block_);
    plat::bigFree(cells_);
    plat::bigFree(queue_);
    plat::bigFree(emit_);
    plat::bigFree(touched_);
    free(tmp_);
    free(direct_);
    free(dist_);
    free(fall_);
}

namespace {

// Breadth-first light flood over the grid. A cell holds the filter (how much light it
// takes away) in its low nibble and its light in the high nibble. For sky light, the cells
// from direct[column] up are not stored: they hold direct sky light (15).
// dist[column] is the number of steps to the centre chunk: light that is not brighter
// than that can never reach it (each step costs at least 1), so it is not spread
// further. That keeps the region mode's margin cheap and its centre exact.
struct Bfs {
    uint8_t* cells;
    uint32_t* q;
    uint32_t cap, head = 0, tail = 0, size = 0;
    int W, H;
    const int16_t* direct;   // sky only, else nullptr
    const uint8_t* dist;
    // block pass: every cell it lights, so they can be cleared for the sky pass
    uint32_t* touched = nullptr;
    uint32_t* touchedN = nullptr;
    uint32_t touchedCap = 0;
    bool* touchedOver = nullptr;
    bool overflow = false;
    uint32_t pushes = 0;

    Bfs(uint8_t* c, uint32_t* queue, uint32_t capacity, int w, int h, const int16_t* dir, const uint8_t* dst)
        : cells(c), q(queue), cap(capacity), W(w), H(h), direct(dir), dist(dst) {}

    inline size_t at(int x, int y, int z) const { return ((size_t)y * W + z) * W + x; }
    inline int light(int x, int y, int z) const {
        if (direct && y >= direct[z * W + x]) return 15;
        return cells[at(x, y, z)] >> 4;
    }

    void push(uint32_t p) {
        if (size == cap) { overflow = true; return; }
        pushes++;
        q[tail] = p;
        tail = tail + 1 == cap ? 0 : tail + 1;
        size++;
    }

    inline void touch(size_t i) {
        if (!touched) return;
        if (*touchedN < touchedCap) touched[(*touchedN)++] = (uint32_t)i;
        else *touchedOver = true;
    }

    // Tries to raise neighbour (x,y,z) with light L coming from above (down) or the side.
    inline void spread(int x, int y, int z, int L, bool down) {
        int col = z * W + x;
        if (direct && y >= direct[col]) return;   // already 15
        size_t i = at(x, y, z);
        uint8_t c = cells[i];
        int f = c & 15;
        if (f >= 15) return;
        int nl = (direct && down && L == 15 && f == 0) ? 15 : L - (f > 1 ? f : 1);
        if (nl <= dist[col] || (c >> 4) >= nl) return;
        if (!(c >> 4)) touch(i);
        cells[i] = (uint8_t)(f | (nl << 4));
        push(pack(x, y, z));
    }

    void step(uint32_t p) {
        int x = p & 63, z = (p >> 6) & 63, y = (int)(p >> 12);
        int L = light(x, y, z);
        if (L <= 1) return;
        if (x > 0) spread(x - 1, y, z, L, false);
        if (x < W - 1) spread(x + 1, y, z, L, false);
        if (z > 0) spread(x, y, z - 1, L, false);
        if (z < W - 1) spread(x, y, z + 1, L, false);
        if (y > 0) spread(x, y - 1, z, L, true);
        if (y < H - 1) spread(x, y + 1, z, L, false);
    }

    void drain() {
        while (size > 0) {
            uint32_t p = q[head];
            head = head + 1 == cap ? 0 : head + 1;
            size--;
            step(p);
        }
    }

    void run() {
        drain();
        // queue overflowed: relax by full sweeps until stable (correct, just slower)
        while (overflow) {
            overflow = false;
            for (int y = 0; y < H; y++)
                for (int z = 0; z < W; z++)
                    for (int x = 0; x < W; x++)
                        if (light(x, y, z) > 1) {
                            step(pack(x, y, z));
                            drain();
                        }
        }
    }
};

}  // namespace

static const int8_t EDGE_DX[4] = {-1, 1, 0, 0}, EDGE_DZ[4] = {0, 0, -1, 1};

void NeighbourEdges::gather(const World& world, int cx, int cz) {
    for (int k = 0; k < 4; k++) {
        const Chunk* n = world.peek(cx + EDGE_DX[k], cz + EDGE_DZ[k]);
        present[k] = n != nullptr;
        if (!n) continue;
        for (int i = 0; i < 16; i++) {
            // the neighbour's column that touches our border cell i
            int nx = EDGE_DX[k] < 0 ? 15 : (EDGE_DX[k] > 0 ? 0 : i);
            int nz = EDGE_DZ[k] < 0 ? 15 : (EDGE_DZ[k] > 0 ? 0 : i);
            height[k][i] = (uint16_t)n->height(nx, nz);
        }
    }
}

bool ChunkLight::compute(const Chunk& c, World* world) {
    if (!world) return computeChunk(c, nullptr);
    NeighbourEdges e;
    e.gather(*world, c.cx, c.cz);
    return computeChunk(c, &e);
}

bool ChunkLight::reserve(int W, int H, int outSections) {
    size_t cells = (size_t)W * W * H;
    if (cells > cellCap_) {
        plat::bigFree(cells_);
        cells_ = (uint8_t*)plat::bigAlloc(cells);
        cellCap_ = cells_ ? cells : 0;
        if (!cells_) return false;
    }
    // small and touched for every cell: internal RAM
    if (!tmp_ && !(tmp_ = (uint8_t*)malloc(SECTION_BLOCKS))) return false;
    if (!direct_ && !(direct_ = (int16_t*)malloc(sizeof(int16_t) * REGION_W * REGION_W))) return false;
    if (!dist_ && !(dist_ = (uint8_t*)malloc(REGION_W * REGION_W))) return false;
    if (!fall_ && !(fall_ = (uint8_t*)malloc(REGION_W * REGION_W))) return false;
    // the region's flood fronts are larger than one chunk's
    uint32_t qwant = W > 16 ? (MC_LIGHT_QUEUE > 16384 ? MC_LIGHT_QUEUE : 16384) : MC_LIGHT_QUEUE;
    if (qwant > qcap_) {
        uint32_t* q = (uint32_t*)plat::bigAlloc(qwant * 4);
        if (q) {
            plat::bigFree(queue_);
            queue_ = q;
            qcap_ = qwant;
        } else if (!queue_) {
            qcap_ = 512;   // a small queue still works (overflow falls back to sweeps)
            if (!(queue_ = (uint32_t*)plat::bigAlloc(qcap_ * 4))) { qcap_ = 0; return false; }
        }
    }
    if (!touched_) {
        touchedCap_ = 8192;   // more than that: one full clear instead
        if (!(touched_ = (uint32_t*)plat::bigAlloc(touchedCap_ * 4))) touchedCap_ = 0;
    }
    if (outSections > capSections_) {
        plat::bigFree(sky_);
        plat::bigFree(block_);
        sky_ = (uint8_t*)plat::bigAlloc((size_t)outSections * 2048);
        block_ = (uint8_t*)plat::bigAlloc((size_t)outSections * 2048);
        capSections_ = outSections;
        if (!sky_ || !block_) { capSections_ = 0; return false; }
    }
    W_ = W;
    H_ = H;
    numSections_ = outSections;
    emitN_ = 0;
    return true;
}

bool ChunkLight::pushEmitter(uint32_t p, int level) {
    if (emitN_ == emitCap_) {
        uint32_t cap = emitCap_ ? emitCap_ * 2 : 1024;
        uint32_t* e = (uint32_t*)plat::bigAlloc(cap * 4);
        if (!e) return false;
        if (emit_) memcpy(e, emit_, emitN_ * 4);
        plat::bigFree(emit_);
        emit_ = e;
        emitCap_ = cap;
    }
    emit_[emitN_++] = (p << 4) | (uint32_t)level;
    return true;
}

// Writes the filters of chunk c's local columns [x0, x1) x [z0, z1) into the grid (every
// cell of that part exactly once), its local (0, 0) at grid (ox, oz), and collects its
// light sources there.
// The filter of every block of section s of c if they all have the same one and none
// emits light (all air; stone underground, plants above ground, open water), else -1:
// such sections need no decoding.
static int uniformFilter(const Chunk& c, int s) {
    const Section* sec = c.section(s);
    if (!sec || sec->nonAirCount() == 0) return 0;
    uint8_t v;
    bool same = sec->mapsUniformly([](uint16_t st) {
        const BlockDef& b = blockOf(st);
        return (uint8_t)(b.emitLight ? 0xFF : b.filterLight);   // 0xFF: a light source
    }, v);
    return same && v != 0xFF ? v : -1;
}

bool ChunkLight::fillFrom(const Chunk& c, int ox, int oz, int x0, int x1, int z0, int z1, uint16_t skip) {
    const int W = W_;
    const int n = x1 - x0;
    for (int s = 0; s < H_ / 16; s++) {
        if (skip & (1u << s)) continue;   // filled by the caller
        const Section* sec = c.section(s);
        int same = uniformFilter(c, s);
        if (same >= 0) {
            for (int ly = 0; ly < 16; ly++)
                for (int z = z0; z < z1; z++) {
                    uint8_t* dst = cells_ + ((size_t)(s * 16 + ly) * W + (z + oz)) * W + ox + x0;
                    for (int x = 0; x < n; x++) dst[x] = (uint8_t)same;   // rows of at most 16
                }
            continue;
        }
        bool emits = sec->anyState([](uint16_t st) { return blockOf(st).emitLight != 0; });
        if (emits) sec->mapStates(tmp_, [](uint16_t st) {
            const BlockDef& b = blockOf(st);
            return (uint8_t)(b.filterLight | (b.emitLight << 4));
        });
        else sec->mapStates(tmp_, [](uint16_t st) { return (uint8_t)blockOf(st).filterLight; });
        for (int ly = 0; ly < 16; ly++) {
            int y = s * 16 + ly;
            for (int z = z0; z < z1; z++) {
                const uint8_t* src = tmp_ + (ly << 8) + (z << 4);
                uint8_t* dst = cells_ + ((size_t)y * W + (z + oz)) * W + ox;
                if (!emits) {
                    memcpy(dst + x0, src + x0, (size_t)n);
                    continue;
                }
                for (int x = x0; x < x1; x++) {
                    dst[x] = src[x] & 15;
                    if (src[x] >> 4 && !pushEmitter(pack(x + ox, y, z + oz), src[x] >> 4)) return false;
                }
            }
        }
    }
    return true;
}

// Direct sky light: per column, from the top down to the first block that filters
// light. Scanned layer by layer (the grid's memory order), not column by column.
void ChunkLight::findDirect() {
    const int W = W_, H = H_, cols = W * W;
    for (int i = 0; i < cols; i++) direct_[i] = (int16_t)H;
    int open = cols;
    for (int y = H - 1; y >= 0 && open > 0; y--) {
        const uint8_t* layer = cells_ + (size_t)y * cols;
        for (int i = 0; i < cols; i++) {
            if (direct_[i] != y + 1) continue;
            if ((layer[i] & 15) == 0) direct_[i] = (int16_t)y;
            else open--;
        }
    }
}

void ChunkLight::skyPass(const NeighbourEdges* edges) {
    const int W = W_, H = H_;
    uint8_t* cells = cells_;
    uint64_t t0 = plat::micros();
    findDirect();
    phaseUs[PH_DIRECT] = (uint32_t)(plat::micros() - t0);
    Bfs sky(cells, queue_, qcap_, W, H, direct_, dist_);
    const int cols = W * W;
    // vertical pass: sky light falling straight down through blocks that dim it (water,
    // leaves), layer by layer in memory order. Open water and flat ground need nothing
    // else; the flood below only handles light that moves sideways.
    for (int c = 0; c < cols; c++) fall_[c] = 15;
    int active = cols, low = H;
    for (int y = H - 1; y >= 0 && active > 0; y--) {
        uint8_t* layer = cells + (size_t)y * cols;
        for (int c = 0; c < cols; c++) {
            int L = fall_[c];
            if (!L || y >= direct_[c]) continue;
            int f = layer[c] & 15;
            int nl = f >= 15 ? 0 : L - (f > 1 ? f : 1);
            if (nl <= dist_[c]) {   // dark from here down (or too weak to reach the centre)
                fall_[c] = 0;
                active--;
                continue;
            }
            layer[c] = (uint8_t)(f | (nl << 4));
            fall_[c] = (uint8_t)nl;
            low = y;
        }
    }
    // seeds: lit cells (direct or from the vertical pass) whose light would brighten a
    // neighbour beside them. Above the highest direct boundary every neighbour is direct.
    int minD = H, maxD = 0;
    for (int c = 0; c < cols; c++) {
        if (direct_[c] < minD) minD = direct_[c];
        if (direct_[c] > maxD) maxD = direct_[c];
    }
    for (int y = low < minD ? low : minD; y < maxD; y++) {
        const uint8_t* layer = cells + (size_t)y * cols;
        for (int z = 0; z < W; z++)
            for (int x = 0; x < W; x++) {
                int c = z * W + x;
                int L = y >= direct_[c] ? 15 : layer[c] >> 4;
                if (L <= 1) continue;
                auto brighter = [&](int n) {
                    if (y >= direct_[n]) return false;
                    int fn = layer[n] & 15;
                    if (fn >= 15) return false;
                    int want = L - (fn > 1 ? fn : 1);
                    return want > (layer[n] >> 4) && want > dist_[n];
                };
                if ((x > 0 && brighter(c - 1)) || (x < W - 1 && brighter(c + 1)) || (z > 0 && brighter(c - W)) ||
                    (z < W - 1 && brighter(c + W)))
                    sky.push(pack(x, y, z));
            }
    }
    // per-chunk mode: open-sky columns of the neighbours next to our border cells
    if (edges) {
        for (int k = 0; k < 4; k++) {
            if (!edges->present[k]) continue;
            for (int i = 0; i < 16; i++) {
                int x = EDGE_DX[k] < 0 ? 0 : (EDGE_DX[k] > 0 ? 15 : i);
                int z = EDGE_DZ[k] < 0 ? 0 : (EDGE_DZ[k] > 0 ? 15 : i);
                for (int y = edges->height[k][i]; y < direct_[z * W + x] && y < H; y++) {
                    uint8_t& c = cells[((size_t)y * W + z) * W + x];
                    int f = c & 15;
                    if (f >= 15) continue;
                    int nl = 15 - (f > 1 ? f : 1);
                    if ((c >> 4) < nl) {
                        c = (uint8_t)(f | (nl << 4));
                        sky.push(pack(x, y, z));
                    }
                }
            }
        }
    }
    sky.run();
    skyPushes = sky.pushes;
}

// Block light, on a grid whose light nibbles are all 0; records the cells it lights.
void ChunkLight::blockPass() {
    Bfs bl(cells_, queue_, qcap_, W_, H_, nullptr, dist_);
    touchedN_ = 0;
    touchedOver_ = touchedCap_ == 0;
    bl.touched = touched_;
    bl.touchedN = &touchedN_;
    bl.touchedCap = touchedCap_;
    bl.touchedOver = &touchedOver_;
    for (uint32_t k = 0; k < emitN_; k++) {
        uint32_t p = emit_[k] >> 4;
        int e = (int)(emit_[k] & 15);
        int x = p & 63, z = (p >> 6) & 63, y = (int)(p >> 12);
        if (e <= dist_[z * W_ + x]) continue;   // too weak to reach the centre
        size_t i = ((size_t)y * W_ + z) * W_ + x;
        uint8_t c = cells_[i];
        if ((c >> 4) < e) {
            if (!(c >> 4)) bl.touch(i);
            cells_[i] = (uint8_t)((c & 15) | (e << 4));
            bl.push(p);
        }
    }
    bl.run();
    blockPushes = bl.pushes;
}

void ChunkLight::clearTouched() {
    if (touchedOver_) {
        size_t n = (size_t)W_ * W_ * H_;
        for (size_t i = 0; i < n; i++) cells_[i] &= 15;
    } else {
        for (uint32_t k = 0; k < touchedN_; k++) cells_[touched_[k]] &= 15;
    }
}

// The centre chunk's light (high nibbles; direct sky from direct_ up) into the 1.16
// section nibble layout: x is the fastest axis, two cells per byte, low nibble first.
void ChunkLight::copyOut(uint8_t* dst, bool sky) {
    const int W = W_, off = off_;
    uint32_t nonZero = 0;
    for (int y = 0; y < numSections_ * 16; y++) {
        uint8_t any = 0;
        for (int z = 0; z < 16; z++) {
            const uint8_t* row = cells_ + ((size_t)y * W + (z + off)) * W + off;
            const int16_t* dir = direct_ + (z + off) * W + off;
            uint8_t* out = dst + (((uint32_t)y << 8 | (uint32_t)z << 4) >> 1);
            for (int x = 0; x < 16; x += 2) {
                int a = sky && y >= dir[x] ? 15 : row[x] >> 4;
                int b = sky && y >= dir[x + 1] ? 15 : row[x + 1] >> 4;
                uint8_t v = (uint8_t)(a | (b << 4));
                out[x >> 1] = v;
                any |= v;
            }
        }
        if (any) nonZero |= 1u << (y >> 4);
    }
    if (!sky) blockNonZero_ = nonZero;
}

// Shared by both modes once the grid is filled: block light first (it usually lights
// few cells, which are then cleared again), then sky light.
bool ChunkLight::run(const NeighbourEdges* edges) {
    uint64_t t1 = plat::micros();
    blockPass();
    uint64_t t2 = plat::micros();
    copyOut(block_, false);
    clearTouched();
    uint64_t t3 = plat::micros();
    skyPass(edges);
    uint64_t t4 = plat::micros();
    copyOut(sky_, true);
    uint64_t t5 = plat::micros();
    phaseUs[PH_BLOCK] = (uint32_t)(t2 - t1);
    phaseUs[PH_SKY] = (uint32_t)(t4 - t3);
    phaseUs[PH_OUT] = (uint32_t)((t3 - t2) + (t5 - t4));
    return true;
}

bool ChunkLight::computeChunk(const Chunk& c, const NeighbourEdges* edges) {
    uint64_t t0 = plat::micros();
    int top = c.highestSection();
    int ns = top + 2;
    if (ns > NUM_SECTIONS) ns = NUM_SECTIONS;
    if (ns < 1) ns = 1;
    if (!reserve(16, ns * 16, ns)) return false;
    off_ = 0;
    memset(dist_, 0, 256);
    if (!fillFrom(c, 0, 0, 0, 16, 0, 16, 0)) return false;
    phaseUs[PH_FILL] = (uint32_t)(plat::micros() - t0);
    return run(edges);
}

bool ChunkLight::computeRegion(const Chunk* const nine[9]) {
    uint64_t t0 = plat::micros();
    // sections up to one above the highest block anywhere in reach: light from a taller
    // neighbour (a torch on a mountain next to the border) reaches above our own blocks
    int ns = 1;
    for (int k = 0; k < 9; k++) {
        int t = nine[k]->highestSection() + 2;
        if (t > ns) ns = t;
    }
    if (ns > NUM_SECTIONS) ns = NUM_SECTIONS;
    const int M = MARGIN, W = REGION_W;
    if (!reserve(W, ns * 16, ns)) return false;
    off_ = M;
    for (int z = 0; z < W; z++)
        for (int x = 0; x < W; x++) {
            int dx = x < M ? M - x : (x >= M + 16 ? x - (M + 15) : 0);
            int dz = z < M ? M - z : (z >= M + 16 ? z - (M + 15) : 0);
            dist_[z * W + x] = (uint8_t)(dx + dz > 15 ? 15 : dx + dz);
        }
    // section layers that are the same uniform filter in all nine chunks (air above the
    // terrain): one contiguous fill for the whole slab instead of small rows
    uint16_t slab = 0;
    for (int s = 0; s < ns; s++) {
        int v = uniformFilter(*nine[0], s);
        for (int k = 1; k < 9 && v >= 0; k++)
            if (uniformFilter(*nine[k], s) != v) v = -1;
        if (v < 0) continue;
        memset(cells_ + (size_t)s * 16 * W * W, v, (size_t)16 * W * W);
        slab |= (uint16_t)(1u << s);
    }
    for (int dz = -1; dz <= 1; dz++)
        for (int dx = -1; dx <= 1; dx++) {
            // the part of this chunk inside the window [-M, 16 + M) around the centre
            int x0 = dx < 0 ? 16 - M : 0, x1 = dx > 0 ? M : 16;
            int z0 = dz < 0 ? 16 - M : 0, z1 = dz > 0 ? M : 16;
            if (!fillFrom(*nine[(dz + 1) * 3 + (dx + 1)], M + dx * 16, M + dz * 16, x0, x1, z0, z1, slab)) return false;
        }
    phaseUs[PH_FILL] = (uint32_t)(plat::micros() - t0);
    bool ok = run(nullptr);
    // the region grid is several times a chunk's: do not keep it between computations
    if (cellCap_ > (size_t)16 * 16 * WORLD_HEIGHT) {
        plat::bigFree(cells_);
        cells_ = nullptr;
        cellCap_ = 0;
    }
    return ok;
}

}  // namespace mc
