// The End cities: vanilla's EndCityStructure and EndCityPieces (1.21.8): the house tower at
// the start, then towers, bridges, houses and fat towers grown by its four generators in
// vanilla's order of random numbers (a recursion's pieces kept only where they meet no
// piece of another depth), and the ship at the end of a bridge; each piece a template
// placed from its parent's (StructureTemplate#calculateConnectedPosition). Chests get the
// city's loot, sentries are shulkers.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "mc/platform.h"
#include "mc/world/vanilla/structure_build.h"
#include "mc/world/vanilla/structures.h"
#include "mc/world/vanilla/templates.h"

namespace mc {
namespace vanilla {
namespace sb {

namespace {

struct CityPiece { int tmpl; int x, y, z; int rot; bool overwrite; SBox box; int depth; };
typedef std::vector<CityPiece> Pieces;

int templateOf(const char* name) {
    char full[48];
    snprintf(full, sizeof(full), "end_city/%s", name);
    for (int i = 0; i < TD_NUM_TEMPLATES; i++)
        if (!strcmp(TD_STRINGS[TD_TEMPLATES[i].name], full)) return i;
    return -1;
}

struct Gen {
    JavaRandom& r;
    bool shipCreated = false;
    explicit Gen(JavaRandom& rnd) : r(rnd) {}

    CityPiece make(const char* name, int x, int y, int z, int rot, bool overwrite) {
        CityPiece p;
        p.tmpl = templateOf(name);
        p.x = x;
        p.y = y;
        p.z = z;
        p.rot = rot & 3;
        p.overwrite = overwrite;
        p.box = p.tmpl >= 0 ? templateBox(p.tmpl, x, y, z, p.rot, 0) : SBox{x, y, z, x, y, z};
        p.depth = 0;
        return p;
    }
    // EndCityPieces#addPiece: placed from the parent (its rotation) at an offset
    CityPiece addPiece(const CityPiece& prev, int ox, int oy, int oz, const char* name, int rot, bool overwrite) {
        int dx, dz;
        templateTransform(ox, oz, 0, prev.rot, 0, 0, dx, dz);
        return make(name, prev.x + dx, prev.y + oy, prev.z + dz, rot, overwrite);
    }
    static CityPiece& add(Pieces& list, const CityPiece& p) {   // addHelper
        list.push_back(p);
        return list.back();
    }

    enum { TOWER, TOWER_BRIDGE, HOUSE_TOWER, FAT_TOWER };

    // EndCityPieces#recursiveChildren
    bool recurse(int generator, int depth, const CityPiece& parent, bool hasPos, int px, int py, int pz, Pieces& pieces) {
        if (depth > 8) return false;
        Pieces list;
        if (!generate(generator, depth, parent, hasPos, px, py, pz, list)) return false;
        int i = r.nextInt();
        for (CityPiece& p : list) {
            p.depth = i;
            // StructurePiece#findCollisionPiece: the first piece it meets
            for (const CityPiece& q : pieces)
                if (q.box.hits(p.box)) {
                    if (q.depth != parent.depth) return false;
                    break;
                }
        }
        pieces.insert(pieces.end(), list.begin(), list.end());
        return true;
    }

    bool generate(int generator, int depth, const CityPiece& parent, bool hasPos, int px, int py, int pz, Pieces& pieces) {
        const int rot = parent.rot;
        switch (generator) {
            case TOWER: {
                int ox = 3 + r.nextInt(2);   // in Java's order: C++ leaves the order of arguments open
                int oz = 3 + r.nextInt(2);
                CityPiece p = add(pieces, addPiece(parent, ox, -3, oz, "tower_base", rot, true));
                p = add(pieces, addPiece(p, 0, 7, 0, "tower_piece", rot, true));
                CityPiece floor = p;
                bool hasFloor = r.nextInt(3) == 0;
                int n = 1 + r.nextInt(3);
                for (int j = 0; j < n; j++) {
                    p = add(pieces, addPiece(p, 0, 4, 0, "tower_piece", rot, true));
                    if (j < n - 1 && r.nextBoolean()) {
                        floor = p;
                        hasFloor = true;
                    }
                }
                if (hasFloor) {
                    // TOWER_BRIDGES: a bridge at each side at 50 %
                    static const int B[4][4] = {{0, 1, -1, 0}, {1, 6, -1, 1}, {3, 0, -1, 5}, {2, 5, -1, 6}};
                    for (const auto& t : B)
                        if (r.nextBoolean()) {
                            CityPiece b = add(pieces, addPiece(floor, t[1], t[2], t[3], "bridge_end", (rot + t[0]) & 3, true));
                            recurse(TOWER_BRIDGE, depth + 1, b, false, 0, 0, 0, pieces);
                        }
                    add(pieces, addPiece(p, -1, 4, -1, "tower_top", rot, true));
                } else {
                    if (depth != 7) return recurse(FAT_TOWER, depth + 1, p, false, 0, 0, 0, pieces);
                    add(pieces, addPiece(p, -1, 4, -1, "tower_top", rot, true));
                }
                return true;
            }
            case TOWER_BRIDGE: {
                int n = r.nextInt(4) + 1;
                add(pieces, addPiece(parent, 0, 0, -4, "bridge_piece", rot, true)).depth = -1;
                CityPiece last = pieces.back();
                int j = 0;
                for (int k = 0; k < n; k++) {
                    if (r.nextBoolean()) {
                        last = add(pieces, addPiece(last, 0, j, -4, "bridge_piece", rot, true));
                        j = 0;
                    } else {
                        if (r.nextBoolean()) last = add(pieces, addPiece(last, 0, j, -4, "bridge_steep_stairs", rot, true));
                        else last = add(pieces, addPiece(last, 0, j, -8, "bridge_gentle_stairs", rot, true));
                        j = 4;
                    }
                }
                if (!shipCreated && r.nextInt(10 - depth) == 0) {
                    int sx = -8 + r.nextInt(8), sz = -70 + r.nextInt(10);
                    add(pieces, addPiece(last, sx, j, sz, "ship", rot, true));
                    shipCreated = true;
                } else if (!recurse(HOUSE_TOWER, depth + 1, last, true, -3, j + 1, -11, pieces)) {
                    return false;
                }
                CityPiece& e = add(pieces, addPiece(last, 4, j, 0, "bridge_end", (rot + 2) & 3, true));
                e.depth = -1;
                return true;
            }
            case HOUSE_TOWER: {
                if (depth > 8) return false;
                CityPiece p = add(pieces, addPiece(parent, hasPos ? px : 0, hasPos ? py : 0, hasPos ? pz : 0, "base_floor", rot, true));
                int i = r.nextInt(3);
                if (i == 0) {
                    add(pieces, addPiece(p, -1, 4, -1, "base_roof", rot, true));
                } else if (i == 1) {
                    p = add(pieces, addPiece(p, -1, 0, -1, "second_floor_2", rot, false));
                    p = add(pieces, addPiece(p, -1, 8, -1, "second_roof", rot, false));
                    recurse(TOWER, depth + 1, p, false, 0, 0, 0, pieces);
                } else {
                    p = add(pieces, addPiece(p, -1, 0, -1, "second_floor_2", rot, false));
                    p = add(pieces, addPiece(p, -1, 4, -1, "third_floor_2", rot, false));
                    p = add(pieces, addPiece(p, -1, 8, -1, "third_roof", rot, true));
                    recurse(TOWER, depth + 1, p, false, 0, 0, 0, pieces);
                }
                return true;
            }
            default: {   // FAT_TOWER
                CityPiece p = add(pieces, addPiece(parent, -3, 4, -3, "fat_tower_base", rot, true));
                p = add(pieces, addPiece(p, 0, 4, 0, "fat_tower_middle", rot, true));
                static const int B[4][4] = {{0, 4, -1, 0}, {1, 12, -1, 4}, {3, 0, -1, 8}, {2, 8, -1, 12}};
                for (int i = 0; i < 2 && r.nextInt(3) != 0; i++) {
                    p = add(pieces, addPiece(p, 0, 8, 0, "fat_tower_middle", rot, true));
                    for (const auto& t : B)
                        if (r.nextBoolean()) {
                            CityPiece b = add(pieces, addPiece(p, t[1], t[2], t[3], "bridge_end", (rot + t[0]) & 3, true));
                            recurse(TOWER_BRIDGE, depth + 1, b, false, 0, 0, 0, pieces);
                        }
                }
                add(pieces, addPiece(p, -2, 8, -2, "fat_tower_top", rot, true));
                return true;
            }
        }
    }
};

}  // namespace

int endCityPieces(JavaRandom& r, int cx, int cz, TerrainProbe& probe, const uint8_t* biomes, Piece* out, int max) {
    // EndCityStructure#findGenerationPoint: the lowest of four corners 5 blocks out (by the
    // rotation) from the chunk's (7, 7); none below y 60
    int rot = r.nextInt(4);
    int w = 5, d = 5;
    if (rot == 1) w = -5;
    else if (rot == 2) { w = -5; d = -5; }
    else if (rot == 3) d = -5;
    int x = cx * 16 + 7, z = cz * 16 + 7;
    int y = probe.height(x, z, false);
    int h;
    if ((h = probe.height(x, z + d, false)) < y) y = h;
    if ((h = probe.height(x + w, z, false)) < y) y = h;
    if ((h = probe.height(x + w, z + d, false)) < y) y = h;
    y -= 1;   // getFirstOccupiedHeight
    if (y < 60) return 0;
    uint8_t b = probe.biome(x, y, z);
    if (b >= 128 || !(biomes[b >> 3] >> (b & 7) & 1)) return 0;
    // EndCityPieces#startHouseTower
    Gen g(r);
    Pieces pieces;
    CityPiece p = Gen::add(pieces, g.make("base_floor", x, y, z, rot, true));
    p = Gen::add(pieces, g.addPiece(p, -1, 0, -1, "second_floor_1", rot, false));
    p = Gen::add(pieces, g.addPiece(p, -1, 4, -1, "third_floor_1", rot, false));
    p = Gen::add(pieces, g.addPiece(p, -1, 8, -1, "third_roof", rot, true));
    g.recurse(Gen::TOWER, 1, p, false, 0, 0, 0, pieces);
    int n = 0;
    for (const CityPiece& c : pieces) {
        if (c.tmpl < 0 || n >= max) continue;
        Piece& o = out[n++];
        o = {};
        o.box = c.box;
        o.kind = PK_TEMPLATE;
        o.orient = (uint8_t)c.rot;
        o.a = (int16_t)c.tmpl;
        o.b = (int32_t)(((uint32_t)(c.x - c.box.x0) & 0xFFu) | ((uint32_t)(c.z - c.box.z0) & 0xFFu) << 8 | (uint32_t)c.rot << 16 |
                        6u << 18 | (c.overwrite ? 1u << 29 : 0));
    }
    return n;
}

}  // namespace sb
}  // namespace vanilla
}  // namespace mc
