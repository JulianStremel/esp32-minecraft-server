// The classic generator's villages (not vanilla's jigsaw villages): a meeting point from
// vanilla's town centres, two to four roads from it with branches, following the ground
// (planks over water), vanilla's houses of the biome's kind along them, each turned so
// that its door (its building_entrance jigsaw) faces the road, on a foundation; lamp
// posts; villagers around the meeting point.
#include <stdio.h>
#include <string.h>
#include "mc/registry.h"
#include "mc/world/classic/classic_structures.h"
#include "mc/world/generator.h"
#include "mc/world/noise.h"
#include "mc/world/vanilla/templates.h"

namespace mc {
namespace classic {

using vanilla::sb::Piece;
using vanilla::sb::SBox;

namespace {

enum Kind { K_PLAINS, K_DESERT, K_SAVANNA, K_TAIGA, K_SNOWY, K_NUM };
const char* const KIND_NAME[K_NUM] = {"plains", "desert", "savanna", "taiga", "snowy"};

// a template with its door: the building_entrance jigsaw's place and the way it faces
struct Building { int16_t tmpl; int8_t jx, jy, jz; uint8_t front; uint8_t weight; };
struct Catalog {
    Building houses[64];
    int nHouses = 0, totalWeight = 0;
    int16_t centres[8];
    int nCentres = 0;
};

enum { N = 0, E = 1, S = 2, W = 3 };
const int SX[4] = {0, 1, 0, -1}, SZ[4] = {-1, 0, 1, 0};

// FrontAndTop's index (JigsawBlock's orientation): the way the jigsaw faces
int frontOf(int orient) {
    static const int8_t F[12] = {-1, -1, -1, -1, -1, -1, -1, -1, W, E, N, S};
    return orient >= 0 && orient < 12 ? F[orient] : -1;
}

Catalog* buildCatalogs() {
    static Catalog cats[K_NUM];
    for (int k = 0; k < K_NUM; k++) {
        Catalog& c = cats[k];
        char houses[48], centres[48];
        snprintf(houses, sizeof(houses), "village/%s/houses/", KIND_NAME[k]);
        snprintf(centres, sizeof(centres), "village/%s/town_centers/", KIND_NAME[k]);
        size_t lh = strlen(houses), lc = strlen(centres);
        for (int i = 0; i < vanilla::TD_NUM_TEMPLATES; i++) {
            const char* n = vanilla::TD_STRINGS[vanilla::TD_TEMPLATES[i].name];
            if (strstr(n, "zombie")) continue;
            if (!strncmp(n, centres, lc) && c.nCentres < 8) c.centres[c.nCentres++] = (int16_t)i;
            if (strncmp(n, houses, lh) || c.nHouses >= 64) continue;
            const vanilla::TdTemplate& t = vanilla::TD_TEMPLATES[i];
            for (int j = 0; j < t.jigCount; j++) {
                const vanilla::TdJigsaw& jg = vanilla::TD_JIGSAWS[t.jigFirst + j];
                if (strcmp(vanilla::TD_STRINGS[jg.name], "minecraft:building_entrance")) continue;
                int f = frontOf(jg.orient);
                if (f < 0) break;
                uint8_t w = strstr(n, "small_house") ? 4 : strstr(n, "temple") || strstr(n, "large_farm") ? 1 : 2;
                c.houses[c.nHouses++] = {(int16_t)i, (int8_t)jg.x, (int8_t)jg.y, (int8_t)jg.z, (uint8_t)f, w};
                c.totalWeight += w;
                break;
            }
        }
    }
    return cats;
}

// the templates of each kind, listed once (C++11's static initialisation: safe on every worker)
const Catalog& catalog(int kind) {
    static Catalog* const cats = buildCatalogs();
    return cats[kind];
}

int kindOf(uint8_t b) {
    switch (b) {
        case biome::Plains: case biome::SunflowerPlains: return K_PLAINS;
        case biome::Desert: return K_DESERT;
        case biome::Savanna: return K_SAVANNA;
        case biome::Taiga: return K_TAIGA;
        case biome::SnowyPlains: case biome::SnowyTaiga: return K_SNOWY;
        default: return -1;
    }
}

struct Builder {
    const Generator& g;
    Rng& r;
    int kind;
    Piece* out;
    int n = 0, max;
    SBox taken[256];   // the boxes houses, roads and the centre take (no overlap)
    int nTaken = 0;

    int ground(int x, int z) const { return g.column(x, z).height; }
    bool free(const SBox& b) const {
        for (int i = 0; i < nTaken; i++)
            if (taken[i].hits(b)) return false;
        return true;
    }
    void take(const SBox& b) {
        if (nTaken < 256) taken[nTaken++] = b;
    }
    Piece* add() { return n < max ? &out[n++] : nullptr; }

    // a template at its origin, turned (PK_TEMPLATE, style 8; bit 30: a sandstone foundation)
    bool templateAt(int tmpl, int x, int y, int z, int rot) {
        Piece* p = add();
        if (!p) return false;
        *p = {};
        p->box = vanilla::sb::templateBox(tmpl, x, y, z, rot, 0);
        p->kind = vanilla::sb::PK_TEMPLATE;
        p->orient = (uint8_t)rot;
        p->a = (int16_t)tmpl;
        p->b = (int32_t)(((uint32_t)(x - p->box.x0) & 0xFFu) | ((uint32_t)(z - p->box.z0) & 0xFFu) << 8 | (uint32_t)(rot & 3) << 16 |
                         8u << 18 | 100u << 22 | (kind == K_DESERT ? 1u << 30 : 0));
        return true;
    }

    // a road from (x, z) in direction d, len blocks, 3 wide; houses along it
    void road(int x, int z, int d, int len, int depth) {
        int x1 = x + SX[d] * (len - 1), z1 = z + SZ[d] * (len - 1);
        int px = SZ[d] != 0 ? 1 : 0, pz = SX[d] != 0 ? 1 : 0;   // across the road
        SBox box = {x < x1 ? x : x1, 0, z < z1 ? z : z1, x > x1 ? x : x1, 0, z > z1 ? z : z1};
        box.x0 -= px; box.x1 += px; box.z0 -= pz; box.z1 += pz;
        if (depth > 0 && !free(SBox{box.x0, -4096, box.z0, box.x1, 4096, box.z1})) return;   // a branch through a house
        Piece* p = add();
        if (!p) return;
        *p = {};
        p->box = {box.x0, -64, box.z0, box.x1, 320, box.z1};
        p->kind = CP_ROAD;
        p->a = 3;
        p->b = kind == K_DESERT ? 1 : 0;
        take(SBox{box.x0, -4096, box.z0, box.x1, 4096, box.z1});
        // houses on both sides (most of the time), every 7 to 11 blocks; now and then a lamp post
        for (int at = 3; at < len - 2; at += 7 + r.range(5)) {
            int cx = x + SX[d] * at, cz = z + SZ[d] * at;
            for (int side = -1; side <= 1; side += 2)
                if (r.range(10) < 7) house(cx + px * side * 2, cz + pz * side * 2, d, side);
            if (r.range(3) == 0) {
                Piece* l = add();
                if (l) {
                    *l = {};
                    int lx = cx + SX[d] * 3 + px * 2, lz = cz + SZ[d] * 3 + pz * 2;
                    l->box = {lx, -64, lz, lx, 320, lz};
                    l->kind = CP_LAMP;
                }
            }
        }
        // a branch: at the end or the middle, turned left or right
        if (depth < 2 && r.range(3) != 0) {
            int from = r.range(2) ? len - 1 : len / 2;
            int nd = (d + (r.range(2) ? 1 : 3)) & 3;
            int bx = x + SX[d] * from + SX[nd] * 2, bz = z + SZ[d] * from + SZ[nd] * 2;
            road(bx, bz, nd, 14 + r.range(16), depth + 1);
        }
    }

    // a house beside the road at (x, z) (the cell next to the road), the road in direction d,
    // the house on side s (+1/-1 across)
    void house(int x, int z, int d, int s) {
        const Catalog& cat = catalog(kind);
        if (!cat.nHouses) return;
        int w = r.range(cat.totalWeight), i = 0;
        for (; i < cat.nHouses - 1; i++) {
            w -= cat.houses[i].weight;
            if (w < 0) break;
        }
        const Building& b = cat.houses[i];
        // the way from the house to the road: across, towards the road
        int px = SZ[d] != 0 ? 1 : 0, pz = SX[d] != 0 ? 1 : 0;
        int toRoad = px ? (s > 0 ? W : E) : (s > 0 ? N : S);
        int rot = (toRoad - b.front + 4) & 3;
        int ox, oz;
        vanilla::sb::templateTransform(b.jx, b.jz, 0, rot, 0, 0, ox, oz);
        int roadY = ground(x - px * s, z - pz * s);   // the road's block next to the door
        if (roadY < SEA_LEVEL) return;
        int X = x - ox, Z = z - oz, Y = roadY - b.jy;
        SBox box = vanilla::sb::templateBox(b.tmpl, X, Y, Z, rot, 0);
        SBox wide = {box.x0 - 1, -4096, box.z0 - 1, box.x1 + 1, 4096, box.z1 + 1};
        // free: the house itself (it starts right beside the road); taken: a block more around
        // it, so that the next house keeps a gap
        if (!free(SBox{box.x0, -4096, box.z0, box.x1, 4096, box.z1})) return;
        // the ground under it: no water, no cliff
        int lo = 1 << 20, hi = -(1 << 20);
        for (int k = 0; k < 9; k++) {
            int gx = box.x0 + (box.x1 - box.x0) * (k % 3) / 2, gz = box.z0 + (box.z1 - box.z0) * (k / 3) / 2;
            int h = ground(gx, gz);
            if (h < lo) lo = h;
            if (h > hi) hi = h;
        }
        if (lo < SEA_LEVEL || hi - roadY > 6 || roadY - lo > 6) return;
        if (templateAt(b.tmpl, X, Y, Z, rot)) take(wide);
    }
};

}  // namespace

int villagePieces(const Generator& g, uint64_t seed, int cx, int cz, Piece* out, int max) {
    int x = cx * 16 + 8, z = cz * 16 + 8;
    ColumnInfo mid = g.column(x, z);
    int kind = kindOf(mid.biome);
    if (kind < 0 || mid.height < SEA_LEVEL) return 0;
    // flat enough and dry around the meeting point
    int lo = mid.height, hi = mid.height;
    for (int dz = -24; dz <= 24; dz += 12)
        for (int dx = -24; dx <= 24; dx += 12) {
            int h = g.column(x + dx, z + dz).height;
            if (h < SEA_LEVEL) return 0;
            if (h < lo) lo = h;
            if (h > hi) hi = h;
        }
    if (hi - lo > 10) return 0;
    const Catalog& cat = catalog(kind);
    if (!cat.nCentres) return 0;
    Rng r(hash3(seed, cx, cz, 0x7111A6E));
    Builder b = {g, r, kind, out, 0, max};
    // the meeting point
    int tmpl = cat.centres[r.range(cat.nCentres)], rot = r.range(4);
    const vanilla::TdTemplate& t = vanilla::TD_TEMPLATES[tmpl];
    int ox, oz;
    vanilla::sb::templateTransform(t.sx / 2, t.sz / 2, 0, rot, 0, 0, ox, oz);
    int X = x - ox, Z = z - oz, Y = mid.height;
    if (!b.templateAt(tmpl, X, Y, Z, rot)) return 0;
    SBox centre = out[0].box;
    b.take(SBox{centre.x0 - 1, -4096, centre.z0 - 1, centre.x1 + 1, 4096, centre.z1 + 1});
    // roads from the middle of the centre's sides
    int dirs[4] = {N, E, S, W};
    for (int i = 3; i > 0; i--) {
        int j = r.range(i + 1), tmp = dirs[i];
        dirs[i] = dirs[j];
        dirs[j] = tmp;
    }
    int roads = 2 + r.range(3);
    int mx = (centre.x0 + centre.x1) / 2, mz = (centre.z0 + centre.z1) / 2;
    for (int k = 0; k < roads; k++) {
        int d = dirs[k];
        int sx = d == E ? centre.x1 + 2 : d == W ? centre.x0 - 2 : mx;
        int sz = d == S ? centre.z1 + 2 : d == N ? centre.z0 - 2 : mz;
        b.road(sx, sz, d, 24 + r.range(22), 0);
    }
    // villagers around the meeting point (one for about every two houses)
    int houses = 0;
    for (int i = 0; i < b.n; i++) houses += out[i].kind == vanilla::sb::PK_TEMPLATE;
    Piece* v = b.add();
    if (v) {
        *v = {};
        v->box = {mx - 6, Y + 1, mz - 6, mx + 6, Y + 1, mz + 6};
        v->kind = CP_VILLAGERS;
        v->a = (int16_t)(2 + houses / 2);
    }
    return b.n;
}

}  // namespace classic
}  // namespace mc
