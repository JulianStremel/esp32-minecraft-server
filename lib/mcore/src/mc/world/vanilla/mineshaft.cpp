// The mineshafts: vanilla's MineshaftPieces and MineshaftStructure (1.21.8): the room, its
// corridors, crossings and stairs grown depth first in vanilla's order of random numbers
// (so a start gives vanilla's layout for the seed the generator's Random is given); each
// piece's blocks as its postProcess places them, the random choices per block from the
// block's own random number. Pieces next to a liquid are left out of that chunk, as
// vanilla's isInInvalidLocation does.
#include <math.h>
#include <stdlib.h>
#include "mc/platform.h"
#include "mc/world/vanilla/structure_build.h"

namespace mc {
namespace vanilla {
namespace sb {

namespace {

enum : int16_t { MS_RAILS = 1, MS_SPIDER = 2, MS_TWO_FLOORS = 4, MS_MESA = 8 };
constexpr int MAX_PIECES = 400;

struct Gen {
    JavaRandom& r;
    Piece* p;
    int n = 0, max;
    bool mesa;
    int depth[MAX_PIECES];
    Gen(JavaRandom& rnd, Piece* out, int maxPieces, bool m) : r(rnd), p(out), max(maxPieces), mesa(m) {}
    bool collides(const SBox& b) const {
        for (int i = 0; i < n; i++)
            if (p[i].box.hits(b) && p[i].kind != PK_MS_ENTRANCE) return true;
        return false;
    }
    int add(const Piece& piece, int genDepth) {
        if (n >= max || n >= MAX_PIECES) return -1;
        p[n] = piece;
        depth[n] = genDepth;
        return n++;
    }
    static SBox moved(SBox b, int x, int y, int z) { return {b.x0 + x, b.y0 + y, b.z0 + z, b.x1 + x, b.y1 + y, b.z1 + z}; }
    // MineShaftPiece#createRandomShaftPiece
    bool randomPiece(int x, int y, int z, int dir, Piece& out) {
        int i = r.nextInt(100);
        out = {};
        out.orient = (uint8_t)dir;
        if (mesa) out.a = MS_MESA;
        if (i >= 80) {   // MineShaftCrossing#findCrossing
            int h = r.nextInt(4) == 0 ? 6 : 2;
            SBox b = dir == SOUTH ? SBox{-1, 0, 0, 3, h, 4} : dir == WEST ? SBox{-4, 0, -1, 0, h, 3}
                     : dir == EAST ? SBox{0, 0, -1, 4, h, 3} : SBox{-1, 0, -4, 3, h, 0};
            b = moved(b, x, y, z);
            if (collides(b)) return false;
            out.box = b;
            out.kind = PK_MS_CROSSING;
            out.orient = UNORIENTED;   // its blocks in world terms (vanilla keeps only its direction)
            out.b = dir;
            if (b.y1 - b.y0 + 1 > 3) out.a |= MS_TWO_FLOORS;
            return true;
        }
        if (i >= 70) {   // MineShaftStairs#findStairs
            SBox b = dir == SOUTH ? SBox{0, -5, 0, 2, 2, 8} : dir == WEST ? SBox{-8, -5, 0, 0, 2, 2}
                     : dir == EAST ? SBox{0, -5, 0, 8, 2, 2} : SBox{0, -5, -8, 2, 2, 0};
            b = moved(b, x, y, z);
            if (collides(b)) return false;
            out.box = b;
            out.kind = PK_MS_STAIRS;
            return true;
        }
        // MineShaftCorridor#findCorridorSize: 2 to 4 sections of 5, the longest that fits
        for (int k = r.nextInt(3) + 2; k > 0; k--) {
            int j = k * 5;
            SBox b = dir == SOUTH ? SBox{0, 0, 0, 2, 2, j - 1} : dir == WEST ? SBox{-(j - 1), 0, 0, 0, 2, 2}
                     : dir == EAST ? SBox{0, 0, 0, j - 1, 2, 2} : SBox{0, 0, -(j - 1), 2, 2, 0};
            b = moved(b, x, y, z);
            if (collides(b)) continue;
            out.box = b;
            out.kind = PK_MS_CORRIDOR;
            if (r.nextInt(3) == 0) out.a |= MS_RAILS;
            else if (r.nextInt(23) == 0) out.a |= MS_SPIDER;
            out.b = (dir == NORTH || dir == SOUTH ? b.z1 - b.z0 + 1 : b.x1 - b.x0 + 1) / 5;
            return true;
        }
        return false;
    }
    // MineShaftPiece#generateAndAddPiece: depth first, the new piece's children at once
    int gen(int x, int y, int z, int dir, int genDepth) {
        if (genDepth > 8) return -1;
        if (abs(x - p[0].box.x0) > 80 || abs(z - p[0].box.z0) > 80) return -1;
        Piece piece;
        if (!randomPiece(x, y, z, dir, piece)) return -1;
        int i = add(piece, genDepth + 1);
        if (i >= 0) children(i);
        return i;
    }
    void children(int i) {
        const SBox b = p[i].box;
        int d = depth[i], dir = p[i].orient;
        switch (p[i].kind) {
            case PK_MS_ROOM: {
                int j = (b.y1 - b.y0 + 1) - 3 - 1;
                if (j <= 0) j = 1;
                int xs = b.x1 - b.x0 + 1, zs = b.z1 - b.z0 + 1;
                // the corridors out of each wall, and the openings they need in it
                for (int side = 0; side < 4; side++) {
                    int span = side < 2 ? xs : zs;
                    for (int k = 0; k < span; k += 4) {
                        k += r.nextInt(span);
                        if (k + 3 > span) break;
                        int y = b.y0 + r.nextInt(j) + 1, c;
                        if (side == 0) c = gen(b.x0 + k, y, b.z0 - 1, NORTH, d);
                        else if (side == 1) c = gen(b.x0 + k, y, b.z1 + 1, SOUTH, d);
                        else if (side == 2) c = gen(b.x0 - 1, y, b.z0 + k, WEST, d);
                        else c = gen(b.x1 + 1, y, b.z0 + k, EAST, d);
                        if (c < 0) continue;
                        const SBox cb = p[c].box;
                        Piece e = {};
                        e.kind = PK_MS_ENTRANCE;
                        e.orient = UNORIENTED;
                        if (side == 0) e.box = {cb.x0, cb.y0, b.z0, cb.x1, cb.y1, b.z0 + 1};
                        else if (side == 1) e.box = {cb.x0, cb.y0, b.z1 - 1, cb.x1, cb.y1, b.z1};
                        else if (side == 2) e.box = {b.x0, cb.y0, cb.z0, b.x0 + 1, cb.y1, cb.z1};
                        else e.box = {b.x1 - 1, cb.y0, cb.z0, b.x1, cb.y1, cb.z1};
                        add(e, d);
                    }
                }
                break;
            }
            case PK_MS_CORRIDOR: {
                int j = r.nextInt(4);
                switch (dir) {
                    case SOUTH:
                        if (j <= 1) gen(b.x0, b.y0 - 1 + r.nextInt(3), b.z1 + 1, SOUTH, d);
                        else if (j == 2) gen(b.x0 - 1, b.y0 - 1 + r.nextInt(3), b.z1 - 3, WEST, d);
                        else gen(b.x1 + 1, b.y0 - 1 + r.nextInt(3), b.z1 - 3, EAST, d);
                        break;
                    case WEST:
                        if (j <= 1) gen(b.x0 - 1, b.y0 - 1 + r.nextInt(3), b.z0, WEST, d);
                        else if (j == 2) gen(b.x0, b.y0 - 1 + r.nextInt(3), b.z0 - 1, NORTH, d);
                        else gen(b.x0, b.y0 - 1 + r.nextInt(3), b.z1 + 1, SOUTH, d);
                        break;
                    case EAST:
                        if (j <= 1) gen(b.x1 + 1, b.y0 - 1 + r.nextInt(3), b.z0, EAST, d);
                        else if (j == 2) gen(b.x1 - 3, b.y0 - 1 + r.nextInt(3), b.z0 - 1, NORTH, d);
                        else gen(b.x1 - 3, b.y0 - 1 + r.nextInt(3), b.z1 + 1, SOUTH, d);
                        break;
                    default:   // NORTH
                        if (j <= 1) gen(b.x0, b.y0 - 1 + r.nextInt(3), b.z0 - 1, NORTH, d);
                        else if (j == 2) gen(b.x0 - 1, b.y0 - 1 + r.nextInt(3), b.z0, WEST, d);
                        else gen(b.x1 + 1, b.y0 - 1 + r.nextInt(3), b.z0, EAST, d);
                        break;
                }
                if (d < 8) {
                    if (dir != NORTH && dir != SOUTH) {
                        for (int k = b.x0 + 3; k + 3 <= b.x1; k += 5) {
                            int l = r.nextInt(5);
                            if (l == 0) gen(k, b.y0, b.z0 - 1, NORTH, d + 1);
                            else if (l == 1) gen(k, b.y0, b.z1 + 1, SOUTH, d + 1);
                        }
                    } else {
                        for (int k = b.z0 + 3; k + 3 <= b.z1; k += 5) {
                            int l = r.nextInt(5);
                            if (l == 0) gen(b.x0 - 1, b.y0, k, WEST, d + 1);
                            else if (l == 1) gen(b.x1 + 1, b.y0, k, EAST, d + 1);
                        }
                    }
                }
                break;
            }
            case PK_MS_CROSSING: {
                switch (p[i].b) {
                    case SOUTH:
                        gen(b.x0 + 1, b.y0, b.z1 + 1, SOUTH, d);
                        gen(b.x0 - 1, b.y0, b.z0 + 1, WEST, d);
                        gen(b.x1 + 1, b.y0, b.z0 + 1, EAST, d);
                        break;
                    case WEST:
                        gen(b.x0 + 1, b.y0, b.z0 - 1, NORTH, d);
                        gen(b.x0 + 1, b.y0, b.z1 + 1, SOUTH, d);
                        gen(b.x0 - 1, b.y0, b.z0 + 1, WEST, d);
                        break;
                    case EAST:
                        gen(b.x0 + 1, b.y0, b.z0 - 1, NORTH, d);
                        gen(b.x0 + 1, b.y0, b.z1 + 1, SOUTH, d);
                        gen(b.x1 + 1, b.y0, b.z0 + 1, EAST, d);
                        break;
                    default:
                        gen(b.x0 + 1, b.y0, b.z0 - 1, NORTH, d);
                        gen(b.x0 - 1, b.y0, b.z0 + 1, WEST, d);
                        gen(b.x1 + 1, b.y0, b.z0 + 1, EAST, d);
                        break;
                }
                if (p[i].a & MS_TWO_FLOORS) {
                    if (r.nextBoolean()) gen(b.x0 + 1, b.y0 + 3 + 1, b.z0 - 1, NORTH, d);
                    if (r.nextBoolean()) gen(b.x0 - 1, b.y0 + 3 + 1, b.z0 + 1, WEST, d);
                    if (r.nextBoolean()) gen(b.x1 + 1, b.y0 + 3 + 1, b.z0 + 1, EAST, d);
                    if (r.nextBoolean()) gen(b.x0 + 1, b.y0 + 3 + 1, b.z1 + 1, SOUTH, d);
                }
                break;
            }
            case PK_MS_STAIRS:
                switch (dir) {
                    case SOUTH: gen(b.x0, b.y0, b.z1 + 1, SOUTH, d); break;
                    case WEST: gen(b.x0 - 1, b.y0, b.z0, WEST, d); break;
                    case EAST: gen(b.x1 + 1, b.y0, b.z0, EAST, d); break;
                    default: gen(b.x0, b.y0, b.z0 - 1, NORTH, d); break;
                }
                break;
            default: break;
        }
    }
};

}  // namespace

int mineshaftPieces(int64_t worldSeed, int cx, int cz, bool mesa, Piece* out, int max, int& lift,
                    int (*ground)(void*, int, int), void* ctx) {
    JavaRandom r;
    r.setLargeFeatureSeed(worldSeed, cx, cz);
    r.nextDouble();   // MineshaftStructure#findGenerationPoint: the old chance, still drawn
    Gen* g = new Gen(r, out, max, mesa);
    if (!g) return 0;
    // MineShaftRoom at (2, 50, 2) in the chunk, 7 to 12 wide, 5 to 10 high
    Piece room = {};
    int x = cx * 16 + 2, z = cz * 16 + 2;
    int x1 = x + 7 + r.nextInt(6), y1 = 54 + r.nextInt(6), z1 = z + 7 + r.nextInt(6);
    room.box = {x, 50, z, x1, y1, z1};
    room.kind = PK_MS_ROOM;
    room.orient = UNORIENTED;
    if (mesa) room.a = MS_MESA;
    g->add(room, 0);
    g->children(0);
    // the whole shaft below the sea (or, in the badlands, to the surface)
    SBox all = out[0].box;
    for (int i = 1; i < g->n; i++) {
        const SBox& b = out[i].box;
        if (b.x0 < all.x0) all.x0 = b.x0;
        if (b.y0 < all.y0) all.y0 = b.y0;
        if (b.z0 < all.z0) all.z0 = b.z0;
        if (b.x1 > all.x1) all.x1 = b.x1;
        if (b.y1 > all.y1) all.y1 = b.y1;
        if (b.z1 > all.z1) all.z1 = b.z1;
    }
    const int sea = 63;
    if (mesa) {
        int cxm = all.x0 + (all.x1 - all.x0 + 1) / 2, cym = all.y0 + (all.y1 - all.y0 + 1) / 2, czm = all.z0 + (all.z1 - all.z0 + 1) / 2;
        int j = ground(ctx, cxm, czm);
        int k = j <= sea ? sea : sea + r.nextInt(j - sea + 1);   // Mth.randomBetweenInclusive
        lift = k - cym;
    } else {   // moveBelowSeaLevel(63, -64, random, 10)
        int top = sea - 10, j = (all.y1 - all.y0 + 1) + (-64) + 1;
        if (j < top) j += r.nextInt(top - j);
        lift = j - all.y1;
    }
    for (int i = 0; i < g->n; i++) out[i].box.moveY(lift);
    int n = g->n;
    delete g;
    return n;
}

// ------------------------------------------------------------------ the pieces' blocks
namespace {

uint16_t planks(const Build& b) { return b.p.a & MS_MESA ? B[B_DARK_OAK_PLANKS] : B[B_OAK_PLANKS]; }
uint16_t fenceOf(const Build& b) { return b.p.a & MS_MESA ? B[B_DARK_OAK_FENCE] : B[B_OAK_FENCE]; }
float rnd(const Build& b, int x, int y, int z, uint32_t salt) { return posRandom(b.seed, b.wx(x, z), b.wy(y), b.wz(x, z), salt); }
void caveAir(Build& b, int x0, int y0, int z0, int x1, int y1, int z1) { b.box(x0, y0, z0, x1, y1, z1, B[B_CAVE_AIR], B[B_CAVE_AIR]); }

// StructurePiece#isInterior: below the chunk's surface
bool interior(Build& b, int x, int y, int z) {
    int X = b.wx(x, z), Y = b.wy(y), Z = b.wz(x, z);
    return b.clip.inside(X, Y, Z) && Y < b.c.height(X & 15, Z & 15);
}

// MineShaftPiece#isInInvalidLocation: a liquid on the piece's faces (in this chunk)
bool liquidAround(Build& b) {
    const SBox& p = b.p.box;
    int x0 = p.x0 - 1 > b.clip.x0 ? p.x0 - 1 : b.clip.x0, y0 = p.y0 - 1 > b.clip.y0 ? p.y0 - 1 : b.clip.y0;
    int z0 = p.z0 - 1 > b.clip.z0 ? p.z0 - 1 : b.clip.z0, x1 = p.x1 + 1 < b.clip.x1 ? p.x1 + 1 : b.clip.x1;
    int y1 = p.y1 + 1 < b.clip.y1 ? p.y1 + 1 : b.clip.y1, z1 = p.z1 + 1 < b.clip.z1 ? p.z1 + 1 : b.clip.z1;
    auto liquid = [&](int x, int y, int z) { return stateIsFluid(b.c.get(x & 15, y, z & 15)); };
    for (int x = x0; x <= x1; x++)
        for (int z = z0; z <= z1; z++)
            if (liquid(x, y0, z) || liquid(x, y1, z)) return true;
    for (int x = x0; x <= x1; x++)
        for (int y = y0; y <= y1; y++)
            if (liquid(x, y, z0) || liquid(x, y, z1)) return true;
    for (int z = z0; z <= z1; z++)
        for (int y = y0; y <= y1; y++)
            if (liquid(x0, y, z) || liquid(x1, y, z)) return true;
    return false;
}

// StructurePiece#generateUpperHalfSphere (the room's dome)
void upperHalfSphere(Build& b, int x0, int y0, int z0, int x1, int y1, int z1, uint16_t st) {
    float f = (float)(x1 - x0 + 1), f1 = (float)(y1 - y0 + 1), f2 = (float)(z1 - z0 + 1);
    float f3 = (float)x0 + f / 2.0f, f4 = (float)z0 + f2 / 2.0f;
    for (int y = y0; y <= y1; y++) {
        float f5 = (float)(y - y0) / f1;
        for (int x = x0; x <= x1; x++) {
            float f6 = ((float)x - f3) / (f * 0.5f);
            for (int z = z0; z <= z1; z++) {
                float f7 = ((float)z - f4) / (f2 * 0.5f);
                if (f6 * f6 + f5 * f5 + f7 * f7 <= 1.05f) b.set(x, y, z, st);
            }
        }
    }
}

void room(Build& b) {
    if (liquidAround(b)) return;
    int w = b.p.box.x1 - b.p.box.x0, h = b.p.box.y1 - b.p.box.y0, d = b.p.box.z1 - b.p.box.z0;
    caveAir(b, 0, 1, 0, w, h < 3 ? h : 3, d);
    upperHalfSphere(b, 0, 4, 0, w, h, d, B[B_CAVE_AIR]);
}

// the opening a corridor needs in the room's wall (MineShaftRoom's childEntranceBoxes)
void entrance(Build& b) {
    int w = b.p.box.x1 - b.p.box.x0, h = b.p.box.y1 - b.p.box.y0, d = b.p.box.z1 - b.p.box.z0;
    caveAir(b, 0, h - 2, 0, w, h, d);
}

// MineShaftCorridor#setPlanksBlock: a floor where it is not sturdy
void planksBlock(Build& b, int x, int y, int z) {
    if (!interior(b, x, y, z)) return;
    if (!stateFaceSturdy(b.get(x, y, z), 1)) b.set(x, y, z, planks(b));
}

// MineShaftCorridor#placeDoubleLowerOrUpperSupport: a fence post down to the ground, or a
// chain up to the ceiling, under the corridor's ends
void doubleSupport(Build& b, int x, int y, int z) {
    int X = b.wx(x, z), Y = b.wy(y), Z = b.wz(x, z);
    if (!b.clip.inside(X, Y, Z)) return;
    Chunk& c = b.c;
    bool down = true, up = true;
    for (int j = 1; down || up; j++) {
        if (down) {
            uint16_t s = c.get(X & 15, Y - j, Z & 15);
            bool repl = (stateIsAir(s) || stateIsFluid(s)) && blockIdOf(s) != blockIdOf(B[B_LAVA]);
            if (!repl && stateFaceSturdy(s, 1)) {
                for (int k = Y - j + 1; k <= Y; k++) c.set(X & 15, k, Z & 15, fenceOf(b));
                return;
            }
            down = j <= 20 && repl && Y - j > c.minY() + 1;
        }
        if (up) {
            uint16_t s = c.get(X & 15, Y + j, Z & 15);
            bool repl = stateIsAir(s) || stateIsFluid(s);
            if (!repl && stateFaceSturdy(s, 0)) {
                c.set(X & 15, Y + 1, Z & 15, fenceOf(b));
                for (int k = Y + 2; k <= Y + j - 1; k++) c.set(X & 15, k, Z & 15, setPropStr(B[B_CHAIN], "axis", "y"));
                return;
            }
            up = j <= 50 && repl && Y + j < c.maxY() - 1;
        }
    }
}

// MineShaftCorridor#placeSupport: two posts and a beam where the ceiling holds it
void support(Build& b, int x0, int y0, int z, int y1, int x1, uint32_t salt) {
    for (int x = x0; x <= x1; x++)
        if (stateIsAir(b.get(x, y1 + 1, z))) return;
    b.box(x0, y0, z, x0, y1 - 1, z, fenceOf(b), B[B_CAVE_AIR]);
    b.box(x1, y0, z, x1, y1 - 1, z, fenceOf(b), B[B_CAVE_AIR]);
    if (rnd(b, x0, y1, z, salt) < 0.25f) {
        b.box(x0, y1, z, x0, y1, z, planks(b), B[B_CAVE_AIR]);
        b.box(x1, y1, z, x1, y1, z, planks(b), B[B_CAVE_AIR]);
    } else {
        b.box(x0, y1, z, x1, y1, z, planks(b), B[B_CAVE_AIR]);
        if (interior(b, x0 + 1, y1, z - 1) && rnd(b, x0 + 1, y1, z - 1, salt + 1) < 0.05f)
            b.set(x0 + 1, y1, z - 1, facing(B[B_WALL_TORCH], SOUTH));
        if (interior(b, x0 + 1, y1, z + 1) && rnd(b, x0 + 1, y1, z + 1, salt + 2) < 0.05f)
            b.set(x0 + 1, y1, z + 1, facing(B[B_WALL_TORCH], NORTH));
    }
}

// MineShaftPiece#maybePlaceCobWeb: inside, at random, with two sturdy neighbours
void cobweb(Build& b, float chance, int x, int y, int z, uint32_t salt) {
    if (!interior(b, x, y, z) || rnd(b, x, y, z, salt) >= chance) return;
    int sturdy = 0;
    static const int D[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const auto& o : D) sturdy += stateOpaque(b.get(x + o[0], y + o[1], z + o[2]));
    if (sturdy >= 2) b.set(x, y, z, B[B_COBWEB]);
}

// MineShaftCorridor#createChest: a chest minecart on a rail, with the shaft's loot
void chestMinecart(Build& b, int x, int y, int z, uint32_t salt) {
    int X = b.wx(x, z), Y = b.wy(y), Z = b.wz(x, z);
    if (!b.clip.inside(X, Y, Z) || !stateIsAir(b.get(x, y, z)) || stateIsAir(b.get(x, y - 1, z))) return;
    uint16_t rail = setPropStr(B[B_RAIL], "shape", rnd(b, x, y, z, salt) < 0.5f ? "north_south" : "east_west");
    b.c.set(X & 15, Y, Z & 15, rail);   // its shape in world terms already
    TileEntity* loot = new TileEntity();
    if (!loot) return;
    fillLoot(loot, "abandoned_mineshaft", b.seed ^ (int64_t)X * 31 ^ (int64_t)Z * 17 ^ Y);
    SavedEntity e;
    e.kind = SAVED_KIND_MINECART;
    int type = findEntityType("chest_minecart");
    e.type = (uint16_t)(type >= 0 ? type : 0);
    e.x = X + 0.5;
    e.y = Y + 0.0625;
    e.z = Z + 0.5;
    e.health = 1;
    e.cargo.resize(27);
    for (int i = 0; i < 27; i++) e.cargo[i] = loot->items[i];
    delete loot;
    if (type >= 0) b.c.addEntity(e);
}

void corridor(Build& b) {
    if (liquidAround(b)) return;
    int a = b.p.a, sections = b.p.b, len = sections * 5 - 1;
    caveAir(b, 0, 0, 0, 2, 1, len);
    for (int y = 2; y <= 2; y++)   // the ceiling's row open at 80 %
        for (int x = 0; x <= 2; x++)
            for (int z = 0; z <= len; z++)
                if (rnd(b, x, y, z, 21) <= 0.8f) b.set(x, y, z, B[B_CAVE_AIR]);
    if (a & MS_SPIDER)   // cobwebs everywhere inside
        for (int y = 0; y <= 1; y++)
            for (int x = 0; x <= 2; x++)
                for (int z = 0; z <= len; z++)
                    if (rnd(b, x, y, z, 22) <= 0.6f && interior(b, x, y, z)) b.set(x, y, z, B[B_COBWEB]);
    for (int j = 0; j < sections; j++) {
        int k = 2 + j * 5;
        support(b, 0, 0, k, 2, 2, 30);
        cobweb(b, 0.1f, 0, 2, k - 1, 31); cobweb(b, 0.1f, 2, 2, k - 1, 32);
        cobweb(b, 0.1f, 0, 2, k + 1, 33); cobweb(b, 0.1f, 2, 2, k + 1, 34);
        cobweb(b, 0.05f, 0, 2, k - 2, 35); cobweb(b, 0.05f, 2, 2, k - 2, 36);
        cobweb(b, 0.05f, 0, 2, k + 2, 37); cobweb(b, 0.05f, 2, 2, k + 2, 38);
        if (rnd(b, 2, 0, k - 1, 39) < 0.01f) chestMinecart(b, 2, 0, k - 1, 40);
        if (rnd(b, 0, 0, k + 1, 41) < 0.01f) chestMinecart(b, 0, 0, k + 1, 42);
    }
    if (a & MS_SPIDER) {   // its cave spider spawner, in the first section
        JavaRandom r(b.seed);
        int z = 1 + r.nextInt(3);
        if (interior(b, 1, 0, z)) b.spawner(1, 0, z, "cave_spider");
    }
    for (int x = 0; x <= 2; x++)
        for (int z = 0; z <= len; z++) planksBlock(b, x, -1, z);
    doubleSupport(b, 0, -1, 2); doubleSupport(b, 2, -1, 2);
    doubleSupport(b, 0, -1, len - 2); doubleSupport(b, 2, -1, len - 2);
    if (a & MS_RAILS) {
        uint16_t rail = setPropStr(B[B_RAIL], "shape", "north_south");
        for (int z = 0; z <= len; z++) {
            uint16_t under = b.get(1, -1, z);
            if (stateIsAir(under) || !stateOpaque(under)) continue;
            float f = interior(b, 1, 0, z) ? 0.7f : 0.9f;
            if (b.inside(1, 0, z) && rnd(b, 1, 0, z, 50) < f) b.set(1, 0, z, rail);
        }
    }
}

void crossing(Build& b) {
    if (liquidAround(b)) return;
    int w = b.p.box.x1 - b.p.box.x0, h = b.p.box.y1 - b.p.box.y0, d = b.p.box.z1 - b.p.box.z0;
    if (b.p.a & MS_TWO_FLOORS) {
        caveAir(b, 1, 0, 0, w - 1, 2, d);
        caveAir(b, 0, 0, 1, w, 2, d - 1);
        caveAir(b, 1, h - 2, 0, w - 1, h, d);
        caveAir(b, 0, h - 2, 1, w, h, d - 1);
        caveAir(b, 1, 3, 1, w - 1, 3, d - 1);
    } else {
        caveAir(b, 1, 0, 0, w - 1, h, d);
        caveAir(b, 0, 0, 1, w, h, d - 1);
    }
    const int px[4] = {1, 1, w - 1, w - 1}, pz[4] = {1, d - 1, 1, d - 1};
    for (int k = 0; k < 4; k++)   // the pillars where the ceiling holds them
        if (!stateIsAir(b.get(px[k], h + 1, pz[k]))) b.box(px[k], 0, pz[k], px[k], h, pz[k], planks(b), B[B_CAVE_AIR]);
    for (int x = 0; x <= w; x++)
        for (int z = 0; z <= d; z++) planksBlock(b, x, -1, z);
}

void stairs(Build& b) {
    if (liquidAround(b)) return;
    caveAir(b, 0, 5, 0, 2, 7, 1);
    caveAir(b, 0, 0, 7, 2, 2, 8);
    for (int i = 0; i < 5; i++) caveAir(b, 0, 5 - i - (i < 4 ? 1 : 0), 2 + i, 2, 7 - i, 2 + i);
}

}  // namespace

void buildMineshaft(Build& b) {
    switch (b.p.kind) {
        case PK_MS_ROOM: room(b); break;
        case PK_MS_ENTRANCE: entrance(b); break;
        case PK_MS_CORRIDOR: corridor(b); break;
        case PK_MS_CROSSING: crossing(b); break;
        case PK_MS_STAIRS: stairs(b); break;
        default: break;
    }
}

}  // namespace sb
}  // namespace vanilla
}  // namespace mc
