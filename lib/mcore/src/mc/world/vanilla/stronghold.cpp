// The strongholds: vanilla's StrongholdPieces and StrongholdStructure#generatePieces
// (1.21.8): the same pieces, weights, limits and order of random numbers, so a start
// gives vanilla's layout for the seed the generator's Random is given; the blocks of
// each piece as its postProcess places them (the random choices per block from the
// block's own random number: the same in whichever chunk comes first).
#include <stdlib.h>
#include "mc/platform.h"
#include "mc/world/vanilla/structure_build.h"

namespace mc {
namespace vanilla {
namespace sb {

namespace {

enum Door : uint8_t { OPENING, WOOD_DOOR, GRATES, IRON_DOOR };

// StrongholdPieces.STRONGHOLD_PIECE_WEIGHTS
struct Weight { uint8_t kind; int weight, maxCount; };
const Weight WEIGHTS[11] = {
    {PK_SH_STRAIGHT, 40, 0},     {PK_SH_PRISON_HALL, 5, 5},          {PK_SH_LEFT_TURN, 20, 0},
    {PK_SH_RIGHT_TURN, 20, 0},   {PK_SH_ROOM_CROSSING, 10, 6},       {PK_SH_STRAIGHT_STAIRS_DOWN, 5, 5},
    {PK_SH_STAIRS_DOWN, 5, 5},   {PK_SH_FIVE_CROSSING, 5, 4},        {PK_SH_CHEST_CORRIDOR, 5, 4},
    {PK_SH_LIBRARY, 10, 2},      {PK_SH_PORTAL_ROOM, 20, 1},
};

constexpr int MAX_PIECES = 400;

struct Gen {
    JavaRandom& r;
    Piece* p;
    int n = 0;
    int depth[MAX_PIECES];
    int list[11], nlist = 0, placed[11] = {};
    int total = 0;
    int imposed = -1;    // a kind (StrongholdPieces.imposedPiece)
    int previous = -1;   // the weight entry last placed (StartPiece.previousPiece)
    int portalRoom = -1;
    int pending[MAX_PIECES], npending = 0;
    explicit Gen(JavaRandom& rnd, Piece* out) : r(rnd), p(out) {
        for (int i = 0; i < 11; i++) list[nlist++] = i;
    }
    bool collides(const SBox& b) const {
        for (int i = 0; i < n; i++)
            if (p[i].box.hits(b)) return true;
        return false;
    }
    int collider(const SBox& b) const {
        for (int i = 0; i < n; i++)
            if (p[i].box.hits(b)) return i;
        return -1;
    }
    static bool okBox(const SBox& b) { return b.y0 > 10; }
    int randomDoor() {
        switch (r.nextInt(5)) {
            case 2: return WOOD_DOOR;
            case 3: return GRATES;
            case 4: return IRON_DOOR;
            default: return OPENING;
        }
    }
    // StrongholdPieces#findAndCreatePieceFactory: the piece's box and constructor, or none
    bool create(int kind, int x, int y, int z, int dir, int genDepth, Piece& out) {
        SBox b;
        switch (kind) {
            case PK_SH_STRAIGHT: b = orientBox(x, y, z, -1, -1, 0, 5, 5, 7, dir); break;
            case PK_SH_PRISON_HALL: b = orientBox(x, y, z, -1, -1, 0, 9, 5, 11, dir); break;
            case PK_SH_LEFT_TURN: case PK_SH_RIGHT_TURN: b = orientBox(x, y, z, -1, -1, 0, 5, 5, 5, dir); break;
            case PK_SH_ROOM_CROSSING: b = orientBox(x, y, z, -4, -1, 0, 11, 7, 11, dir); break;
            case PK_SH_STRAIGHT_STAIRS_DOWN: b = orientBox(x, y, z, -1, -7, 0, 5, 11, 8, dir); break;
            case PK_SH_STAIRS_DOWN: b = orientBox(x, y, z, -1, -7, 0, 5, 11, 5, dir); break;
            case PK_SH_FIVE_CROSSING: b = orientBox(x, y, z, -4, -3, 0, 10, 9, 11, dir); break;
            case PK_SH_CHEST_CORRIDOR: b = orientBox(x, y, z, -1, -1, 0, 5, 5, 7, dir); break;
            case PK_SH_LIBRARY:
                b = orientBox(x, y, z, -4, -1, 0, 14, 11, 15, dir);
                if (!okBox(b) || collides(b)) b = orientBox(x, y, z, -4, -1, 0, 14, 6, 15, dir);
                break;
            case PK_SH_PORTAL_ROOM: b = orientBox(x, y, z, -4, -1, 0, 11, 8, 16, dir); break;
            default: return false;
        }
        if (!okBox(b) || collides(b)) return false;
        out = {};
        out.box = b;
        out.kind = (uint8_t)kind;
        out.orient = (uint8_t)dir;
        int a = 0;
        if (kind != PK_SH_PORTAL_ROOM) a = randomDoor();
        switch (kind) {
            case PK_SH_STRAIGHT:
                if (r.nextInt(2) == 0) a |= 8;    // left child
                if (r.nextInt(2) == 0) a |= 16;   // right child
                break;
            case PK_SH_ROOM_CROSSING: a |= r.nextInt(5) << 8; break;
            case PK_SH_FIVE_CROSSING:
                if (r.nextBoolean()) a |= 8;       // left low
                if (r.nextBoolean()) a |= 16;      // left high
                if (r.nextBoolean()) a |= 32;      // right low
                if (r.nextInt(3) > 0) a |= 64;     // right high
                break;
            case PK_SH_LIBRARY:
                if (b.y1 - b.y0 + 1 > 6) a |= 8;   // tall
                break;
            default: break;
        }
        out.a = (int16_t)a;
        out.b = genDepth;
        return true;
    }
    bool updateWeights() {
        bool any = false;
        total = 0;
        for (int i = 0; i < nlist; i++) {
            const Weight& w = WEIGHTS[list[i]];
            if (w.maxCount > 0 && placed[list[i]] < w.maxCount) any = true;
            total += w.weight;
        }
        return any;
    }
    // StrongholdPieces#generatePieceFromSmallDoor
    bool fromSmallDoor(int x, int y, int z, int dir, int genDepth, Piece& out) {
        if (!updateWeights()) return false;
        if (imposed >= 0) {
            int k = imposed;
            imposed = -1;
            if (create(k, x, y, z, dir, genDepth, out)) return true;
        }
        for (int tries = 0; tries < 5; tries++) {
            int j = r.nextInt(total);
            for (int li = 0; li < nlist; li++) {
                int wi = list[li];
                const Weight& w = WEIGHTS[wi];
                j -= w.weight;
                if (j < 0) {
                    bool can = w.maxCount == 0 || placed[wi] < w.maxCount;
                    if (w.kind == PK_SH_LIBRARY) can = can && genDepth > 4;
                    if (w.kind == PK_SH_PORTAL_ROOM) can = can && genDepth > 5;
                    if (!can || wi == previous) break;
                    if (create(w.kind, x, y, z, dir, genDepth, out)) {
                        placed[wi]++;
                        previous = wi;
                        if (w.maxCount > 0 && placed[wi] >= w.maxCount) {   // !isValid(): out of the list
                            for (int m = li; m < nlist - 1; m++) list[m] = list[m + 1];
                            nlist--;
                        }
                        return true;
                    }
                }
            }
        }
        // FillerCorridor#findPieceBox: a short corridor up to the piece in the way
        SBox b = orientBox(x, y, z, -1, -1, 0, 5, 5, 4, dir);
        int c = collider(b);
        if (c < 0 || p[c].box.y0 != b.y0) return false;
        for (int len = 2; len >= 1; len--) {
            SBox t = orientBox(x, y, z, -1, -1, 0, 5, 5, len, dir);
            if (!p[c].box.hits(t)) {
                SBox f = orientBox(x, y, z, -1, -1, 0, 5, 5, len + 1, dir);
                if (f.y0 <= 1) return false;
                out = {};
                out.box = f;
                out.kind = PK_SH_FILLER_CORRIDOR;
                out.orient = (uint8_t)dir;
                out.a = (int16_t)(dir == NORTH || dir == SOUTH ? f.z1 - f.z0 + 1 : f.x1 - f.x0 + 1);   // steps
                out.b = genDepth;
                return true;
            }
        }
        return false;
    }
    // StrongholdPieces#generateAndAddPiece
    void addPiece(int x, int y, int z, int dir, int genDepth) {
        if (genDepth > 50 || n >= MAX_PIECES) return;
        if (abs(x - p[0].box.x0) > 112 || abs(z - p[0].box.z0) > 112) return;
        Piece out;
        if (!fromSmallDoor(x, y, z, dir, genDepth + 1, out)) return;
        p[n] = out;
        depth[n] = genDepth + 1;
        pending[npending++] = n;
        n++;
    }
    // StrongholdPiece#generateSmallDoorChildForward / Left / Right
    void forward(int i, int ox, int oy) {
        const SBox b = p[i].box;
        int d = p[i].orient, g = depth[i];
        switch (d) {
            case NORTH: addPiece(b.x0 + ox, b.y0 + oy, b.z0 - 1, d, g); break;
            case SOUTH: addPiece(b.x0 + ox, b.y0 + oy, b.z1 + 1, d, g); break;
            case WEST: addPiece(b.x0 - 1, b.y0 + oy, b.z0 + ox, d, g); break;
            default: addPiece(b.x1 + 1, b.y0 + oy, b.z0 + ox, d, g); break;
        }
    }
    void left(int i, int oy, int ox) {
        const SBox b = p[i].box;
        int d = p[i].orient, g = depth[i];
        if (d == NORTH || d == SOUTH) addPiece(b.x0 - 1, b.y0 + oy, b.z0 + ox, WEST, g);
        else addPiece(b.x0 + ox, b.y0 + oy, b.z0 - 1, NORTH, g);
    }
    void right(int i, int oy, int ox) {
        const SBox b = p[i].box;
        int d = p[i].orient, g = depth[i];
        if (d == NORTH || d == SOUTH) addPiece(b.x1 + 1, b.y0 + oy, b.z0 + ox, EAST, g);
        else addPiece(b.x0 + ox, b.y0 + oy, b.z1 + 1, SOUTH, g);
    }
    // StructurePiece#addChildren per kind
    void children(int i) {
        int a = p[i].a, d = p[i].orient;
        switch (p[i].kind) {
            case PK_SH_STAIRS_DOWN:
                if (a & 8) imposed = PK_SH_FIVE_CROSSING;   // the start: a five-way crossing first
                forward(i, 1, 1);
                break;
            case PK_SH_STRAIGHT:
                forward(i, 1, 1);
                if (a & 8) left(i, 1, 2);
                if (a & 16) right(i, 1, 2);
                break;
            case PK_SH_CHEST_CORRIDOR: case PK_SH_STRAIGHT_STAIRS_DOWN: case PK_SH_PRISON_HALL: forward(i, 1, 1); break;
            case PK_SH_LEFT_TURN:
                if (d != NORTH && d != EAST) right(i, 1, 1);
                else left(i, 1, 1);
                break;
            case PK_SH_RIGHT_TURN:
                if (d != NORTH && d != EAST) left(i, 1, 1);
                else right(i, 1, 1);
                break;
            case PK_SH_ROOM_CROSSING:
                forward(i, 4, 1);
                left(i, 1, 4);
                right(i, 1, 4);
                break;
            case PK_SH_FIVE_CROSSING: {
                int lo = 3, hi = 5;
                if (d == WEST || d == NORTH) {
                    lo = 8 - lo;
                    hi = 8 - hi;
                }
                forward(i, 5, 1);
                if (a & 8) left(i, lo, 1);
                if (a & 16) left(i, hi, 7);
                if (a & 32) right(i, lo, 1);
                if (a & 64) right(i, hi, 7);
                break;
            }
            case PK_SH_PORTAL_ROOM: portalRoom = i; break;
            default: break;
        }
    }
};

}  // namespace

int strongholdPieces(int64_t worldSeed, int cx, int cz, Piece* out, int max) {
    if (max < MAX_PIECES) return 0;
    Gen* g = nullptr;
    for (int attempt = 0; attempt < 16; attempt++) {
        JavaRandom r;
        r.setLargeFeatureSeed(worldSeed, cx, cz + attempt);
        delete g;
        g = new Gen(r, out);
        if (!g) return 0;
        // StartPiece: a StairsDown at (2, 64, 2) in the chunk, the source
        int dir = r.nextInt(4);
        Piece& s = out[0];
        s = {};
        s.box = makeBox(cx * 16 + 2, 64, cz * 16 + 2, dir, 5, 11, 5);
        s.kind = PK_SH_STAIRS_DOWN;
        s.orient = (uint8_t)dir;
        s.a = OPENING | 8;
        g->n = 1;
        g->depth[0] = 0;
        g->children(0);
        while (g->npending > 0) {
            int j = r.nextInt(g->npending);
            int i = g->pending[j];
            for (int k = j; k < g->npending - 1; k++) g->pending[k] = g->pending[k + 1];
            g->npending--;
            g->children(i);
        }
        // StructurePiecesBuilder#moveBelowSeaLevel(63, -64, random, 10)
        SBox all = out[0].box;
        for (int i = 1; i < g->n; i++) {
            if (out[i].box.y0 < all.y0) all.y0 = out[i].box.y0;
            if (out[i].box.y1 > all.y1) all.y1 = out[i].box.y1;
        }
        int top = 63 - 10, j = (all.y1 - all.y0 + 1) + (-64) + 1;
        if (j < top) j += r.nextInt(top - j);
        int k = j - all.y1;
        for (int i = 0; i < g->n; i++) out[i].box.moveY(k);
        if (g->portalRoom >= 0) {
            int n = g->n;
            delete g;
            return n;
        }
    }
    delete g;
    return 0;
}

// ------------------------------------------------------------------ the pieces' blocks
namespace {

// StrongholdPieces.SmoothStoneSelector: the walls stone bricks, cracked, mossy or infested
uint16_t smoothStone(Build& b, int x, int y, int z, bool wall) {
    if (!wall) return B[B_CAVE_AIR];
    float f = posRandom(b.seed, b.wx(x, z), b.wy(y), b.wz(x, z), 7);
    return f < 0.2f ? B[B_CRACKED_STONE_BRICKS] : f < 0.5f ? B[B_MOSSY_STONE_BRICKS] : f < 0.55f ? B[B_INFESTED_STONE_BRICKS]
                                                                                                : B[B_STONE_BRICKS];
}
// generateBox with the selector; keepAir: not where there is air already (caves cut strongholds)
void smoothBox(Build& b, int x0, int y0, int z0, int x1, int y1, int z1, bool keepAir) {
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            for (int z = z0; z <= z1; z++) {
                if (!b.inside(x, y, z) || (keepAir && stateIsAir(b.get(x, y, z)))) continue;
                b.set(x, y, z, smoothStone(b, x, y, z, y == y0 || y == y1 || x == x0 || x == x1 || z == z0 || z == z1));
            }
}
void maybe(Build& b, float chance, int x, int y, int z, uint16_t st) {
    if (b.inside(x, y, z) && posRandom(b.seed, b.wx(x, z), b.wy(y), b.wz(x, z), 11) < chance) b.set(x, y, z, st);
}
uint16_t bars(bool n, bool e, bool s, bool w) {
    uint16_t st = B[B_IRON_BARS];
    st = setBool(st, "north", n);
    st = setBool(st, "east", e);
    st = setBool(st, "south", s);
    return setBool(st, "west", w);
}
uint16_t fence(bool n, bool e, bool s, bool w) {
    uint16_t st = B[B_OAK_FENCE];
    st = setBool(st, "north", n);
    st = setBool(st, "east", e);
    st = setBool(st, "south", s);
    return setBool(st, "west", w);
}
uint16_t door(uint16_t base, int dir, bool upper) { return setPropStr(facing(base, dir), "half", upper ? "upper" : "lower"); }
uint16_t slab() { return B[B_SMOOTH_STONE_SLAB]; }

// StrongholdPiece#generateSmallDoor
void smallDoor(Build& b, int type, int x, int y, int z) {
    uint16_t SB = B[B_STONE_BRICKS];
    switch (type) {
        case OPENING: b.box(x, y, z, x + 2, y + 2, z, B[B_CAVE_AIR], B[B_CAVE_AIR]); break;
        case WOOD_DOOR: case IRON_DOOR:
            b.set(x, y, z, SB); b.set(x, y + 1, z, SB); b.set(x, y + 2, z, SB); b.set(x + 1, y + 2, z, SB);
            b.set(x + 2, y + 2, z, SB); b.set(x + 2, y + 1, z, SB); b.set(x + 2, y, z, SB);
            if (type == WOOD_DOOR) {
                b.set(x + 1, y, z, door(B[B_OAK_DOOR], NORTH, false));
                b.set(x + 1, y + 1, z, door(B[B_OAK_DOOR], NORTH, true));
            } else {
                b.set(x + 1, y, z, door(B[B_IRON_DOOR], NORTH, false));
                b.set(x + 1, y + 1, z, door(B[B_IRON_DOOR], NORTH, true));
                b.set(x + 2, y + 1, z + 1, facing(B[B_STONE_BUTTON], NORTH));
                b.set(x + 2, y + 1, z - 1, facing(B[B_STONE_BUTTON], SOUTH));
            }
            break;
        case GRATES:
            b.set(x + 1, y, z, B[B_CAVE_AIR]); b.set(x + 1, y + 1, z, B[B_CAVE_AIR]);
            b.set(x, y, z, bars(false, false, false, true)); b.set(x, y + 1, z, bars(false, false, false, true));
            b.set(x, y + 2, z, bars(false, true, false, true)); b.set(x + 1, y + 2, z, bars(false, true, false, true));
            b.set(x + 2, y + 2, z, bars(false, true, false, true));
            b.set(x + 2, y + 1, z, bars(false, true, false, false)); b.set(x + 2, y, z, bars(false, true, false, false));
            break;
        default: break;
    }
}

void airBox(Build& b, int x0, int y0, int z0, int x1, int y1, int z1) { b.box(x0, y0, z0, x1, y1, z1, B[B_CAVE_AIR], B[B_CAVE_AIR]); }

void straight(Build& b, int a) {
    smoothBox(b, 0, 0, 0, 4, 4, 6, true);
    smallDoor(b, a & 7, 1, 1, 0);
    smallDoor(b, OPENING, 1, 1, 6);
    uint16_t tE = facing(B[B_WALL_TORCH], EAST), tW = facing(B[B_WALL_TORCH], WEST);
    maybe(b, 0.1f, 1, 2, 1, tE); maybe(b, 0.1f, 3, 2, 1, tW); maybe(b, 0.1f, 1, 2, 5, tE); maybe(b, 0.1f, 3, 2, 5, tW);
    if (a & 8) airBox(b, 0, 1, 2, 0, 3, 4);
    if (a & 16) airBox(b, 4, 1, 2, 4, 3, 4);
}

void prisonHall(Build& b, int a) {
    smoothBox(b, 0, 0, 0, 8, 4, 10, true);
    smallDoor(b, a & 7, 1, 1, 0);
    airBox(b, 1, 1, 10, 3, 3, 10);
    smoothBox(b, 4, 1, 1, 4, 3, 1, false);
    smoothBox(b, 4, 1, 3, 4, 3, 3, false);
    smoothBox(b, 4, 1, 7, 4, 3, 7, false);
    smoothBox(b, 4, 1, 9, 4, 3, 9, false);
    for (int i = 1; i <= 3; i++) {
        b.set(4, i, 4, bars(true, false, true, false));
        b.set(4, i, 5, bars(true, true, true, false));
        b.set(4, i, 6, bars(true, false, true, false));
        b.set(5, i, 5, bars(false, true, false, true));
        b.set(6, i, 5, bars(false, true, false, true));
        b.set(7, i, 5, bars(false, true, false, true));
    }
    b.set(4, 3, 2, bars(true, false, true, false));
    b.set(4, 3, 8, bars(true, false, true, false));
    b.set(4, 1, 2, door(B[B_IRON_DOOR], WEST, false)); b.set(4, 2, 2, door(B[B_IRON_DOOR], WEST, true));
    b.set(4, 1, 8, door(B[B_IRON_DOOR], WEST, false)); b.set(4, 2, 8, door(B[B_IRON_DOOR], WEST, true));
}

void turn(Build& b, int a, bool leftTurn) {
    smoothBox(b, 0, 0, 0, 4, 4, 4, true);
    smallDoor(b, a & 7, 1, 1, 0);
    bool ne = b.p.orient == NORTH || b.p.orient == EAST;
    bool openRight = leftTurn ? !ne : ne;   // the side its child leaves by
    if (openRight) airBox(b, 4, 1, 1, 4, 3, 3);
    else airBox(b, 0, 1, 1, 0, 3, 3);
}

void roomCrossing(Build& b, int a) {
    smoothBox(b, 0, 0, 0, 10, 6, 10, true);
    smallDoor(b, a & 7, 4, 1, 0);
    airBox(b, 4, 1, 10, 6, 3, 10);
    airBox(b, 0, 1, 4, 0, 3, 6);
    airBox(b, 10, 1, 4, 10, 3, 6);
    uint16_t SB = B[B_STONE_BRICKS], CO = B[B_COBBLESTONE];
    switch ((a >> 8) & 7) {
        case 0:
            b.set(5, 1, 5, SB); b.set(5, 2, 5, SB); b.set(5, 3, 5, SB);
            b.set(4, 3, 5, facing(B[B_WALL_TORCH], WEST)); b.set(6, 3, 5, facing(B[B_WALL_TORCH], EAST));
            b.set(5, 3, 4, facing(B[B_WALL_TORCH], SOUTH)); b.set(5, 3, 6, facing(B[B_WALL_TORCH], NORTH));
            b.set(4, 1, 4, slab()); b.set(4, 1, 5, slab()); b.set(4, 1, 6, slab()); b.set(6, 1, 4, slab());
            b.set(6, 1, 5, slab()); b.set(6, 1, 6, slab()); b.set(5, 1, 4, slab()); b.set(5, 1, 6, slab());
            break;
        case 1:
            for (int i = 0; i < 5; i++) {
                b.set(3, 1, 3 + i, SB); b.set(7, 1, 3 + i, SB); b.set(3 + i, 1, 3, SB); b.set(3 + i, 1, 7, SB);
            }
            b.set(5, 1, 5, SB); b.set(5, 2, 5, SB); b.set(5, 3, 5, SB);
            b.set(5, 4, 5, B[B_WATER]);
            break;
        case 2:
            for (int i = 1; i <= 9; i++) { b.set(1, 3, i, CO); b.set(9, 3, i, CO); }
            for (int i = 1; i <= 9; i++) { b.set(i, 3, 1, CO); b.set(i, 3, 9, CO); }
            b.set(5, 1, 4, CO); b.set(5, 1, 6, CO); b.set(5, 3, 4, CO); b.set(5, 3, 6, CO);
            b.set(4, 1, 5, CO); b.set(6, 1, 5, CO); b.set(4, 3, 5, CO); b.set(6, 3, 5, CO);
            for (int i = 1; i <= 3; i++) { b.set(4, i, 4, CO); b.set(6, i, 4, CO); b.set(4, i, 6, CO); b.set(6, i, 6, CO); }
            b.set(5, 3, 5, B[B_WALL_TORCH]);
            for (int i = 2; i <= 8; i++) {
                b.set(2, 3, i, B[B_OAK_PLANKS]); b.set(3, 3, i, B[B_OAK_PLANKS]);
                if (i <= 3 || i >= 7) { b.set(4, 3, i, B[B_OAK_PLANKS]); b.set(5, 3, i, B[B_OAK_PLANKS]); b.set(6, 3, i, B[B_OAK_PLANKS]); }
                b.set(7, 3, i, B[B_OAK_PLANKS]); b.set(8, 3, i, B[B_OAK_PLANKS]);
            }
            b.set(9, 1, 3, facing(B[B_LADDER], WEST)); b.set(9, 2, 3, facing(B[B_LADDER], WEST)); b.set(9, 3, 3, facing(B[B_LADDER], WEST));
            b.chest(3, 4, 8, "stronghold_crossing", 0);
            break;
        default: break;
    }
}

void straightStairsDown(Build& b, int a) {
    smoothBox(b, 0, 0, 0, 4, 10, 7, true);
    smallDoor(b, a & 7, 1, 7, 0);
    smallDoor(b, OPENING, 1, 1, 7);
    uint16_t st = facing(B[B_COBBLESTONE_STAIRS], SOUTH);
    for (int i = 0; i < 6; i++) {
        b.set(1, 6 - i, 1 + i, st); b.set(2, 6 - i, 1 + i, st); b.set(3, 6 - i, 1 + i, st);
        if (i < 5) { b.set(1, 5 - i, 1 + i, B[B_STONE_BRICKS]); b.set(2, 5 - i, 1 + i, B[B_STONE_BRICKS]); b.set(3, 5 - i, 1 + i, B[B_STONE_BRICKS]); }
    }
}

void stairsDown(Build& b, int a) {
    smoothBox(b, 0, 0, 0, 4, 10, 4, true);
    smallDoor(b, a & 7, 1, 7, 0);
    smallDoor(b, OPENING, 1, 1, 4);
    uint16_t SB = B[B_STONE_BRICKS];
    b.set(2, 6, 1, SB); b.set(1, 5, 1, SB); b.set(1, 6, 1, slab()); b.set(1, 5, 2, SB); b.set(1, 4, 3, SB);
    b.set(1, 5, 3, slab()); b.set(2, 4, 3, SB); b.set(3, 3, 3, SB); b.set(3, 4, 3, slab()); b.set(3, 3, 2, SB);
    b.set(3, 2, 1, SB); b.set(3, 3, 1, slab()); b.set(2, 2, 1, SB); b.set(1, 1, 1, SB); b.set(1, 2, 1, slab());
    b.set(1, 1, 2, SB); b.set(1, 1, 3, slab());
}

void fiveCrossing(Build& b, int a) {
    smoothBox(b, 0, 0, 0, 9, 8, 10, true);
    smallDoor(b, a & 7, 4, 3, 0);
    if (a & 8) airBox(b, 0, 3, 1, 0, 5, 3);
    if (a & 32) airBox(b, 9, 3, 1, 9, 5, 3);
    if (a & 16) airBox(b, 0, 5, 7, 0, 7, 9);
    if (a & 64) airBox(b, 9, 5, 7, 9, 7, 9);
    airBox(b, 5, 1, 10, 7, 3, 10);
    smoothBox(b, 1, 2, 1, 8, 2, 6, false);
    smoothBox(b, 4, 1, 5, 4, 4, 9, false);
    smoothBox(b, 8, 1, 5, 8, 4, 9, false);
    smoothBox(b, 1, 4, 7, 3, 4, 9, false);
    smoothBox(b, 1, 3, 5, 3, 3, 6, false);
    b.box(1, 3, 4, 3, 3, 4, slab(), slab());
    b.box(1, 4, 6, 3, 4, 6, slab(), slab());
    smoothBox(b, 5, 1, 7, 7, 1, 8, false);
    b.box(5, 1, 9, 7, 1, 9, slab(), slab());
    b.box(5, 2, 7, 7, 2, 7, slab(), slab());
    b.box(4, 5, 7, 4, 5, 9, slab(), slab());
    b.box(8, 5, 7, 8, 5, 9, slab(), slab());
    uint16_t dbl = setPropStr(slab(), "type", "double");
    b.box(5, 5, 7, 7, 5, 9, dbl, dbl);
    b.set(6, 5, 6, facing(B[B_WALL_TORCH], SOUTH));
}

void chestCorridor(Build& b, int a) {
    smoothBox(b, 0, 0, 0, 4, 4, 6, true);
    smallDoor(b, a & 7, 1, 1, 0);
    smallDoor(b, OPENING, 1, 1, 6);
    b.box(3, 1, 2, 3, 1, 4, B[B_STONE_BRICKS], B[B_STONE_BRICKS]);
    b.set(3, 1, 1, slab()); b.set(3, 1, 5, slab()); b.set(3, 2, 2, slab()); b.set(3, 2, 4, slab());
    for (int i = 2; i <= 4; i++) b.set(2, 1, i, slab());
    b.chest(3, 2, 3, "stronghold_corridor", 0);
}

void library(Build& b, int a) {
    bool tall = a & 8;
    int h = tall ? 11 : 6;
    smoothBox(b, 0, 0, 0, 13, h - 1, 14, true);
    smallDoor(b, a & 7, 4, 1, 0);
    // generateMaybeBox: cobwebs at random (7 %) where there is no air already... vanilla: any block
    for (int y = 1; y <= 4; y++)
        for (int x = 2; x <= 11; x++)
            for (int z = 1; z <= 13; z++)
                if (b.inside(x, y, z) && posRandom(b.seed, b.wx(x, z), b.wy(y), b.wz(x, z), 13) <= 0.07f) b.set(x, y, z, B[B_COBWEB]);
    uint16_t P = B[B_OAK_PLANKS], BS = B[B_BOOKSHELF];
    for (int l = 1; l <= 13; l++) {
        if ((l - 1) % 4 == 0) {
            b.box(1, 1, l, 1, 4, l, P, P);
            b.box(12, 1, l, 12, 4, l, P, P);
            b.set(2, 3, l, facing(B[B_WALL_TORCH], EAST));
            b.set(11, 3, l, facing(B[B_WALL_TORCH], WEST));
            if (tall) { b.box(1, 6, l, 1, 9, l, P, P); b.box(12, 6, l, 12, 9, l, P, P); }
        } else {
            b.box(1, 1, l, 1, 4, l, BS, BS);
            b.box(12, 1, l, 12, 4, l, BS, BS);
            if (tall) { b.box(1, 6, l, 1, 9, l, BS, BS); b.box(12, 6, l, 12, 9, l, BS, BS); }
        }
    }
    for (int k = 3; k < 12; k += 2) {
        b.box(3, 1, k, 4, 3, k, BS, BS);
        b.box(6, 1, k, 7, 3, k, BS, BS);
        b.box(9, 1, k, 10, 3, k, BS, BS);
    }
    if (tall) {
        b.box(1, 5, 1, 3, 5, 13, P, P);
        b.box(10, 5, 1, 12, 5, 13, P, P);
        b.box(4, 5, 1, 9, 5, 2, P, P);
        b.box(4, 5, 12, 9, 5, 13, P, P);
        b.set(9, 5, 11, P); b.set(8, 5, 11, P); b.set(9, 5, 10, P);
        uint16_t fWE = fence(false, true, false, true), fNS = fence(true, false, true, false);
        b.box(3, 6, 3, 3, 6, 11, fNS, fNS);
        b.box(10, 6, 3, 10, 6, 9, fNS, fNS);
        b.box(4, 6, 2, 9, 6, 2, fWE, fWE);
        b.box(4, 6, 12, 7, 6, 12, fWE, fWE);
        b.set(3, 6, 2, fence(false, true, true, false));
        b.set(3, 6, 12, fence(true, true, false, false));
        b.set(10, 6, 2, fence(false, false, true, true));
        for (int i = 0; i <= 2; i++) {
            b.set(8 + i, 6, 12 - i, fence(true, false, false, true));
            if (i != 2) b.set(8 + i, 6, 11 - i, fence(false, true, true, false));
        }
        uint16_t ladder = facing(B[B_LADDER], SOUTH);
        for (int y = 1; y <= 7; y++) b.set(10, y, 13, ladder);
        uint16_t F = B[B_OAK_FENCE];
        const int i1 = 7, j1 = 7;
        b.set(i1 - 1, 9, j1, F); b.set(i1, 9, j1, F); b.set(i1 - 1, 8, j1, F); b.set(i1, 8, j1, F);
        b.set(i1 - 1, 7, j1, F); b.set(i1, 7, j1, F); b.set(i1 - 2, 7, j1, F); b.set(i1 + 1, 7, j1, F);
        b.set(i1 - 1, 7, j1 - 1, F); b.set(i1, 7, j1 - 1, F); b.set(i1 - 1, 7, j1 + 1, F); b.set(i1, 7, j1 + 1, F);
        uint16_t T = B[B_TORCH];
        b.set(i1 - 2, 8, j1, T); b.set(i1 + 1, 8, j1, T); b.set(i1 - 1, 8, j1 - 1, T); b.set(i1, 8, j1 - 1, T);
        b.set(i1 - 1, 8, j1 + 1, T); b.set(i1, 8, j1 + 1, T);
    }
    b.chest(3, 3, 5, "stronghold_library", 0);
    if (tall) {
        b.set(12, 9, 1, B[B_CAVE_AIR]);
        b.chest(12, 8, 1, "stronghold_library", 1);
    }
}

void portalRoom(Build& b) {
    smoothBox(b, 0, 0, 0, 10, 7, 15, false);
    smallDoor(b, GRATES, 4, 1, 0);
    const int i = 6;
    smoothBox(b, 1, i, 1, 1, i, 14, false);
    smoothBox(b, 9, i, 1, 9, i, 14, false);
    smoothBox(b, 2, i, 1, 8, i, 2, false);
    smoothBox(b, 2, i, 14, 8, i, 14, false);
    smoothBox(b, 1, 1, 1, 2, 1, 4, false);
    smoothBox(b, 8, 1, 1, 9, 1, 4, false);
    b.box(1, 1, 1, 1, 1, 3, B[B_LAVA], B[B_LAVA]);
    b.box(9, 1, 1, 9, 1, 3, B[B_LAVA], B[B_LAVA]);
    smoothBox(b, 3, 1, 8, 7, 1, 12, false);
    b.box(4, 1, 9, 6, 1, 11, B[B_LAVA], B[B_LAVA]);
    uint16_t bNS = bars(true, false, true, false), bWE = bars(false, true, false, true);
    for (int j = 3; j < 14; j += 2) {
        b.box(0, 3, j, 0, 4, j, bNS, bNS);
        b.box(10, 3, j, 10, 4, j, bNS, bNS);
    }
    for (int k = 2; k < 9; k += 2) b.box(k, 3, 15, k, 4, 15, bWE, bWE);
    uint16_t st = facing(B[B_STONE_BRICK_STAIRS], NORTH);
    smoothBox(b, 4, 1, 5, 6, 1, 7, false);
    smoothBox(b, 4, 2, 6, 6, 2, 7, false);
    smoothBox(b, 4, 3, 7, 6, 3, 7, false);
    for (int l = 4; l <= 6; l++) {
        b.set(l, 1, 4, st);
        b.set(l, 2, 5, st);
        b.set(l, 3, 6, st);
    }
    // the frame: an eye in each at 10 % (all twelve: the portal is open)
    JavaRandom r(b.seed);
    bool eye[12], all = true;
    for (int m = 0; m < 12; m++) {
        eye[m] = r.nextFloat() > 0.9f;
        all = all && eye[m];
    }
    auto frame = [&](int dir, int m) { return setBool(facing(B[B_END_PORTAL_FRAME], dir), "eye", eye[m]); };
    b.set(4, 3, 8, frame(NORTH, 0)); b.set(5, 3, 8, frame(NORTH, 1)); b.set(6, 3, 8, frame(NORTH, 2));
    b.set(4, 3, 12, frame(SOUTH, 3)); b.set(5, 3, 12, frame(SOUTH, 4)); b.set(6, 3, 12, frame(SOUTH, 5));
    b.set(3, 3, 9, frame(EAST, 6)); b.set(3, 3, 10, frame(EAST, 7)); b.set(3, 3, 11, frame(EAST, 8));
    b.set(7, 3, 9, frame(WEST, 9)); b.set(7, 3, 10, frame(WEST, 10)); b.set(7, 3, 11, frame(WEST, 11));
    if (all)
        for (int z = 9; z <= 11; z++)
            for (int x = 4; x <= 6; x++) b.set(x, 3, z, B[B_END_PORTAL]);
    b.spawner(5, 3, 6, "silverfish");
}

void fillerCorridor(Build& b, int steps) {
    uint16_t SB = B[B_STONE_BRICKS], A = B[B_CAVE_AIR];
    for (int i = 0; i < steps; i++) {
        for (int x = 0; x <= 4; x++) b.set(x, 0, i, SB);
        for (int j = 1; j <= 3; j++) {
            b.set(0, j, i, SB);
            b.set(1, j, i, A); b.set(2, j, i, A); b.set(3, j, i, A);
            b.set(4, j, i, SB);
        }
        for (int x = 0; x <= 4; x++) b.set(x, 4, i, SB);
    }
}

}  // namespace

void buildStronghold(Build& b) {
    int a = b.p.a;
    switch (b.p.kind) {
        case PK_SH_STRAIGHT: straight(b, a); break;
        case PK_SH_PRISON_HALL: prisonHall(b, a); break;
        case PK_SH_LEFT_TURN: turn(b, a, true); break;
        case PK_SH_RIGHT_TURN: turn(b, a, false); break;
        case PK_SH_ROOM_CROSSING: roomCrossing(b, a); break;
        case PK_SH_STRAIGHT_STAIRS_DOWN: straightStairsDown(b, a); break;
        case PK_SH_STAIRS_DOWN: stairsDown(b, a); break;
        case PK_SH_FIVE_CROSSING: fiveCrossing(b, a); break;
        case PK_SH_CHEST_CORRIDOR: chestCorridor(b, a); break;
        case PK_SH_LIBRARY: library(b, a); break;
        case PK_SH_PORTAL_ROOM: portalRoom(b); break;
        case PK_SH_FILLER_CORRIDOR: fillerCorridor(b, a); break;
        default: break;
    }
}

}  // namespace sb
}  // namespace vanilla
}  // namespace mc
