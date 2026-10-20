// The single-template structures: igloos (IglooPieces), shipwrecks (ShipwreckPieces),
// ocean ruins (OceanRuinPieces) and Nether fossils (NetherFossilPieces): which templates,
// where and how turned, in vanilla's order of random numbers; their heights from the
// terrain probe (vanilla reads them from the chunk being decorated, which can differ from
// chunk to chunk: here every chunk of a structure agrees); their data markers' chests,
// loot and mobs.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc/platform.h"
#include "mc/world/vanilla/structure_build.h"
#include "mc/world/vanilla/structures.h"
#include "mc/world/vanilla/templates.h"

namespace mc {
namespace vanilla {
namespace sb {

int templateByName(const char* name) {
    for (int i = 0; i < TD_NUM_TEMPLATES; i++)
        if (!strcmp(TD_STRINGS[TD_TEMPLATES[i].name], name)) return i;
    return -1;
}

namespace {

// a piece: the template at its origin, turned (b: origin's offset in its box, rotation,
// style, integrity in percent, a flag)
Piece templatePiece(int tmpl, int x, int y, int z, int rot, int pivotX, int pivotZ, int style, int integrity, bool flag) {
    Piece p = {};
    p.box = templateBox(tmpl, x, y, z, rot, 0, pivotX, pivotZ);
    p.kind = PK_TEMPLATE;
    p.orient = (uint8_t)rot;
    p.a = (int16_t)tmpl;
    p.b = (int32_t)(((uint32_t)(x - p.box.x0) & 0xFFu) | ((uint32_t)(z - p.box.z0) & 0xFFu) << 8 | (uint32_t)(rot & 3) << 16 | (uint32_t)(style & 15) << 18 |
                    (uint32_t)(integrity & 127) << 22 | (flag ? 1u << 29 : 0));
    return p;
}

// IglooPieces' pivots and offsets per template
struct IglooPart { const char* name; int px, pz, ox, oy, oz; };
const IglooPart IGLOO[3] = {{"igloo/top", 3, 5, 0, 0, 0}, {"igloo/middle", 1, 1, 2, -3, 4}, {"igloo/bottom", 3, 7, 0, -3, -2}};
const IglooPart* iglooPart(int tmpl) {
    for (const IglooPart& p : IGLOO)
        if (!strcmp(TD_STRINGS[TD_TEMPLATES[tmpl].name], p.name)) return &p;
    return nullptr;
}

const char* const SHIP_BEACHED[11] = {"shipwreck/with_mast", "shipwreck/sideways_full", "shipwreck/sideways_fronthalf",
    "shipwreck/sideways_backhalf", "shipwreck/rightsideup_full", "shipwreck/rightsideup_fronthalf", "shipwreck/rightsideup_backhalf",
    "shipwreck/with_mast_degraded", "shipwreck/rightsideup_full_degraded", "shipwreck/rightsideup_fronthalf_degraded",
    "shipwreck/rightsideup_backhalf_degraded"};
const char* const SHIP_OCEAN[20] = {"shipwreck/with_mast", "shipwreck/upsidedown_full", "shipwreck/upsidedown_fronthalf",
    "shipwreck/upsidedown_backhalf", "shipwreck/sideways_full", "shipwreck/sideways_fronthalf", "shipwreck/sideways_backhalf",
    "shipwreck/rightsideup_full", "shipwreck/rightsideup_fronthalf", "shipwreck/rightsideup_backhalf", "shipwreck/with_mast_degraded",
    "shipwreck/upsidedown_full_degraded", "shipwreck/upsidedown_fronthalf_degraded", "shipwreck/upsidedown_backhalf_degraded",
    "shipwreck/sideways_full_degraded", "shipwreck/sideways_fronthalf_degraded", "shipwreck/sideways_backhalf_degraded",
    "shipwreck/rightsideup_full_degraded", "shipwreck/rightsideup_fronthalf_degraded", "shipwreck/rightsideup_backhalf_degraded"};

// the ground over a footprint (x0..x1, z0..z1) on a grid: the lowest and the average
void groundOver(TerrainProbe& probe, int x0, int z0, int x1, int z1, bool oceanFloor, int& lowest, int& average) {
    int lo = 1 << 30, sum = 0, n = 0;
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 4; k++) {
            int h = probe.height(x0 + (x1 - x0) * i / 3, z0 + (z1 - z0) * k / 3, oceanFloor);
            sum += h;
            n++;
            if (h < lo) lo = h;
        }
    lowest = lo;
    average = sum / n;
}

}  // namespace

// ------------------------------------------------------------------ igloos
int iglooPieces(JavaRandom& r, int cx, int cz, TerrainProbe& probe, Piece* out, int max) {
    int x = cx * 16, z = cz * 16;
    int rot = r.nextInt(4);   // Rotation.getRandom
    int n = 0;
    int depth = -1;
    if (r.nextDouble() < 0.5) depth = r.nextInt(8) + 4;   // the basement and its ladder
    // the height (IglooPiece#postProcess): the surface at the top's door column
    const IglooPart& top = IGLOO[0];
    int bx, bz;
    templateTransform(3, 0, 0, rot, top.px, top.pz, bx, bz);
    int lift = probe.height(x + bx, z + bz, false) - 90 - 1;
    auto add = [&](const IglooPart& part, int down) {
        int t = templateByName(part.name);
        if (t < 0 || n >= max) return;
        out[n++] = templatePiece(t, x + part.ox, 90 + part.oy - down + lift, z + part.oz, rot, part.px, part.pz, 1, 100, false);
    };
    if (depth > 0) {
        add(IGLOO[2], depth * 3);
        for (int j = 0; j < depth - 1; j++) add(IGLOO[1], j * 3);
    }
    add(IGLOO[0], 0);
    return n;
}

// ------------------------------------------------------------------ shipwrecks
int shipwreckPieces(JavaRandom& r, int cx, int cz, bool beached, TerrainProbe& probe, Piece* out, int max) {
    if (max < 1) return 0;
    int rot = r.nextInt(4);
    const char* name = beached ? SHIP_BEACHED[r.nextInt(11)] : SHIP_OCEAN[r.nextInt(20)];
    int t = templateByName(name);
    if (t < 0) return 0;
    const TdTemplate& tp = TD_TEMPLATES[t];
    int x = cx * 16, z = cz * 16;
    // ShipwreckPiece#postProcess: the footprint's lowest and average ground (unturned)
    int lowest, average;
    groundOver(probe, x, z, x + tp.sx - 1, z + tp.sz - 1, !beached, lowest, average);
    int y = beached ? lowest - tp.sy / 2 - r.nextInt(3) : average;   // calculateBeachedPosition
    out[0] = templatePiece(t, x, y, z, rot, 4, 15, 2, 100, beached);
    return 1;
}

// ------------------------------------------------------------------ ocean ruins
namespace {
const char* const WARM[8] = {"warm_1", "warm_2", "warm_3", "warm_4", "warm_5", "warm_6", "warm_7", "warm_8"};
const char* const BIG_WARM[4] = {"big_warm_4", "big_warm_5", "big_warm_6", "big_warm_7"};
const char* const BRICK[8] = {"brick_1", "brick_2", "brick_3", "brick_4", "brick_5", "brick_6", "brick_7", "brick_8"};
const char* const CRACKED[8] = {"cracked_1", "cracked_2", "cracked_3", "cracked_4", "cracked_5", "cracked_6", "cracked_7", "cracked_8"};
const char* const MOSSY[8] = {"mossy_1", "mossy_2", "mossy_3", "mossy_4", "mossy_5", "mossy_6", "mossy_7", "mossy_8"};
const char* const BIG_BRICK[4] = {"big_brick_1", "big_brick_2", "big_brick_3", "big_brick_8"};
const char* const BIG_CRACKED[4] = {"big_cracked_1", "big_cracked_2", "big_cracked_3", "big_cracked_8"};
const char* const BIG_MOSSY[4] = {"big_mossy_1", "big_mossy_2", "big_mossy_3", "big_mossy_8"};

int ruinTemplate(const char* name) {
    char full[48];
    snprintf(full, sizeof(full), "underwater_ruin/%s", name);
    return templateByName(full);
}
int nextIntBetween(JavaRandom& r, int lo, int hi) { return r.nextInt(hi - lo + 1) + lo; }   // Mth.nextInt

struct Ruins {
    JavaRandom& r;
    TerrainProbe& probe;
    bool warm;
    Piece* out;
    int n, max;
    // OceanRuinPiece#postProcess's height: the ocean floor at the origin, lowered to the
    // lowest ground of the footprint if most of it lies lower
    void piece(const char* name, int x, int z, int rot, int integrity, bool large) {
        int t = ruinTemplate(name);
        if (t < 0 || n >= max) return;
        const TdTemplate& tp = TD_TEMPLATES[t];
        int y = probe.height(x, z, true);
        int cxx, czz;
        templateTransform(tp.sx - 1, tp.sz - 1, 0, rot, 0, 0, cxx, czz);
        int x1 = x + cxx, z1 = z + czz;
        int lowest = 512, below = 0, samples = 0;
        for (int i = 0; i < 4; i++)
            for (int k = 0; k < 4; k++) {
                int g = probe.height(x + (x1 - x) * i / 3, z + (z1 - z) * k / 3, true) - 1;   // the ground under the water
                if (g < lowest) lowest = g;
                if (g < y - 1 - 2) below++;
                samples++;
            }
        int span = abs(x - x1);
        if (y - 1 - lowest > 2 && below * (span + 1) / samples > span - 2) y = lowest + 1;
        out[n++] = templatePiece(t, x, y, z, rot, 0, 0, 3, integrity, large);
    }
    // OceanRuinPieces#addPiece
    void add(int x, int z, int rot, bool large, int integrity) {
        if (warm) {
            piece(large ? BIG_WARM[r.nextInt(4)] : WARM[r.nextInt(8)], x, z, rot, integrity, large);
        } else {
            int i = r.nextInt(large ? 4 : 8);
            piece(large ? BIG_BRICK[i] : BRICK[i], x, z, rot, integrity, large);
            piece(large ? BIG_CRACKED[i] : CRACKED[i], x, z, rot, 70, large);
            piece(large ? BIG_MOSSY[i] : MOSSY[i], x, z, rot, 50, large);
        }
    }
};
}  // namespace

int oceanRuinPieces(JavaRandom& r, int cx, int cz, bool warm, TerrainProbe& probe, Piece* out, int max) {
    Ruins ru = {r, probe, warm, out, 0, max};
    int x = cx * 16, z = cz * 16;
    int rot = r.nextInt(4);
    bool large = r.nextFloat() <= 0.3f;   // OceanRuinStructure's largeProbability
    ru.add(x, z, rot, large, large ? 90 : 80);
    if (large && r.nextFloat() <= 0.9f) {   // clusterProbability: small ruins around it
        int px2, pz2;
        templateTransform(15, 15, 0, rot, 0, 0, px2, pz2);
        SBox box = {x < x + px2 ? x : x + px2, 90, z < z + pz2 ? z : z + pz2, x > x + px2 ? x : x + px2, 90, z > z + pz2 ? z : z + pz2};
        int bx = box.x0, bz = box.z0;
        int list[8][2] = {
            {bx - 16 + nextIntBetween(r, 1, 8), bz + 16 + nextIntBetween(r, 1, 7)},
            {bx - 16 + nextIntBetween(r, 1, 8), bz + nextIntBetween(r, 1, 7)},
            {bx - 16 + nextIntBetween(r, 1, 8), bz - 16 + nextIntBetween(r, 4, 8)},
            {bx + nextIntBetween(r, 1, 7), bz + 16 + nextIntBetween(r, 1, 7)},
            {bx + nextIntBetween(r, 1, 7), bz - 16 + nextIntBetween(r, 4, 6)},
            {bx + 16 + nextIntBetween(r, 1, 7), bz + 16 + nextIntBetween(r, 3, 8)},
            {bx + 16 + nextIntBetween(r, 1, 7), bz + nextIntBetween(r, 1, 7)},
            {bx + 16 + nextIntBetween(r, 1, 7), bz - 16 + nextIntBetween(r, 4, 8)},
        };
        int left = 8;
        int k = nextIntBetween(r, 4, 8);
        for (int j = 0; j < k && left > 0; j++) {
            int pick = r.nextInt(left);
            int qx = list[pick][0], qz = list[pick][1];
            for (int m = pick; m < left - 1; m++) {
                list[m][0] = list[m + 1][0];
                list[m][1] = list[m + 1][1];
            }
            left--;
            int rr = r.nextInt(4);
            int ex, ez;
            templateTransform(5, 6, 0, rr, 0, 0, ex, ez);
            SBox b2 = {qx < qx + ex ? qx : qx + ex, 90, qz < qz + ez ? qz : qz + ez, qx > qx + ex ? qx : qx + ex, 90, qz > qz + ez ? qz : qz + ez};
            if (!b2.hits(box)) ru.add(qx, qz, rr, false, 80);
        }
    }
    return ru.n;
}

// ------------------------------------------------------------------ Nether fossils
int netherFossilPieces(JavaRandom& r, int cx, int cz, TerrainProbe& probe, const uint8_t* biomes, Piece* out, int max) {
    // NetherFossilStructure#findGenerationPoint: a random column, a random height (32 to 2
    // below the top), down to air over ground above the lava sea
    int x = cx * 16 + r.nextInt(16), z = cz * 16 + r.nextInt(16);
    int y = 32 + r.nextInt(125 - 32 + 1);
    const int sea = 32;
    while (y > sea) {
        if (!probe.solid(x, y, z) && probe.solid(x, y - 1, z)) break;
        y--;
    }
    if (y <= sea || max < 1) return 0;
    uint8_t b = probe.biome(x, y, z);
    if (b >= 128 || !(biomes[b >> 3] >> (b & 7) & 1)) return 0;
    int rot = r.nextInt(4);
    char name[40];
    snprintf(name, sizeof(name), "nether_fossils/fossil_%d", (int)(r.nextInt(14) + 1));
    int t = templateByName(name);
    if (t < 0) return 0;
    out[0] = templatePiece(t, x, y, z, rot, 0, 0, 4, 100, false);
    return 1;
}

// ------------------------------------------------------------------ their blocks
namespace {
struct MarkerCtx { int style; bool flag; int64_t seed; int rot; };
void onMarker(void* ctx, Chunk& c, const SBox& clip, const char* name, int x, int y, int z) {
    MarkerCtx& m = *(MarkerCtx*)ctx;
    int lx = x & 15, lz = z & 15;
    const char* table = nullptr;
    if (m.style == 6) {   // an End city: its treasure chests, its sentries (shulkers)
        if (!strcmp(name, "Chest")) {
            table = "end_city_treasure";
        } else if (!strcmp(name, "Sentry")) {
            int type = findEntityType("shulker");
            if (type >= 0) {
                SavedEntity e;
                e.kind = 3;
                e.type = (uint16_t)type;
                e.x = x + 0.5;
                e.y = y;
                e.z = z + 0.5;
                e.health = 30;
                c.addEntity(e);
            }
            return;
        } else if (!strcmp(name, "Elytra")) {   // the ship's elytra in its item frame, facing south as the ship turns
            int type = findEntityType("item_frame"), elytra = findItem("elytra");
            if (type < 0 || elytra < 0) return;
            static const int8_t STEP[4][2] = {{0, 1}, {-1, 0}, {0, -1}, {1, 0}};   // south turned 0..3 times clockwise
            static const uint8_t DIR3D[4] = {3, 4, 2, 5};                          // south, west, north, east
            int rot = m.rot & 3;
            SavedEntity e;
            e.kind = SAVED_KIND_FRAME;
            e.type = (uint16_t)type;
            e.variant = DIR3D[rot];
            e.x = x + 0.5 - STEP[rot][0] * 0.46875;   // HangingEntity: against the block behind it
            e.y = y + 0.5;
            e.z = z + 0.5 - STEP[rot][1] * 0.46875;
            e.health = 1;
            e.item.id = (uint16_t)elytra;
            e.item.count = 1;
            c.addEntity(e);
            return;
        } else {
            return;
        }
    } else if (m.style == 7) {   // a woodland mansion: its chests (turned with the piece), its illagers and allays
        if (!strncmp(name, "Chest", 5)) {
            if (!clip.inside(x, y, z) || blockIdOf(c.get(lx, y, lz)) == blockIdOf(B[B_CHEST])) return;
            uint16_t st = B[B_CHEST];
            static const char* const SIDES[4] = {"ChestNorth", "ChestEast", "ChestSouth", "ChestWest"};
            for (int d = 0; d < 4; d++)
                if (!strcmp(name, SIDES[d])) st = facing(st, (d + m.rot) & 3);
            c.set(lx, y, lz, st);
            TileEntity* t = c.addTile(TILE_CHEST, lx, y, lz);
            fillLoot(t, "woodland_mansion", m.seed ^ positionSeed(x, y, z));
            return;
        }
        const char* mob = !strcmp(name, "Mage") ? "evoker" : !strcmp(name, "Warrior") ? "vindicator" : !strcmp(name, "Group of Allays") ? "allay" : nullptr;
        if (!mob) return;
        int type = findEntityType(mob);
        // a group of allays: 1 to 3 (vanilla draws from the level's random)
        int count = mob[0] == 'a' ? 1 + (int)(posRandom(m.seed, x, y, z, 7) * 3) : 1;
        for (int i = 0; type >= 0 && i < count; i++) {
            SavedEntity e;
            e.kind = 3;
            e.type = (uint16_t)type;
            e.x = x + 0.5;
            e.y = y;
            e.z = z + 0.5;
            e.health = mob[0] == 'a' ? 20 : 24;
            c.addEntity(e);
        }
        c.set(lx, y, lz, 0);
        return;
    } else if (m.style == 1 && !strcmp(name, "chest")) {   // the igloo's basement: the chest below
        c.set(lx, y, lz, 0);
        table = "igloo_chest";
    } else if (m.style == 2) {   // a shipwreck's chests
        table = !strcmp(name, "map_chest") ? "shipwreck_map" : !strcmp(name, "treasure_chest") ? "shipwreck_treasure"
                : !strcmp(name, "supply_chest") ? "shipwreck_supply" : nullptr;
    } else if (m.style == 3) {
        if (!strcmp(name, "chest")) {   // an ocean ruin's chest, waterlogged in water
            bool water = stateIsFluid(c.get(lx, y, lz));
            c.set(lx, y, lz, setBool(B[B_CHEST], "waterlogged", water));
            TileEntity* t = c.addTile(TILE_CHEST, lx, y, lz);
            fillLoot(t, m.flag ? "underwater_ruin_big" : "underwater_ruin_small", m.seed ^ positionSeed(x, y, z));
        } else if (!strcmp(name, "drowned")) {   // a drowned waiting with the chunk
            int type = findEntityType("drowned");
            if (type >= 0) {
                SavedEntity e;
                e.kind = 3;
                e.type = (uint16_t)type;
                e.x = x + 0.5;
                e.y = y;
                e.z = z + 0.5;
                e.health = 20;
                c.addEntity(e);
            }
            c.set(lx, y, lz, y > SEA_LEVEL - 1 ? 0 : B[B_WATER]);
        }
        return;
    }
    if (!table || !c.hasY(y - 1)) return;
    if (blockIdOf(c.get(lx, y - 1, lz)) != blockIdOf(B[B_CHEST])) return;
    TileEntity* t = c.addTile(TILE_CHEST, lx, y - 1, lz);
    fillLoot(t, table, m.seed ^ positionSeed(x, y - 1, z));
    (void)clip;
}
}  // namespace

void buildTemplatePiece(Chunk& c, const SBox& clip, const Piece& p, int64_t seed) {
    uint32_t b = (uint32_t)p.b;
    // the origin's offset from the box: signed (turned around a pivot it can lie outside)
    int x = p.box.x0 + (int8_t)(b & 0xFF), z = p.box.z0 + (int8_t)((b >> 8) & 0xFF), y = p.box.y0;
    int rot = (b >> 16) & 3, style = (b >> 18) & 15, integrity = (b >> 22) & 127;
    bool flag = (b >> 29) & 1;
    TemplatePlacement t = {p.a, x, y, z, (uint8_t)rot, (uint8_t)(b >> 30), 0, false, false, false, false, false};
    MarkerCtx mc = {style, flag, seed, rot};
    t.marker = onMarker;
    t.markerCtx = &mc;
    if (style == 1) {   // igloos: their air placed, no waterlogging, turned around their pivots
        const IglooPart* part = iglooPart(p.a);
        if (part) {
            t.pivotX = part->px;
            t.pivotZ = part->pz;
        }
        t.noWaterlogging = true;
    } else if (style == 6) {   // End city pieces: their air only where they overwrite
        t.legacy = !flag;
        t.ignoreEntities = true;
    } else if (style == 7) {   // woodland mansion pieces: their air placed, their mobs from the markers
        t.ignoreEntities = true;
    } else {   // shipwrecks, ocean ruins, Nether fossils: no air (STRUCTURE_AND_AIR)
        t.legacy = true;
        if (style == 2) {
            t.pivotX = 4;
            t.pivotZ = 15;
        }
        if (style == 3) t.integrity = integrity / 100.0f;
    }
    placeTemplate(c, clip, t, seed);
    if (style == 1 && t.pivotX == 3 && t.pivotZ == 5) {   // the top: snow under its entrance unless it leads down
        int ox, oz;
        templateTransform(3, 5, 0, rot, 3, 5, ox, oz);
        int X = x + ox, Z = z + oz;
        if (clip.inside(X, y, Z) && c.hasY(y - 1)) {
            uint16_t below = c.get(X & 15, y - 1, Z & 15);
            int snow = findBlock("snow_block");
            if (!stateIsAir(below) && blockIdOf(below) != blockIdOf(B[B_LADDER]) && snow >= 0) c.set(X & 15, y, Z & 15, BLOCKS[snow].defState);
        }
    }
}

}  // namespace sb
}  // namespace vanilla
}  // namespace mc
