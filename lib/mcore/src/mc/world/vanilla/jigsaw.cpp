// Jigsaw structures: vanilla's JigsawPlacement (1.21.8): the start element from its pool,
// raised to the ground, then each piece's jigsaw blocks in vanilla's order (shuffled, by
// selection priority), pieces by placement priority, each jigsaw offered its pool's
// elements shuffled by weight (then the fallback's), each in four shuffled rotations,
// attached where the jigsaws face each other with matching names and fit the free space
// (the area around the start less the pieces, or inside the parent for a jigsaw pointing
// into it); rigid pieces keep their parent's height, terrain-matching ones follow the
// ground. The same random numbers in the same order as vanilla: a start gives vanilla's
// layout for the seed the generator's Random is given (where the terrain's heights agree).
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "mc/platform.h"
#include "mc/world/vanilla/structure_build.h"
#include "mc/world/vanilla/templates.h"

namespace mc {
namespace vanilla {
namespace sb {

namespace {

// directions as Minecraft's Direction: down, up, north, south, west, east
enum : uint8_t { D_DOWN, D_UP, D_NORTH, D_SOUTH, D_WEST, D_EAST };
// FrontAndTop in TdJigsaw's order: down_east .. south_up
const uint8_t FRONT[12] = {D_DOWN, D_DOWN, D_DOWN, D_DOWN, D_UP, D_UP, D_UP, D_UP, D_WEST, D_EAST, D_NORTH, D_SOUTH};
const uint8_t TOP[12] = {D_EAST, D_NORTH, D_SOUTH, D_WEST, D_EAST, D_NORTH, D_SOUTH, D_WEST, D_UP, D_UP, D_UP, D_UP};
const int8_t DX[6] = {0, 0, 0, 0, -1, 1}, DY[6] = {-1, 1, 0, 0, 0, 0}, DZ[6] = {0, 0, -1, 1, 0, 0};
uint8_t opposite(uint8_t d) { return d ^ 1; }
// Rotation#rotate(Direction): clockwise steps on the horizontal ones
uint8_t rotateDir(uint8_t d, int rot) {
    static const uint8_t ORDER[4] = {D_NORTH, D_EAST, D_SOUTH, D_WEST};
    if (d < 2) return d;
    int i = d == D_NORTH ? 0 : d == D_EAST ? 1 : d == D_SOUTH ? 2 : 3;
    return ORDER[(i + rot) & 3];
}
void rotXZ(int x, int z, int rot, int& ox, int& oz) {
    switch (rot & 3) {
        case 1: ox = -z; oz = x; break;
        case 2: ox = -x; oz = -z; break;
        case 3: ox = z; oz = -x; break;
        default: ox = x; oz = z; break;
    }
}

struct Jig { int x, y, z; uint8_t front, top, joint; int8_t selection, placement; uint16_t name, target, pool; };

const TdElement& element(int e) { return TD_ELEMENTS[e]; }

// StructurePoolElement#getBoundingBox
SBox elementBox(const TdElement& e, int x, int y, int z, int rot) {
    if (e.type == TE_SINGLE || e.type == TE_LEGACY) return templateBox(e.ref, x, y, z, rot, 0);
    if (e.type == TE_LIST) {
        SBox b = {x, y, z, x, y, z};
        bool first = true;
        for (int i = 0; i < e.count; i++) {
            const TdElement& s = TD_LIST_ITEMS[e.ref + i];
            if (s.type != TE_SINGLE && s.type != TE_LEGACY) continue;
            SBox t = templateBox(s.ref, x, y, z, rot, 0);
            if (first) b = t;
            else {
                if (t.x0 < b.x0) b.x0 = t.x0;
                if (t.y0 < b.y0) b.y0 = t.y0;
                if (t.z0 < b.z0) b.z0 = t.z0;
                if (t.x1 > b.x1) b.x1 = t.x1;
                if (t.y1 > b.y1) b.y1 = t.y1;
                if (t.z1 > b.z1) b.z1 = t.z1;
            }
            first = false;
        }
        return b;
    }
    return {x, y, z, x, y, z};   // a feature: its block
}

// the element's jigsaws at (x, y, z) rotated, in the template's order (y, x, z)
void elementJigsaws(const TdElement& e, int x, int y, int z, int rot, std::vector<Jig>& out) {
    out.clear();
    const TdElement* t = &e;
    if (e.type == TE_LIST) {
        if (!e.count) return;
        t = &TD_LIST_ITEMS[e.ref];   // ListPoolElement: the first element's
    }
    if (t->type == TE_FEATURE) {   // FeaturePoolElement's one jigsaw: "bottom", down, south
        out.push_back({x, y, z, D_DOWN, D_SOUTH, 0, 0, 0, 0, 0, 0});
        static int bottom = -1;
        if (bottom < 0)
            for (int i = 0; i < TD_NUM_STRINGS; i++)
                if (!strcmp(TD_STRINGS[i], "minecraft:bottom")) bottom = i;
        out.back().name = (uint16_t)(bottom < 0 ? 0 : bottom);
        return;
    }
    if (t->type != TE_SINGLE && t->type != TE_LEGACY) return;
    const TdTemplate& tp = TD_TEMPLATES[t->ref];
    for (int i = 0; i < tp.jigCount; i++) {
        const TdJigsaw& j = TD_JIGSAWS[tp.jigFirst + i];
        int ox, oz;
        rotXZ(j.x, j.z, rot, ox, oz);
        Jig g;
        g.x = x + ox;
        g.y = y + j.y;
        g.z = z + oz;
        g.front = rotateDir(FRONT[j.orient], rot);
        g.top = rotateDir(TOP[j.orient], rot);
        g.joint = j.joint;
        g.selection = j.selection;
        g.placement = j.placement;
        g.name = j.name;
        g.target = j.target;
        g.pool = j.pool;
        out.push_back(g);
    }
}

// Util.shuffle (Collections.shuffle's order)
template <typename T> void shuffle(std::vector<T>& v, JavaRandom& r) {
    for (int i = (int)v.size(); i > 1; i--) {
        int j = r.nextInt(i);
        T t = v[i - 1];
        v[i - 1] = v[j];
        v[j] = t;
    }
}
// SinglePoolElement#getShuffledJigsawBlocks: shuffled, then by selection priority (stable)
void shuffledJigsaws(const TdElement& e, int x, int y, int z, int rot, JavaRandom& r, std::vector<Jig>& out) {
    elementJigsaws(e, x, y, z, rot, out);
    shuffle(out, r);
    for (size_t i = 1; i < out.size(); i++)   // insertion sort: stable, highest priority first
        for (size_t k = i; k > 0 && out[k].selection > out[k - 1].selection; k--) {
            Jig t = out[k];
            out[k] = out[k - 1];
            out[k - 1] = t;
        }
}

int poolIndex(uint16_t name) {   // a jigsaw's pool by its name string ("minecraft:village/...")
    const char* n = TD_STRINGS[name];
    if (!strncmp(n, "minecraft:", 10)) n += 10;
    for (int i = 0; i < TD_NUM_POOLS; i++)
        if (!strcmp(TD_STRINGS[TD_POOLS[i].name], n)) return i;
    return -1;
}
// StructureTemplatePool's templates list: each element as often as its weight
void poolTemplates(int pool, std::vector<int>& out) {
    out.clear();
    if (pool < 0) return;
    const TdPool& p = TD_POOLS[pool];
    for (int i = 0; i < p.count; i++)
        for (int w = 0; w < TD_ELEMENTS[p.first + i].weight; w++) out.push_back(p.first + i);
}
// StructureTemplatePool#getMaxSize: the tallest of its elements (unrotated)
int poolMaxSize(int pool) {
    if (pool < 0) return 0;
    const TdPool& p = TD_POOLS[pool];
    int m = 0;
    for (int i = 0; i < p.count; i++) {
        const TdElement& e = TD_ELEMENTS[p.first + i];
        if (e.type == TE_EMPTY) continue;
        SBox b = elementBox(e, 0, 0, 0, 0);
        if (b.y1 - b.y0 + 1 > m) m = b.y1 - b.y0 + 1;
    }
    return m;
}
bool emptyPool(int pool) {
    return pool >= 0 && !strcmp(TD_STRINGS[TD_POOLS[pool].name], "empty");
}

// the free space: a box less the boxes taken out of it
struct Region {
    SBox bound;
    std::vector<SBox> holes;
    bool fits(const SBox& b) const {
        if (b.x0 < bound.x0 || b.y0 < bound.y0 || b.z0 < bound.z0 || b.x1 > bound.x1 || b.y1 > bound.y1 || b.z1 > bound.z1) return false;
        for (const SBox& h : holes)
            if (h.hits(b)) return false;
        return true;
    }
};

struct PlacedPiece { int elem; int x, y, z; int rot; SBox box; int groundDelta; };
struct Junction { int x, y, z; };
struct State { int piece, region, depth; int priority; uint32_t seq; };

}  // namespace

int jigsawPieces(const JigsawParams& jp, int64_t worldSeed, int cx, int cz, JavaRandom& r, TerrainHeight height,
                 bool (*biomeOk)(void* ctx, int x, int y, int z), void* ctx, Piece* out, int max) {
    int startPool = jp.startPool;
    if (startPool < 0) return 0;
    (void)worldSeed;
    int rot = r.nextInt(4);   // Rotation.getRandom
    std::vector<int> tl;
    poolTemplates(startPool, tl);
    if (tl.empty()) return 0;
    int e0 = tl[r.nextInt((int)tl.size())];
    if (element(e0).type == TE_EMPTY) return 0;
    int px = cx * 16, py = jp.startY, pz = cz * 16;
    int bx = px, by = py, bz = pz;
    std::vector<Jig> jigs;
    if (jp.startJigsawName) {   // getRandomNamedJigsaw: the start moved so that jigsaw is at the position
        shuffledJigsaws(element(e0), px, py, pz, rot, r, jigs);
        bool found = false;
        for (const Jig& j : jigs)
            if (j.name == jp.startJigsawName) {
                bx = j.x;
                by = j.y;
                bz = j.z;
                found = true;
                break;
            }
        if (!found) return 0;
    }
    int offX = bx - px, offY = by - py, offZ = bz - pz;
    int sx0 = px - offX, sy0 = py - offY, sz0 = pz - offZ;
    std::vector<PlacedPiece> pieces;
    std::vector<Junction> junctions;
    PlacedPiece start = {e0, sx0, sy0, sz0, rot, elementBox(element(e0), sx0, sy0, sz0, rot), 1};
    int i = (start.box.x1 + start.box.x0) / 2, j = (start.box.z1 + start.box.z0) / 2;
    int k = jp.projectToHeightmap ? py + height(ctx, i, j) : sy0;
    int l = start.box.y0 + start.groundDelta;
    start.y += k - l;
    start.box.moveY(k - l);
    const int sy = k + offY;
    if (!biomeOk(ctx, i, sy, j)) return 0;
    pieces.push_back(start);
    if (jp.maxDepth > 0) {
        std::vector<Region> regions;
        const int D = jp.maxDistance;
        Region outer;
        int ylo = sy - D > jp.minY + jp.padBottom ? sy - D : jp.minY + jp.padBottom;
        int yhi = sy + D < jp.maxY - jp.padTop ? sy + D : jp.maxY - jp.padTop;
        outer.bound = {i - D, ylo, j - D, i + D, yhi, j + D};
        outer.holes.push_back(start.box);
        regions.push_back(outer);
        std::vector<State> queue;
        uint32_t seq = 0;
        queue.push_back({0, 0, 0, 0, seq++});
        std::vector<int> cands, fallbackT;
        std::vector<Jig> candJigs;
        bool first = true;
        while (!queue.empty() && (int)pieces.size() < max) {
            // SequencedPriorityIterator: the highest placement priority, first come first
            size_t best = 0;
            if (!first)
                for (size_t q = 1; q < queue.size(); q++)
                    if (queue[q].priority > queue[best].priority ||
                        (queue[q].priority == queue[best].priority && queue[q].seq < queue[best].seq))
                        best = q;
            first = false;
            State st = queue[best];
            queue.erase(queue.begin() + best);
            // Placer#tryPlacingChildren
            PlacedPiece parent = pieces[st.piece];
            const TdElement& pe = element(parent.elem);
            bool rigid = pe.projection == 0;
            int inner = -1;
            SBox box = parent.box;
            int minY = box.y0;
            shuffledJigsaws(pe, parent.x, parent.y, parent.z, parent.rot, r, jigs);
            for (const Jig& jg : jigs) {
                int tx = jg.x + DX[jg.front], ty = jg.y + DY[jg.front], tz = jg.z + DZ[jg.front];
                int jy = jg.y - minY;
                int kk = INT32_MIN;
                int pool = poolIndex(jg.pool);
                if (pool < 0) continue;
                if (TD_POOLS[pool].count == 0 && !emptyPool(pool)) continue;
                int fb = TD_POOLS[pool].fallback;
                if (fb < 0 || (TD_POOLS[fb].count == 0 && !emptyPool(fb))) continue;
                bool insideParent = box.inside(tx, ty, tz);
                int reg;
                if (insideParent) {
                    if (inner < 0) {
                        Region in;
                        in.bound = box;
                        regions.push_back(in);
                        inner = (int)regions.size() - 1;
                    }
                    reg = inner;
                } else {
                    reg = st.region;
                }
                cands.clear();
                if (st.depth != jp.maxDepth) {
                    poolTemplates(pool, tl);
                    shuffle(tl, r);
                    cands = tl;
                }
                poolTemplates(fb, fallbackT);
                shuffle(fallbackT, r);
                cands.insert(cands.end(), fallbackT.begin(), fallbackT.end());
                int prio = jg.placement;
                bool placed = false;
                for (size_t ci = 0; ci < cands.size() && !placed; ci++) {
                    const TdElement& ce = element(cands[ci]);
                    if (ce.type == TE_EMPTY) break;
                    int rots[4] = {0, 1, 2, 3};   // Rotation.getShuffled
                    for (int q = 4; q > 1; q--) {
                        int w = r.nextInt(q);
                        int t2 = rots[q - 1];
                        rots[q - 1] = rots[w];
                        rots[w] = t2;
                    }
                    for (int ri = 0; ri < 4 && !placed; ri++) {
                        int crot = rots[ri];
                        shuffledJigsaws(ce, 0, 0, 0, crot, r, candJigs);
                        SBox b0 = elementBox(ce, 0, 0, 0, crot);
                        int m = 0;
                        if (jp.expansionHack && b0.y1 - b0.y0 + 1 <= 16)
                            for (const Jig& cj : candJigs) {
                                if (!b0.inside(cj.x + DX[cj.front], cj.y + DY[cj.front], cj.z + DZ[cj.front])) continue;
                                int cp = poolIndex(cj.pool);
                                int cf = cp >= 0 ? TD_POOLS[cp].fallback : -1;
                                int a = poolMaxSize(cp), bb = poolMaxSize(cf);
                                int v = a > bb ? a : bb;
                                if (v > m) m = v;
                            }
                        for (const Jig& cj : candJigs) {
                            // JigsawBlock#canAttach
                            if (jg.front != opposite(cj.front) || (jg.joint != 0 && jg.top != cj.top) || jg.target != cj.name) continue;
                            int ox = tx - cj.x, oy = ty - cj.y, oz = tz - cj.z;
                            SBox cb = elementBox(ce, ox, oy, oz, crot);
                            int n = cb.y0;
                            bool candRigid = ce.projection == 0;
                            int i1 = cj.y;
                            int j1 = jy - i1 + DY[jg.front];
                            int k1;
                            if (rigid && candRigid) k1 = minY + j1;
                            else {
                                if (kk == INT32_MIN) kk = height(ctx, jg.x, jg.z);
                                k1 = kk - i1;
                            }
                            int l1 = k1 - n;
                            SBox moved = cb;
                            moved.moveY(l1);
                            int oy2 = oy + l1;
                            if (m > 0) {
                                int i2 = m + 1 > moved.y1 - moved.y0 ? m + 1 : moved.y1 - moved.y0;
                                if (moved.y0 + i2 > moved.y1) moved.y1 = moved.y0 + i2;
                            }
                            if (!regions[reg].fits(moved)) continue;
                            regions[reg].holes.push_back(moved);
                            int ground = candRigid ? parent.groundDelta - j1 : 1;
                            PlacedPiece np = {cands[ci], ox, oy2, oz, crot, moved, ground};
                            pieces.push_back(np);
                            // the junctions on both sides (JigsawJunction: where the ground is at the joint)
                            int l2;
                            if (rigid) l2 = minY + jy;
                            else if (candRigid) l2 = k1 + i1;
                            else {
                                if (kk == INT32_MIN) kk = height(ctx, jg.x, jg.z);
                                l2 = kk + j1 / 2;
                            }
                            junctions.push_back({tx, l2 - jy + parent.groundDelta, tz});
                            junctions.push_back({jg.x, l2 - i1 + ground, jg.z});
                            if (st.depth + 1 <= jp.maxDepth) queue.push_back({(int)pieces.size() - 1, reg, st.depth + 1, prio, seq++});
                            placed = true;
                            break;
                        }
                    }
                }
                if ((int)pieces.size() >= max) break;
            }
        }
    }
    int n = (int)pieces.size() < max ? (int)pieces.size() : max;
    for (int q = 0; q < n; q++) {
        const PlacedPiece& pp = pieces[q];
        Piece& o = out[q];
        o = {};
        o.box = pp.box;
        o.kind = PK_JIGSAW;
        o.orient = (uint8_t)pp.rot;
        o.a = (int16_t)pp.elem;
        o.b = (int32_t)((uint32_t)(pp.x - pp.box.x0) | (uint32_t)(pp.z - pp.box.z0) << 8 | ((uint32_t)(pp.y - pp.box.y0) & 0xFF) << 16 |
                        ((uint32_t)pp.groundDelta & 0xFF) << 24);
    }
    for (const Junction& jn : junctions) {
        if (n >= max) break;
        Piece& o = out[n++];
        o = {};
        o.box = {jn.x, jn.y, jn.z, jn.x, jn.y, jn.z};
        o.kind = PK_JUNCTION;
    }
    return n;
}

// a jigsaw piece's blocks: its element's templates at its place and rotation
void buildJigsaw(Chunk& c, const SBox& clip, const Piece& p, int64_t seed) {
    int x = p.box.x0 + (p.b & 0xFF), z = p.box.z0 + ((p.b >> 8) & 0xFF), y = p.box.y0 + (int8_t)((p.b >> 16) & 0xFF);
    const TdElement& e = TD_ELEMENTS[p.a];
    auto one = [&](const TdElement& s) {
        if (s.type != TE_SINGLE && s.type != TE_LEGACY) return;
        TemplatePlacement t = {s.ref, x, y, z, p.orient, 0, s.processors, s.type == TE_LEGACY, s.projection == 1, false, false};
        placeTemplate(c, clip, t, seed);
    };
    if (e.type == TE_LIST)
        for (int i = 0; i < e.count; i++) one(TD_LIST_ITEMS[e.ref + i]);
    else one(e);
}

}  // namespace sb
}  // namespace vanilla
}  // namespace mc
