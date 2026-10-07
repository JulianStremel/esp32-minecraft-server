#include "mc/world/light.h"
#include <stdlib.h>
#include <string.h>
#include "mc/platform.h"
#include "mc/registry.h"
#include "mc/world/world.h"

namespace mc {

static inline int nib(const uint8_t* a, uint32_t i) { return (a[i >> 1] >> ((i & 1) << 2)) & 15; }
static inline void setNib(uint8_t* a, uint32_t i, int v) {
    uint8_t& b = a[i >> 1];
    b = (i & 1) ? (uint8_t)((b & 0x0F) | (v << 4)) : (uint8_t)((b & 0xF0) | v);
}
static inline uint32_t cellIndex(int x, int y, int z) { return ((uint32_t)y << 8) | ((uint32_t)z << 4) | (uint32_t)x; }

ChunkLight::~ChunkLight() {
    plat::bigFree(sky_);
    plat::bigFree(block_);
    plat::bigFree(queue_);
}

namespace {

struct Bfs {
    const Chunk& c;
    uint8_t* arr;
    uint32_t* q;
    uint32_t cap, head = 0, tail = 0, size = 0;
    int H;
    bool overflow = false;
    bool sky;

    Bfs(const Chunk& ch, uint8_t* a, uint32_t* queue, uint32_t capacity, int height, bool isSky)
        : c(ch), arr(a), q(queue), cap(capacity), H(height), sky(isSky) {}

    void push(uint32_t i) {
        if (size == cap) { overflow = true; return; }
        q[tail] = i;
        tail = (tail + 1) % cap;
        size++;
    }

    inline int filterAt(int x, int y, int z) const {
        const BlockDef& b = blockOf(c.get(x, y, z));
        return b.filterLight;
    }

    // Tries to raise neighbour (x,y,z) from light L coming from direction dir (5 = down).
    inline void spread(int x, int y, int z, int L, bool down) {
        if (x < 0 || x > 15 || z < 0 || z > 15 || y < 0 || y >= H) return;
        int f = filterAt(x, y, z);
        if (f >= 15) return;
        int nl = (sky && down && L == 15 && f == 0) ? 15 : L - (f > 1 ? f : 1);
        if (nl <= 0) return;
        uint32_t i = cellIndex(x, y, z);
        if (nib(arr, i) >= nl) return;
        setNib(arr, i, nl);
        push(i);
    }

    void step(uint32_t i) {
        int x = i & 15, z = (i >> 4) & 15, y = (int)(i >> 8);
        int L = nib(arr, i);
        if (L <= 1) return;
        spread(x - 1, y, z, L, false);
        spread(x + 1, y, z, L, false);
        spread(x, y, z - 1, L, false);
        spread(x, y, z + 1, L, false);
        spread(x, y - 1, z, L, true);
        spread(x, y + 1, z, L, false);
    }

    void run() {
        while (size > 0) {
            uint32_t i = q[head];
            head = (head + 1) % cap;
            size--;
            step(i);
        }
        // queue overflowed: relax by full sweeps until stable (correct, just slower)
        while (overflow) {
            overflow = false;
            for (int y = 0; y < H; y++)
                for (int z = 0; z < 16; z++)
                    for (int x = 0; x < 16; x++) {
                        uint32_t i = cellIndex(x, y, z);
                        if (nib(arr, i) > 1) {
                            step(i);
                            while (size > 0) {
                                uint32_t j = q[head];
                                head = (head + 1) % cap;
                                size--;
                                step(j);
                            }
                        }
                    }
        }
    }
};

}  // namespace

bool ChunkLight::compute(const Chunk& c, World* world) {
    int top = c.highestSection();
    numSections_ = top + 2;
    if (numSections_ > NUM_SECTIONS) numSections_ = NUM_SECTIONS;
    if (numSections_ < 1) numSections_ = 1;
    if (numSections_ > capSections_) {
        plat::bigFree(sky_);
        plat::bigFree(block_);
        sky_ = (uint8_t*)plat::bigAlloc((size_t)numSections_ * 2048);
        block_ = (uint8_t*)plat::bigAlloc((size_t)numSections_ * 2048);
        capSections_ = numSections_;
        if (!sky_ || !block_) { capSections_ = 0; return false; }
    }
    if (!queue_) {
        qcap_ = 4096;
        queue_ = (uint32_t*)plat::bigAlloc(qcap_ * 4);
        if (!queue_) { qcap_ = 512; queue_ = (uint32_t*)plat::bigAlloc(qcap_ * 4); }
        if (!queue_) { qcap_ = 0; return false; }
    }
    uint32_t qcap = qcap_;
    const int H = numSections_ * 16;
    memset(sky_, 0, (size_t)numSections_ * 2048);
    memset(block_, 0, (size_t)numSections_ * 2048);

    // ---- sky light, phase 1: direct light per column
    int16_t direct[256];  // lowest y that receives direct (15) sky light; H if none
    for (int z = 0; z < 16; z++)
        for (int x = 0; x < 16; x++) {
            int y = H - 1;
            while (y >= 0 && blockOf(c.get(x, y, z)).filterLight == 0) {
                setNib(sky_, cellIndex(x, y, z), 15);
                y--;
            }
            direct[z * 16 + x] = (int16_t)(y + 1);
        }
    Bfs sky(c, sky_, queue_, qcap, H, true);
    // seed: lit cells next to shaded columns, and the lowest lit cell of each column
    for (int z = 0; z < 16; z++)
        for (int x = 0; x < 16; x++) {
            int d = direct[z * 16 + x];
            int maxN = d;
            static const int8_t dx[4] = {-1, 1, 0, 0}, dz[4] = {0, 0, -1, 1};
            for (int k = 0; k < 4; k++) {
                int nx = x + dx[k], nz = z + dz[k];
                if (nx < 0 || nx > 15 || nz < 0 || nz > 15) continue;
                int nd = direct[nz * 16 + nx];
                if (nd > maxN) maxN = nd;
            }
            if (d < H) {
                for (int y = d; y < maxN && y < H; y++) sky.push(cellIndex(x, y, z));
                if (maxN <= d) sky.push(cellIndex(x, d, z));
            }
        }
    // seed from neighbouring chunks: open-sky columns next to our border cells
    if (world) {
        static const int8_t ndx[4] = {-1, 1, 0, 0}, ndz[4] = {0, 0, -1, 1};
        for (int k = 0; k < 4; k++) {
            Chunk* n = world->get(c.cx + ndx[k], c.cz + ndz[k]);
            if (!n) continue;
            for (int i = 0; i < 16; i++) {
                int x = ndx[k] < 0 ? 0 : (ndx[k] > 0 ? 15 : i);
                int z = ndz[k] < 0 ? 0 : (ndz[k] > 0 ? 15 : i);
                int nx = (x + ndx[k]) & 15, nz = (z + ndz[k]) & 15;
                int nTop = n->height(nx, nz);
                for (int y = nTop; y < direct[z * 16 + x] && y < H; y++) {
                    int f = blockOf(c.get(x, y, z)).filterLight;
                    if (f >= 15) continue;
                    int nl = 15 - (f > 1 ? f : 1);
                    uint32_t ci = cellIndex(x, y, z);
                    if (nib(sky_, ci) < nl) { setNib(sky_, ci, nl); sky.push(ci); }
                }
            }
        }
    }
    sky.run();

    // ---- block light: flood from emitters
    Bfs bl(c, block_, queue_, qcap, H, false);
    blockNonZero_ = 0;
    for (int s = 0; s < numSections_; s++) {
        const Section* sec = c.section(s);
        if (!sec || sec->nonAirCount() == 0) continue;
        if (sec->isUniform() && BLOCKS[blockIdOf(sec->uniformState())].emitLight == 0) continue;
        for (int i = 0; i < 4096; i++) {
            int e = blockOf(sec->get(i)).emitLight;
            if (!e) continue;
            uint32_t ci = ((uint32_t)s << 12) | (uint32_t)i;
            if (nib(block_, ci) < e) {
                setNib(block_, ci, e);
                bl.push(ci);
            }
        }
    }
    bl.run();
    for (int s = 0; s < numSections_; s++) {
        const uint8_t* p = block_ + (size_t)s * 2048;
        for (int i = 0; i < 2048; i++)
            if (p[i]) { blockNonZero_ |= 1u << s; break; }
    }
    return true;
}

}  // namespace mc
