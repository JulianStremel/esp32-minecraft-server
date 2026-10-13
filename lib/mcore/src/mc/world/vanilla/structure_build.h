// Internal to the structures (structures.cpp and the files with their pieces): boxes,
// pieces, the blocks they use and StructurePiece's placement into a chunk, clipped to it.
#pragma once
#include <stdint.h>
#include <string.h>
#include "mc/registry.h"
#include "mc/world/chunk.h"
#include "mc/world/vanilla/java_random.h"
#include "mc/world/vanilla/structures.h"

namespace mc {
namespace vanilla {
namespace sb {

enum Dir : uint8_t { NORTH, EAST, SOUTH, WEST, UNORIENTED };   // Direction.Plane.HORIZONTAL's order; a piece without one
static const char* const DIR_NAME[4] = {"north", "east", "south", "west"};
inline int dirOf(const char* s) {
    for (int i = 0; i < 4; i++)
        if (!strcmp(s, DIR_NAME[i])) return i;
    return -1;
}

struct SBox {
    int32_t x0, y0, z0, x1, y1, z1;
    bool hits(const SBox& o) const { return x0 <= o.x1 && x1 >= o.x0 && y0 <= o.y1 && y1 >= o.y0 && z0 <= o.z1 && z1 >= o.z0; }
    bool inside(int x, int y, int z) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1 && z >= z0 && z <= z1; }
    void moveY(int dy) { y0 += dy; y1 += dy; }
};
// StructurePiece.makeBoundingBox
inline SBox makeBox(int x, int y, int z, int dir, int w, int h, int d) {
    if (dir == NORTH || dir == SOUTH) return {x, y, z, x + w - 1, y + h - 1, z + d - 1};
    return {x, y, z, x + d - 1, y + h - 1, z + w - 1};
}

// BoundingBox.orientBox: a piece's box from the door it is entered by
inline SBox orientBox(int x, int y, int z, int ox, int oy, int oz, int w, int h, int d, int dir) {
    switch (dir) {
        case NORTH: return {x + ox, y + oy, z - d + 1 + oz, x + w - 1 + ox, y + h - 1 + oy, z + oz};
        case WEST: return {x - d + 1 + oz, y + oy, z + ox, x + oz, y + h - 1 + oy, z + w - 1 + ox};
        case EAST: return {x + oz, y + oy, z + ox, x + d - 1 + oz, y + h - 1 + oy, z + w - 1 + ox};
        default: return {x + ox, y + oy, z + oz, x + w - 1 + ox, y + h - 1 + oy, z + d - 1 + oz};   // SOUTH
    }
}

// ------------------------------------------------------------------ blocks
enum BlockName : uint8_t {
    B_AIR, B_SANDSTONE, B_CUT_SANDSTONE, B_CHISELED_SANDSTONE, B_SANDSTONE_STAIRS, B_SANDSTONE_SLAB, B_ORANGE_TERRACOTTA,
    B_BLUE_TERRACOTTA, B_TNT, B_STONE_PRESSURE_PLATE, B_CHEST, B_COBBLESTONE, B_MOSSY_COBBLESTONE, B_COBBLESTONE_STAIRS,
    B_CHISELED_STONE_BRICKS, B_TRIPWIRE_HOOK, B_TRIPWIRE, B_REDSTONE_WIRE, B_DISPENSER, B_VINE, B_LEVER, B_STICKY_PISTON,
    B_REPEATER, B_SPRUCE_PLANKS, B_OAK_LOG, B_OAK_FENCE, B_POTTED_RED_MUSHROOM, B_CRAFTING_TABLE, B_CAULDRON,
    B_SPRUCE_STAIRS, B_SAND, B_STONE, B_ANDESITE, B_GRANITE, B_DIORITE,
    // strongholds
    B_STONE_BRICKS, B_CRACKED_STONE_BRICKS, B_MOSSY_STONE_BRICKS, B_INFESTED_STONE_BRICKS, B_CAVE_AIR, B_WALL_TORCH,
    B_SMOOTH_STONE_SLAB, B_IRON_BARS, B_IRON_DOOR, B_OAK_DOOR, B_STONE_BUTTON, B_WATER, B_OAK_PLANKS, B_LADDER, B_COBWEB,
    B_BOOKSHELF, B_TORCH, B_LAVA, B_STONE_BRICK_STAIRS, B_END_PORTAL_FRAME, B_END_PORTAL, B_SPAWNER,
    // Nether fortresses
    B_NETHER_BRICKS, B_NETHER_BRICK_FENCE, B_NETHER_BRICK_STAIRS, B_SOUL_SAND, B_NETHER_WART,
    // mineshafts
    B_RAIL, B_DARK_OAK_PLANKS, B_DARK_OAK_FENCE, B_CHAIN, B_NUM
};
extern uint16_t B[B_NUM];   // resolved once (structuresInit)

inline uint16_t facing(uint16_t st, int dir) { return setPropStr(st, "facing", DIR_NAME[dir & 3]); }
// a block's own random number in [0, 1): what vanilla draws per block while placing (the
// same in every chunk the piece reaches, whatever order they come in)
inline float posRandom(int64_t seed, int x, int y, int z, uint32_t salt = 0) {
    uint64_t h = (uint64_t)seed ^ ((uint64_t)(uint32_t)x * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)(uint32_t)y * 0xC2B2AE3D27D4EB4Full) ^
                 ((uint64_t)(uint32_t)z * 0x165667B19E3779F9ull) ^ ((uint64_t)salt << 40);
    h = (h ^ (h >> 30)) * 0xBF58476D1CE4E5B9ull;
    h = (h ^ (h >> 27)) * 0x94D049BB133111EBull;
    h ^= h >> 31;
    return (float)(h >> 40) * (1.0f / 16777216.0f);
}

// ------------------------------------------------------------------ pieces
enum PieceKind : uint8_t {
    PK_DESERT_PYRAMID, PK_JUNGLE_TEMPLE, PK_SWAMP_HUT, PK_BURIED_TREASURE,
    // StrongholdPieces (stronghold.cpp)
    PK_SH_STRAIGHT, PK_SH_PRISON_HALL, PK_SH_LEFT_TURN, PK_SH_RIGHT_TURN, PK_SH_ROOM_CROSSING, PK_SH_STRAIGHT_STAIRS_DOWN,
    PK_SH_STAIRS_DOWN, PK_SH_FIVE_CROSSING, PK_SH_CHEST_CORRIDOR, PK_SH_LIBRARY, PK_SH_PORTAL_ROOM, PK_SH_FILLER_CORRIDOR,
    // NetherFortressPieces (fortress.cpp)
    PK_NF_BRIDGE_STRAIGHT, PK_NF_BRIDGE_END_FILLER, PK_NF_BRIDGE_CROSSING, PK_NF_ROOM_CROSSING, PK_NF_STAIRS_ROOM,
    PK_NF_MONSTER_THRONE, PK_NF_CASTLE_ENTRANCE, PK_NF_CORRIDOR, PK_NF_CORRIDOR_CROSSING, PK_NF_CORRIDOR_RIGHT_TURN,
    PK_NF_CORRIDOR_LEFT_TURN, PK_NF_CORRIDOR_STAIRS, PK_NF_CORRIDOR_T_BALCONY, PK_NF_STALK_ROOM,
    // MineshaftPieces (mineshaft.cpp)
    PK_MS_ROOM, PK_MS_ENTRANCE, PK_MS_CORRIDOR, PK_MS_CROSSING, PK_MS_STAIRS,
    // a jigsaw structure's piece (jigsaw.cpp): a: its pool element, b: its origin in its box
    // (x, z, y: a byte each) and its ground level delta (the top byte)
    PK_JIGSAW,
    // a jigsaw junction (where two pieces join; the Beardifier's): box: the point, its y the ground
    PK_JUNCTION,
    // a single template (template_structures.cpp): a: the template, b: its origin in its box,
    // rotation, style (1 igloo, 2 shipwreck, 3 ocean ruin, 4 Nether fossil), integrity, a flag
    PK_TEMPLATE,
    // a ruined portal's properties (follows its PK_TEMPLATE piece; no blocks of its own)
    PK_PORTAL_PROPS,
};

struct Piece {
    SBox box;
    uint8_t kind, orient;
    int16_t a;   // per kind (a stronghold piece's door and options)
    int32_t b;
};

void fillLoot(TileEntity* t, const char* table, int64_t seed);

// StructurePiece's coordinates, orientation and block placement, clipped to the chunk
struct Build {
    Chunk& c;
    SBox clip;
    const Piece& p;
    int64_t seed;
    bool mirror;
    int rot;
    uint32_t blocks = 0;
    uint32_t tfKey[32];
    uint16_t tfVal[32];

    Build(Chunk& ch, const Piece& piece, int64_t s) : c(ch), p(piece), seed(s) {
        clip = {ch.cx * 16, ch.minY(), ch.cz * 16, ch.cx * 16 + 15, ch.maxY(), ch.cz * 16 + 15};
        // StructurePiece#setOrientation
        mirror = piece.orient == SOUTH || piece.orient == WEST;
        rot = piece.orient == WEST || piece.orient == EAST ? 1 : 0;
        memset(tfKey, 0xFF, sizeof(tfKey));
    }
    int wx(int x, int z) const {
        switch (p.orient) {
            case WEST: return p.box.x1 - z;
            case EAST: return p.box.x0 + z;
            default: return p.box.x0 + x;
        }
    }
    int wy(int y) const { return p.box.y0 + y; }
    int wz(int x, int z) const {
        switch (p.orient) {
            case NORTH: return p.box.z1 - z;
            case SOUTH: case UNORIENTED: return p.box.z0 + z;
            default: return p.box.z0 + x;
        }
    }
    // a direction in the piece's frame as the world's
    int dirOut(int d) const {
        if (mirror && (d == NORTH || d == SOUTH)) d ^= 2;
        return (d + rot) & 3;
    }
    // BlockState#mirror(LEFT_RIGHT) then #rotate(CLOCKWISE_90): facing, axis, the side
    // properties, stairs' and doors' handedness
    uint16_t tf(uint16_t st) {
        if ((!mirror && !rot) || st == 0) return st;
        uint32_t key = st;
        int slot = st & 31;
        if (tfKey[slot] == key) return tfVal[slot];
        uint16_t out = st;
        uint16_t b = blockIdOf(st);
        if (propIndexOf(b, "facing") >= 0) {
            int d = dirOf(getPropStr(st, "facing"));
            if (d >= 0) out = setPropStr(out, "facing", DIR_NAME[dirOut(d)]);
        }
        if (rot && propIndexOf(b, "axis") >= 0) {
            const char* a = getPropStr(st, "axis");
            if (!strcmp(a, "x")) out = setPropStr(out, "axis", "z");
            else if (!strcmp(a, "z")) out = setPropStr(out, "axis", "x");
        }
        if (propIndexOf(b, "north") >= 0 && propIndexOf(b, "east") >= 0) {
            const char* v[4];
            for (int d = 0; d < 4; d++) v[d] = getPropStr(st, DIR_NAME[d]);
            for (int d = 0; d < 4; d++) out = setPropStr(out, DIR_NAME[dirOut(d)], v[d]);
        }
        if (rot && propIndexOf(b, "shape") >= 0) {   // rails
            const char* s = getPropStr(st, "shape");
            if (!strcmp(s, "north_south")) out = setPropStr(out, "shape", "east_west");
            else if (!strcmp(s, "east_west")) out = setPropStr(out, "shape", "north_south");
        }
        if (mirror) {
            static const char* const SWAP[][2] = {{"inner_left", "inner_right"}, {"outer_left", "outer_right"}};
            if (propIndexOf(b, "shape") >= 0) {
                const char* s = getPropStr(st, "shape");
                for (const auto& sw : SWAP) {
                    if (!strcmp(s, sw[0])) out = setPropStr(out, "shape", sw[1]);
                    else if (!strcmp(s, sw[1])) out = setPropStr(out, "shape", sw[0]);
                }
            }
            if (propIndexOf(b, "hinge") >= 0) out = setPropStr(out, "hinge", !strcmp(getPropStr(st, "hinge"), "left") ? "right" : "left");
        }
        tfKey[slot] = key;
        tfVal[slot] = out;
        return out;
    }
    uint16_t get(int x, int y, int z) const {
        int X = wx(x, z), Y = wy(y), Z = wz(x, z);
        if (!clip.inside(X, Y, Z)) return 0;
        return c.get(X & 15, Y, Z & 15);
    }
    bool inside(int x, int y, int z) const { return clip.inside(wx(x, z), wy(y), wz(x, z)); }
    void set(int x, int y, int z, uint16_t st) {
        int X = wx(x, z), Y = wy(y), Z = wz(x, z);
        if (!clip.inside(X, Y, Z)) return;
        c.set(X & 15, Y, Z & 15, tf(st));
        blocks++;
    }
    // StructurePiece#generateBox
    void box(int x0, int y0, int z0, int x1, int y1, int z1, uint16_t edge, uint16_t in, bool keepAir = false) {
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++)
                for (int z = z0; z <= z1; z++) {
                    if (keepAir && stateIsAir(get(x, y, z))) continue;
                    set(x, y, z, (y == y0 || y == y1 || x == x0 || x == x1 || z == z0 || z == z1) ? edge : in);
                }
    }
    // generateBox with a BlockSelector: cobblestone, mossy at random (draws for every block)
    void mossBox(JavaRandom& r, int x0, int y0, int z0, int x1, int y1, int z1, bool keepAir = false) {
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++)
                for (int z = z0; z <= z1; z++) {
                    uint16_t st = r.nextFloat() < 0.4f ? B[B_COBBLESTONE] : B[B_MOSSY_COBBLESTONE];
                    if (keepAir && stateIsAir(get(x, y, z))) continue;
                    set(x, y, z, st);
                }
    }
    void air(int x0, int y0, int z0, int x1, int y1, int z1) { box(x0, y0, z0, x1, y1, z1, 0, 0); }
    // StructurePiece#fillColumnDown: through air and fluids
    void columnDown(uint16_t st, int x, int y, int z) {
        int X = wx(x, z), Z = wz(x, z);
        if (!clip.inside(X, clip.y0 + 1, Z)) return;
        for (int Y = wy(y); Y > c.minY() + 1; Y--) {
            uint16_t at = c.get(X & 15, Y, Z & 15);
            if (!stateIsAir(at) && !stateIsFluid(at)) break;
            c.set(X & 15, Y, Z & 15, st);
            blocks++;
        }
    }
    bool solidAtWorld(int X, int Y, int Z) const { return clip.inside(X, Y, Z) && stateOpaque(c.get(X & 15, Y, Z & 15)); }
    // StructurePiece#createChest (reorient: away from the one solid side)
    void chest(int x, int y, int z, const char* table, int salt) {
        int X = wx(x, z), Y = wy(y), Z = wz(x, z);
        if (!clip.inside(X, Y, Z) || blockIdOf(c.get(X & 15, Y, Z & 15)) == blockIdOf(B[B_CHEST])) return;
        static const int DX[4] = {0, 1, 0, -1}, DZ[4] = {-1, 0, 1, 0};
        int solid = -1, count = 0;
        for (int d = 0; d < 4; d++)
            if (solidAtWorld(X + DX[d], Y, Z + DZ[d])) {
                solid = d;
                count++;
            }
        int f = count == 1 ? (solid ^ 2) : NORTH;
        if (count != 1) {
            for (int k = 0; k < 4 && solidAtWorld(X + DX[f], Y, Z + DZ[f]); k++) f = (f + 1) & 3;
        }
        c.set(X & 15, Y, Z & 15, facing(B[B_CHEST], f));
        blocks++;
        TileEntity* t = c.addTile(TILE_CHEST, X & 15, Y, Z & 15);
        fillLoot(t, table, seed ^ (int64_t)((uint64_t)(uint32_t)X * 0x9E3779B97F4A7C15ull) ^ (int64_t)((uint64_t)(int64_t)Y << 20) ^
                               (int64_t)((uint64_t)(uint32_t)Z * 0xC2B2AE3D27D4EB4Full) ^ salt);
    }
    // StructurePiece#createDispenser
    // a monster spawner and its mob (StructurePiece places the block, then sets the
    // spawner block entity's entity)
    void spawner(int x, int y, int z, const char* mob) {
        int X = wx(x, z), Y = wy(y), Z = wz(x, z);
        if (!clip.inside(X, Y, Z)) return;
        set(x, y, z, B[B_SPAWNER]);
        TileEntity* t = c.addTile(TILE_SPAWNER, X & 15, Y, Z & 15);
        int type = findEntityType(mob);
        if (t && type >= 0) t->spawnEntity = (uint16_t)type;
    }
    void dispenser(int x, int y, int z, int dir, const char* table) {
        int X = wx(x, z), Y = wy(y), Z = wz(x, z);
        if (!clip.inside(X, Y, Z)) return;
        c.set(X & 15, Y, Z & 15, tf(facing(B[B_DISPENSER], dir)));
        blocks++;
        TileEntity* t = c.addTile(TILE_DISPENSER, X & 15, Y, Z & 15);
        TileEntity* tmp = t ? new TileEntity() : nullptr;
        if (tmp) {
            fillLoot(tmp, table, seed ^ (int64_t)X * 31 ^ (int64_t)Z * 17 ^ Y);
            int k = 0;
            for (int i = 0; i < 27 && k < 9; i++)
                if (!tmp->items[i].empty()) t->items[k++] = tmp->items[i];
            delete tmp;
        }
    }
};


// ------------------------------------------------------------------ templates (templates.cpp)
// A template placed at (x, y, z) (its origin; the pivot at 0), mirrored (1 left-right, 2
// front-back) and rotated (1 clockwise 90, 2 180, 3 counter-clockwise 90), with a processor
// list (TD_PROC_LISTS); legacy: its air is not placed; gravity: terrain-matching, each
// block's y from the chunk's surface.
struct TemplatePlacement {
    int tmpl;
    int x, y, z;
    uint8_t rot, mirror;
    uint16_t processors;
    bool legacy, gravity, keepJigsaws, ignoreEntities;
    bool noWaterlogging;   // liquid_settings: ignore_waterlogging
    int pivotX = 0, pivotZ = 0;   // the rotation's pivot (StructurePlaceSettings#setRotationPivot)
    float integrity = 1.0f;       // a BlockRotProcessor of its own (ocean ruins)
    // the template's data markers ("chest", "drowned", ...), at their place in the world
    void (*marker)(void* ctx, Chunk& c, const SBox& clip, const char* name, int x, int y, int z) = nullptr;
    void* markerCtx = nullptr;
    // processors of the structure's own after the list's: the new state (drop: none)
    uint16_t (*process)(void* ctx, uint16_t st, uint16_t world, int x, int y, int z, bool& drop) = nullptr;
    void* processCtx = nullptr;
};
// StructureTemplate#transform: a template position in the world's terms (around the pivot)
void templateTransform(int x, int z, int mirror, int rot, int pivotX, int pivotZ, int& ox, int& oz);
void templatesInit();
uint16_t templateState(int i);   // TD_STATES[i] resolved
uint16_t transformState(uint16_t st, int mirror, int rot);
SBox templateBox(int tmpl, int x, int y, int z, int rot, int mirror, int pivotX = 0, int pivotZ = 0);
int placeTemplate(Chunk& c, const SBox& clip, const TemplatePlacement& t, int64_t seed);   // the blocks placed

// ------------------------------------------------------------------ the strongholds (stronghold.cpp)
// The pieces of the stronghold starting in chunk (cx, cz) (StrongholdStructure#generatePieces):
// their count (at most max), 0 if none
int strongholdPieces(int64_t worldSeed, int cx, int cz, Piece* out, int max);
void buildStronghold(Build& b);   // a piece of a stronghold

// ------------------------------------------------------------------ Nether fortresses (fortress.cpp)
// NetherFortressStructure#generatePieces from chunk (cx, cz): the pieces' count (at most max)
int fortressPieces(int64_t worldSeed, int cx, int cz, Piece* out, int max);
void buildFortress(Build& b);

// ------------------------------------------------------------------ mineshafts (mineshaft.cpp)
// MineshaftStructure#generatePiecesAndAdjust from chunk (cx, cz): the pieces' count, and how
// far they were moved up or down (the start's height for its biome); mesa: the badlands'
// kind (dark oak, raised to the surface: ground is the WORLD_SURFACE_WG height there)
int mineshaftPieces(int64_t worldSeed, int cx, int cz, bool mesa, Piece* out, int max, int& lift, int (*ground)(void*, int, int), void* ctx);
void buildMineshaft(Build& b);

// ------------------------------------------------------------------ jigsaw structures (jigsaw.cpp)
struct JigsawParams {
    int startPool;          // TD_POOLS index
    uint16_t startJigsawName;   // TD_STRINGS index, 0: none
    int startY;
    bool projectToHeightmap, expansionHack;
    int maxDepth, maxDistance;
    int minY, maxY, padBottom, padTop;   // the dimension's
    // pool aliases resolved for this start (alias with "minecraft:" -> target pool)
    const char* aliasName[16];
    int aliasPool[16];
    int aliases;
};
int64_t positionSeed(int x, int y, int z);   // Mth.getSeed (templates.cpp)
typedef int (*TerrainHeight)(void* ctx, int x, int z);   // WORLD_SURFACE_WG's first free y
// JigsawPlacement#addPieces from chunk (cx, cz) with the generation context's random (r):
// the pieces (at most max) and the start's position for its biome (sx, sy, sz)
// biomeOk: whether the structure may stand at the start's position (Structure#isValidBiome,
// asked before the pieces are assembled: no random number is drawn for it)
int jigsawPieces(const JigsawParams& jp, int64_t worldSeed, int cx, int cz, JavaRandom& r, TerrainHeight height,
                 bool (*biomeOk)(void* ctx, int x, int y, int z), void* ctx, Piece* out, int max);
void buildJigsaw(Chunk& c, const SBox& clip, const Piece& p, int64_t seed, bool noWaterlogging);

// ------------------------------------------------------------------ single templates (template_structures.cpp)
int iglooPieces(JavaRandom& r, int cx, int cz, TerrainProbe& probe, Piece* out, int max);
int shipwreckPieces(JavaRandom& r, int cx, int cz, bool beached, TerrainProbe& probe, Piece* out, int max);
int oceanRuinPieces(JavaRandom& r, int cx, int cz, bool warm, TerrainProbe& probe, Piece* out, int max);
int netherFossilPieces(JavaRandom& r, int cx, int cz, TerrainProbe& probe, const uint8_t* biomes, Piece* out, int max);
void buildTemplatePiece(Chunk& c, const SBox& clip, const Piece& p, int64_t seed);
// ruined_portal.cpp: the portal and its properties' piece (2 pieces)
int ruinedPortalPieces(JavaRandom& r, int cx, int cz, const StructureDef& st, uint8_t dim, TerrainProbe& probe, Piece* out, int max);
void buildRuinedPortal(Chunk& c, const SBox& clip, const Piece& p, const Piece& props, int64_t seed);
// end_city.cpp: an End city's pieces (PK_TEMPLATE, style 6)
int endCityPieces(JavaRandom& r, int cx, int cz, TerrainProbe& probe, const uint8_t* biomes, Piece* out, int max);

}  // namespace sb
}  // namespace vanilla
}  // namespace mc
