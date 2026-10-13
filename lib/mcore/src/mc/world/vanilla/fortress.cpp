// The Nether fortresses: vanilla's NetherFortressPieces and NetherFortressStructure
// (1.21.8): the bridge and castle pieces with their weights, limits, "in a row" rules and
// order of random numbers, so a start gives vanilla's layout for the seed the generator's
// Random is given; each piece's blocks as its postProcess places them.
#include <stdlib.h>
#include "mc/platform.h"
#include "mc/world/vanilla/structure_build.h"

namespace mc {
namespace vanilla {
namespace sb {

namespace {

struct Weight { uint8_t kind; int weight, maxCount; bool inRow; };
// NetherFortressPieces.BRIDGE_PIECE_WEIGHTS and CASTLE_PIECE_WEIGHTS
const Weight BRIDGE[6] = {
    {PK_NF_BRIDGE_STRAIGHT, 30, 0, true}, {PK_NF_BRIDGE_CROSSING, 10, 4, false}, {PK_NF_ROOM_CROSSING, 10, 4, false},
    {PK_NF_STAIRS_ROOM, 10, 3, false},    {PK_NF_MONSTER_THRONE, 5, 2, false},   {PK_NF_CASTLE_ENTRANCE, 5, 1, false},
};
const Weight CASTLE[7] = {
    {PK_NF_CORRIDOR, 25, 0, true},           {PK_NF_CORRIDOR_CROSSING, 15, 5, false}, {PK_NF_CORRIDOR_RIGHT_TURN, 5, 10, false},
    {PK_NF_CORRIDOR_LEFT_TURN, 5, 10, false}, {PK_NF_CORRIDOR_STAIRS, 10, 3, true},   {PK_NF_CORRIDOR_T_BALCONY, 7, 2, false},
    {PK_NF_STALK_ROOM, 5, 2, false},
};

constexpr int MAX_PIECES = 600;

struct List {
    const Weight* w;
    int idx[8], n = 0, placed[8] = {};
    void init(const Weight* weights, int count) {
        w = weights;
        n = 0;
        for (int i = 0; i < count; i++) idx[n++] = i;
    }
};

struct Gen {
    JavaRandom& r;
    Piece* p;
    int n = 0;
    int depth[MAX_PIECES];
    List bridge, castle;
    const Weight* previous = nullptr;   // StartPiece.previousPiece
    int pending[MAX_PIECES], npending = 0;
    Gen(JavaRandom& rnd, Piece* out) : r(rnd), p(out) {
        bridge.init(BRIDGE, 6);
        castle.init(CASTLE, 7);
    }
    bool collides(const SBox& b) const {
        for (int i = 0; i < n; i++)
            if (p[i].box.hits(b)) return true;
        return false;
    }
    // the piece's box, or none (isOkBox and no collision), and its constructor's numbers
    bool create(int kind, int x, int y, int z, int dir, Piece& out) {
        SBox b;
        switch (kind) {
            case PK_NF_BRIDGE_STRAIGHT: b = orientBox(x, y, z, -1, -3, 0, 5, 10, 19, dir); break;
            case PK_NF_BRIDGE_END_FILLER: b = orientBox(x, y, z, -1, -3, 0, 5, 10, 8, dir); break;
            case PK_NF_BRIDGE_CROSSING: b = orientBox(x, y, z, -8, -3, 0, 19, 10, 19, dir); break;
            case PK_NF_ROOM_CROSSING: b = orientBox(x, y, z, -2, 0, 0, 7, 9, 7, dir); break;
            case PK_NF_STAIRS_ROOM: b = orientBox(x, y, z, -2, 0, 0, 7, 11, 7, dir); break;
            case PK_NF_MONSTER_THRONE: b = orientBox(x, y, z, -2, 0, 0, 7, 8, 9, dir); break;
            case PK_NF_CASTLE_ENTRANCE: case PK_NF_STALK_ROOM: b = orientBox(x, y, z, -5, -3, 0, 13, 14, 13, dir); break;
            case PK_NF_CORRIDOR: case PK_NF_CORRIDOR_CROSSING: case PK_NF_CORRIDOR_RIGHT_TURN: case PK_NF_CORRIDOR_LEFT_TURN:
                b = orientBox(x, y, z, -1, 0, 0, 5, 7, 5, dir);
                break;
            case PK_NF_CORRIDOR_STAIRS: b = orientBox(x, y, z, -1, -7, 0, 5, 14, 10, dir); break;
            case PK_NF_CORRIDOR_T_BALCONY: b = orientBox(x, y, z, -3, 0, 0, 9, 7, 9, dir); break;
            default: return false;
        }
        if (b.y0 <= 10 || collides(b)) return false;
        out = {};
        out.box = b;
        out.kind = (uint8_t)kind;
        out.orient = (uint8_t)dir;
        if (kind == PK_NF_BRIDGE_END_FILLER) out.b = r.nextInt();   // selfSeed
        if (kind == PK_NF_CORRIDOR_RIGHT_TURN || kind == PK_NF_CORRIDOR_LEFT_TURN) out.a = r.nextInt(3) == 0 ? 1 : 0;   // a chest
        return true;
    }
    // NetherBridgePiece#generatePiece
    bool generate(List& l, int x, int y, int z, int dir, int genDepth, Piece& out) {
        int total = 0;
        bool any = false;
        for (int i = 0; i < l.n; i++) {
            const Weight& w = l.w[l.idx[i]];
            if (w.maxCount > 0 && l.placed[l.idx[i]] < w.maxCount) any = true;
            total += w.weight;
        }
        bool ok = any && total > 0 && genDepth <= 30;
        for (int tries = 0; tries < 5 && ok; tries++) {
            int k = r.nextInt(total);
            for (int li = 0; li < l.n; li++) {
                int wi = l.idx[li];
                const Weight& w = l.w[wi];
                k -= w.weight;
                if (k < 0) {
                    bool can = w.maxCount == 0 || l.placed[wi] < w.maxCount;
                    if (!can || (&w == previous && !w.inRow)) break;
                    if (create(w.kind, x, y, z, dir, out)) {
                        l.placed[wi]++;
                        previous = &w;
                        if (w.maxCount > 0 && l.placed[wi] >= w.maxCount) {
                            for (int m = li; m < l.n - 1; m++) l.idx[m] = l.idx[m + 1];
                            l.n--;
                        }
                        return true;
                    }
                }
            }
        }
        return create(PK_NF_BRIDGE_END_FILLER, x, y, z, dir, out);
    }
    // NetherBridgePiece#generateAndAddPiece
    void addPiece(int x, int y, int z, int dir, int genDepth, bool castlePiece) {
        Piece out;
        if (abs(x - p[0].box.x0) > 112 || abs(z - p[0].box.z0) > 112) {
            create(PK_NF_BRIDGE_END_FILLER, x, y, z, dir, out);   // made, not added (its number still drawn)
            return;
        }
        if (n >= MAX_PIECES) return;
        if (!generate(castlePiece ? castle : bridge, x, y, z, dir, genDepth + 1, out)) return;
        p[n] = out;
        depth[n] = genDepth + 1;
        pending[npending++] = n;
        n++;
    }
    void forward(int i, int ox, int oy, bool castle) {
        const SBox b = p[i].box;
        int d = p[i].orient, g = depth[i];
        switch (d) {
            case NORTH: addPiece(b.x0 + ox, b.y0 + oy, b.z0 - 1, d, g, castle); break;
            case SOUTH: addPiece(b.x0 + ox, b.y0 + oy, b.z1 + 1, d, g, castle); break;
            case WEST: addPiece(b.x0 - 1, b.y0 + oy, b.z0 + ox, d, g, castle); break;
            default: addPiece(b.x1 + 1, b.y0 + oy, b.z0 + ox, d, g, castle); break;
        }
    }
    void left(int i, int oy, int ox, bool castle) {
        const SBox b = p[i].box;
        int d = p[i].orient, g = depth[i];
        if (d == NORTH || d == SOUTH) addPiece(b.x0 - 1, b.y0 + oy, b.z0 + ox, WEST, g, castle);
        else addPiece(b.x0 + ox, b.y0 + oy, b.z0 - 1, NORTH, g, castle);
    }
    void right(int i, int oy, int ox, bool castle) {
        const SBox b = p[i].box;
        int d = p[i].orient, g = depth[i];
        if (d == NORTH || d == SOUTH) addPiece(b.x1 + 1, b.y0 + oy, b.z0 + ox, EAST, g, castle);
        else addPiece(b.x0 + ox, b.y0 + oy, b.z1 + 1, SOUTH, g, castle);
    }
    void children(int i) {
        int d = p[i].orient;
        switch (p[i].kind) {
            case PK_NF_BRIDGE_CROSSING:
                forward(i, 8, 3, false);
                left(i, 3, 8, false);
                right(i, 3, 8, false);
                break;
            case PK_NF_BRIDGE_STRAIGHT: forward(i, 1, 3, false); break;
            case PK_NF_ROOM_CROSSING:
                forward(i, 2, 0, false);
                left(i, 0, 2, false);
                right(i, 0, 2, false);
                break;
            case PK_NF_STAIRS_ROOM: right(i, 6, 2, false); break;
            case PK_NF_CASTLE_ENTRANCE: forward(i, 5, 3, true); break;
            case PK_NF_STALK_ROOM:
                forward(i, 5, 3, true);
                forward(i, 5, 11, true);
                break;
            case PK_NF_CORRIDOR: case PK_NF_CORRIDOR_STAIRS: forward(i, 1, 0, true); break;
            case PK_NF_CORRIDOR_CROSSING:
                forward(i, 1, 0, true);
                left(i, 0, 1, true);
                right(i, 0, 1, true);
                break;
            case PK_NF_CORRIDOR_RIGHT_TURN: right(i, 0, 1, true); break;
            case PK_NF_CORRIDOR_LEFT_TURN: left(i, 0, 1, true); break;
            case PK_NF_CORRIDOR_T_BALCONY: {
                int off = d == WEST || d == NORTH ? 5 : 1;
                bool c1 = r.nextInt(8) > 0;
                left(i, 0, off, c1);
                bool c2 = r.nextInt(8) > 0;
                right(i, 0, off, c2);
                break;
            }
            default: break;
        }
    }
};

}  // namespace

int fortressPieces(int64_t worldSeed, int cx, int cz, Piece* out, int max) {
    if (max < MAX_PIECES) return 0;
    JavaRandom r;
    r.setLargeFeatureSeed(worldSeed, cx, cz);
    Gen* g = new Gen(r, out);
    if (!g) return 0;
    // StartPiece: a bridge crossing at (2, 64, 2) in the chunk
    int dir = r.nextInt(4);
    Piece& s = out[0];
    s = {};
    s.box = makeBox(cx * 16 + 2, 64, cz * 16 + 2, dir, 19, 10, 19);
    s.kind = PK_NF_BRIDGE_CROSSING;
    s.orient = (uint8_t)dir;
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
    // StructurePiecesBuilder#moveInsideHeights(random, 48, 70)
    SBox all = out[0].box;
    for (int i = 1; i < g->n; i++) {
        if (out[i].box.y0 < all.y0) all.y0 = out[i].box.y0;
        if (out[i].box.y1 > all.y1) all.y1 = out[i].box.y1;
    }
    int room = 70 - 48 + 1 - (all.y1 - all.y0 + 1);
    int y = room > 1 ? 48 + r.nextInt(room) : 48;
    for (int i = 0; i < g->n; i++) out[i].box.moveY(y - all.y0);
    int n = g->n;
    delete g;
    return n;
}

// ------------------------------------------------------------------ the pieces' blocks
namespace {

uint16_t fence(bool n, bool e, bool s, bool w) {
    uint16_t st = B[B_NETHER_BRICK_FENCE];
    st = setBool(st, "north", n);
    st = setBool(st, "east", e);
    st = setBool(st, "south", s);
    return setBool(st, "west", w);
}
void nb(Build& b, int x0, int y0, int z0, int x1, int y1, int z1) { b.box(x0, y0, z0, x1, y1, z1, B[B_NETHER_BRICKS], B[B_NETHER_BRICKS]); }
void air(Build& b, int x0, int y0, int z0, int x1, int y1, int z1) { b.box(x0, y0, z0, x1, y1, z1, 0, 0); }
void fbox(Build& b, int x0, int y0, int z0, int x1, int y1, int z1, uint16_t f) { b.box(x0, y0, z0, x1, y1, z1, f, f); }
void pillars(Build& b, int w, int d) {   // fillColumnDown under the whole floor
    for (int i = 0; i < w; i++)
        for (int j = 0; j < d; j++) b.columnDown(B[B_NETHER_BRICKS], i, -1, j);
}

void bridgeStraight(Build& b) {
    nb(b, 0, 3, 0, 4, 4, 18);
    air(b, 1, 5, 0, 3, 7, 18);
    nb(b, 0, 5, 0, 0, 5, 18);
    nb(b, 4, 5, 0, 4, 5, 18);
    nb(b, 0, 2, 0, 4, 2, 5);
    nb(b, 0, 2, 13, 4, 2, 18);
    nb(b, 0, 0, 0, 4, 1, 3);
    nb(b, 0, 0, 15, 4, 1, 18);
    for (int i = 0; i <= 4; i++)
        for (int j = 0; j <= 2; j++) {
            b.columnDown(B[B_NETHER_BRICKS], i, -1, j);
            b.columnDown(B[B_NETHER_BRICKS], i, -1, 18 - j);
        }
    uint16_t ns = fence(true, false, true, false);
    fbox(b, 0, 1, 1, 0, 4, 1, ns); fbox(b, 0, 3, 4, 0, 4, 4, ns); fbox(b, 0, 3, 14, 0, 4, 14, ns); fbox(b, 0, 1, 17, 0, 4, 17, ns);
    fbox(b, 4, 1, 1, 4, 4, 1, ns); fbox(b, 4, 3, 4, 4, 4, 4, ns); fbox(b, 4, 3, 14, 4, 4, 14, ns); fbox(b, 4, 1, 17, 4, 4, 17, ns);
}

void bridgeEndFiller(Build& b, int32_t selfSeed) {
    JavaRandom r(selfSeed);
    for (int i = 0; i <= 4; i++)
        for (int j = 3; j <= 4; j++) {
            int k = r.nextInt(8);
            nb(b, i, j, 0, i, j, k);
        }
    int l = r.nextInt(8);
    nb(b, 0, 5, 0, 0, 5, l);
    l = r.nextInt(8);
    nb(b, 4, 5, 0, 4, 5, l);
    for (l = 0; l <= 4; l++) {
        int i1 = r.nextInt(5);
        nb(b, l, 2, 0, l, 2, i1);
    }
    for (l = 0; l <= 4; l++)
        for (int j1 = 0; j1 <= 1; j1++) {
            int k1 = r.nextInt(3);
            nb(b, l, j1, 0, l, j1, k1);
        }
}

void bridgeCrossing(Build& b) {
    nb(b, 7, 3, 0, 11, 4, 18);
    nb(b, 0, 3, 7, 18, 4, 11);
    air(b, 8, 5, 0, 10, 7, 18);
    air(b, 0, 5, 8, 18, 7, 10);
    nb(b, 7, 5, 0, 7, 5, 7); nb(b, 7, 5, 11, 7, 5, 18); nb(b, 11, 5, 0, 11, 5, 7); nb(b, 11, 5, 11, 11, 5, 18);
    nb(b, 0, 5, 7, 7, 5, 7); nb(b, 11, 5, 7, 18, 5, 7); nb(b, 0, 5, 11, 7, 5, 11); nb(b, 11, 5, 11, 18, 5, 11);
    nb(b, 7, 2, 0, 11, 2, 0); nb(b, 7, 2, 18, 11, 2, 18);
    uint16_t we = fence(false, true, false, true), ns = fence(true, false, true, false);
    fbox(b, 7, 0, 0, 11, 1, 0, we); fbox(b, 7, 0, 18, 11, 1, 18, we);
    nb(b, 0, 2, 7, 0, 2, 11); nb(b, 18, 2, 7, 18, 2, 11);
    fbox(b, 0, 0, 7, 0, 1, 11, ns); fbox(b, 18, 0, 7, 18, 1, 11, ns);
    for (int i = 7; i <= 11; i++)
        for (int j = 0; j <= 2; j++) {
            b.columnDown(B[B_NETHER_BRICKS], i, -1, j);
            b.columnDown(B[B_NETHER_BRICKS], i, -1, 18 - j);
        }
    for (int k = 0; k <= 2; k++)
        for (int l = 7; l <= 11; l++) {
            b.columnDown(B[B_NETHER_BRICKS], k, -1, l);
            b.columnDown(B[B_NETHER_BRICKS], 18 - k, -1, l);
        }
}

void roomCrossing(Build& b) {
    nb(b, 0, 0, 0, 6, 1, 6);
    air(b, 0, 2, 0, 6, 7, 6);
    nb(b, 0, 2, 0, 1, 6, 0); nb(b, 0, 2, 6, 1, 6, 6); nb(b, 5, 2, 0, 6, 6, 0); nb(b, 5, 2, 6, 6, 6, 6);
    nb(b, 0, 2, 0, 0, 6, 1); nb(b, 0, 2, 5, 0, 6, 6); nb(b, 6, 2, 0, 6, 6, 1); nb(b, 6, 2, 5, 6, 6, 6);
    uint16_t we = fence(false, true, false, true), ns = fence(true, false, true, false);
    nb(b, 2, 6, 0, 4, 6, 0); fbox(b, 2, 5, 0, 4, 5, 0, we); nb(b, 2, 6, 6, 4, 6, 6); fbox(b, 2, 5, 6, 4, 5, 6, we);
    nb(b, 0, 6, 2, 0, 6, 4); fbox(b, 0, 5, 2, 0, 5, 4, ns); nb(b, 6, 6, 2, 6, 6, 4); fbox(b, 6, 5, 2, 6, 5, 4, ns);
    pillars(b, 7, 7);
}

void stairsRoom(Build& b) {
    nb(b, 0, 0, 0, 6, 1, 6);
    air(b, 0, 2, 0, 6, 10, 6);
    nb(b, 0, 2, 0, 1, 8, 0); nb(b, 5, 2, 0, 6, 8, 0); nb(b, 0, 2, 1, 0, 8, 6); nb(b, 6, 2, 1, 6, 8, 6); nb(b, 1, 2, 6, 5, 8, 6);
    uint16_t we = fence(false, true, false, true), ns = fence(true, false, true, false);
    fbox(b, 0, 3, 2, 0, 5, 4, ns); fbox(b, 6, 3, 2, 6, 5, 2, ns); fbox(b, 6, 3, 4, 6, 5, 4, ns);
    b.set(5, 2, 5, B[B_NETHER_BRICKS]);
    nb(b, 4, 2, 5, 4, 3, 5); nb(b, 3, 2, 5, 3, 4, 5); nb(b, 2, 2, 5, 2, 5, 5); nb(b, 1, 2, 5, 1, 6, 5);
    nb(b, 1, 7, 1, 5, 7, 4);
    air(b, 6, 8, 2, 6, 8, 4);
    nb(b, 2, 6, 0, 4, 8, 0);
    fbox(b, 2, 5, 0, 4, 5, 0, we);
    pillars(b, 7, 7);
}

void monsterThrone(Build& b) {
    air(b, 0, 2, 0, 6, 7, 7);
    nb(b, 1, 0, 0, 5, 1, 7); nb(b, 1, 2, 1, 5, 2, 7); nb(b, 1, 3, 2, 5, 3, 7); nb(b, 1, 4, 3, 5, 4, 7);
    nb(b, 1, 2, 0, 1, 4, 2); nb(b, 5, 2, 0, 5, 4, 2); nb(b, 1, 5, 2, 1, 5, 3); nb(b, 5, 5, 2, 5, 5, 3);
    nb(b, 0, 5, 3, 0, 5, 8); nb(b, 6, 5, 3, 6, 5, 8); nb(b, 1, 5, 8, 5, 5, 8);
    uint16_t we = fence(false, true, false, true), ns = fence(true, false, true, false);
    b.set(1, 6, 3, fence(false, false, false, true)); b.set(5, 6, 3, fence(false, true, false, false));
    b.set(0, 6, 3, fence(true, true, false, false)); b.set(6, 6, 3, fence(true, false, false, true));
    fbox(b, 0, 6, 4, 0, 6, 7, ns); fbox(b, 6, 6, 4, 6, 6, 7, ns);
    b.set(0, 6, 8, fence(false, true, true, false)); b.set(6, 6, 8, fence(false, false, true, true));
    fbox(b, 1, 6, 8, 5, 6, 8, we);
    b.set(1, 7, 8, fence(false, true, false, false)); fbox(b, 2, 7, 8, 4, 7, 8, we); b.set(5, 7, 8, fence(false, false, false, true));
    b.set(2, 8, 8, fence(false, true, false, false)); b.set(3, 8, 8, we); b.set(4, 8, 8, fence(false, false, false, true));
    b.spawner(3, 5, 5, "blaze");
    pillars(b, 7, 7);
}

// the castle's outer walls, battlements and floor (CastleEntrance and CastleStalkRoom)
void castleShell(Build& b) {
    nb(b, 0, 3, 0, 12, 4, 12);
    air(b, 0, 5, 0, 12, 13, 12);
    nb(b, 0, 5, 0, 1, 12, 12); nb(b, 11, 5, 0, 12, 12, 12);
    nb(b, 2, 5, 11, 4, 12, 12); nb(b, 8, 5, 11, 10, 12, 12); nb(b, 5, 9, 11, 7, 12, 12);
    nb(b, 2, 5, 0, 4, 12, 1); nb(b, 8, 5, 0, 10, 12, 1); nb(b, 5, 9, 0, 7, 12, 1);
    nb(b, 2, 11, 2, 10, 12, 10);
    uint16_t we = fence(false, true, false, true), ns = fence(true, false, true, false);
    for (int i = 1; i <= 11; i += 2) {
        fbox(b, i, 10, 0, i, 11, 0, we); fbox(b, i, 10, 12, i, 11, 12, we);
        fbox(b, 0, 10, i, 0, 11, i, ns); fbox(b, 12, 10, i, 12, 11, i, ns);
        b.set(i, 13, 0, B[B_NETHER_BRICKS]); b.set(i, 13, 12, B[B_NETHER_BRICKS]);
        b.set(0, 13, i, B[B_NETHER_BRICKS]); b.set(12, 13, i, B[B_NETHER_BRICKS]);
        if (i != 11) {
            b.set(i + 1, 13, 0, we); b.set(i + 1, 13, 12, we);
            b.set(0, 13, i + 1, ns); b.set(12, 13, i + 1, ns);
        }
    }
    b.set(0, 13, 0, fence(true, true, false, false)); b.set(0, 13, 12, fence(false, true, true, false));
    b.set(12, 13, 12, fence(false, false, true, true)); b.set(12, 13, 0, fence(true, false, false, true));
    for (int k = 3; k <= 9; k += 2) {
        fbox(b, 1, 7, k, 1, 8, k, fence(true, false, true, true));
        fbox(b, 11, 7, k, 11, 8, k, fence(true, true, true, false));
    }
}
void castleFloor(Build& b) {
    nb(b, 4, 2, 0, 8, 2, 12); nb(b, 0, 2, 4, 12, 2, 8);
    nb(b, 4, 0, 0, 8, 1, 3); nb(b, 4, 0, 9, 8, 1, 12); nb(b, 0, 0, 4, 3, 1, 8); nb(b, 9, 0, 4, 12, 1, 8);
    for (int l = 4; l <= 8; l++)
        for (int i1 = 0; i1 <= 2; i1++) {
            b.columnDown(B[B_NETHER_BRICKS], l, -1, i1);
            b.columnDown(B[B_NETHER_BRICKS], l, -1, 12 - i1);
        }
    for (int j1 = 0; j1 <= 2; j1++)
        for (int k1 = 4; k1 <= 8; k1++) {
            b.columnDown(B[B_NETHER_BRICKS], j1, -1, k1);
            b.columnDown(B[B_NETHER_BRICKS], 12 - j1, -1, k1);
        }
}

void castleEntrance(Build& b) {
    castleShell(b);
    fbox(b, 5, 8, 0, 7, 8, 0, fence(false, true, false, true));
    castleFloor(b);
    nb(b, 5, 5, 5, 7, 5, 7);
    air(b, 6, 1, 6, 6, 4, 6);
    b.set(6, 0, 6, B[B_NETHER_BRICKS]);
    b.set(6, 5, 6, B[B_LAVA]);   // the well: lava flows down it
}

void stalkRoom(Build& b) {
    castleShell(b);
    uint16_t sN = facing(B[B_NETHER_BRICK_STAIRS], NORTH);
    for (int l = 0; l <= 6; l++) {
        int i1 = l + 4;
        for (int j1 = 5; j1 <= 7; j1++) b.set(j1, 5 + l, i1, sN);
        if (i1 >= 5 && i1 <= 8) nb(b, 5, 5, i1, 7, l + 4, i1);
        else if (i1 >= 9 && i1 <= 10) nb(b, 5, 8, i1, 7, l + 4, i1);
        if (l >= 1) air(b, 5, 6 + l, i1, 7, 9 + l, i1);
    }
    for (int k1 = 5; k1 <= 7; k1++) b.set(k1, 12, 11, sN);
    fbox(b, 5, 6, 7, 5, 7, 7, fence(false, true, false, false));
    fbox(b, 7, 6, 7, 7, 7, 7, fence(false, false, false, true));
    air(b, 5, 13, 12, 7, 13, 12);
    nb(b, 2, 5, 2, 3, 5, 3); nb(b, 2, 5, 9, 3, 5, 10); nb(b, 2, 5, 4, 2, 5, 8);
    nb(b, 9, 5, 2, 10, 5, 3); nb(b, 9, 5, 9, 10, 5, 10); nb(b, 10, 5, 4, 10, 5, 8);
    uint16_t sE = facing(B[B_NETHER_BRICK_STAIRS], EAST), sW = facing(B[B_NETHER_BRICK_STAIRS], WEST);
    b.set(4, 5, 2, sW); b.set(4, 5, 3, sW); b.set(4, 5, 9, sW); b.set(4, 5, 10, sW);
    b.set(8, 5, 2, sE); b.set(8, 5, 3, sE); b.set(8, 5, 9, sE); b.set(8, 5, 10, sE);
    b.box(3, 4, 4, 4, 4, 8, B[B_SOUL_SAND], B[B_SOUL_SAND]);
    b.box(8, 4, 4, 9, 4, 8, B[B_SOUL_SAND], B[B_SOUL_SAND]);
    b.box(3, 5, 4, 4, 5, 8, B[B_NETHER_WART], B[B_NETHER_WART]);
    b.box(8, 5, 4, 9, 5, 8, B[B_NETHER_WART], B[B_NETHER_WART]);
    castleFloor(b);
}

void corridorBase(Build& b) {
    nb(b, 0, 0, 0, 4, 1, 4);
    air(b, 0, 2, 0, 4, 5, 4);
}

void corridor(Build& b) {
    corridorBase(b);
    nb(b, 0, 2, 0, 0, 5, 4); nb(b, 4, 2, 0, 4, 5, 4);
    uint16_t ns = fence(true, false, true, false);
    fbox(b, 0, 3, 1, 0, 4, 1, ns); fbox(b, 0, 3, 3, 0, 4, 3, ns); fbox(b, 4, 3, 1, 4, 4, 1, ns); fbox(b, 4, 3, 3, 4, 4, 3, ns);
    nb(b, 0, 6, 0, 4, 6, 4);
    pillars(b, 5, 5);
}

void corridorCrossing(Build& b) {
    corridorBase(b);
    nb(b, 0, 2, 0, 0, 5, 0); nb(b, 4, 2, 0, 4, 5, 0); nb(b, 0, 2, 4, 0, 5, 4); nb(b, 4, 2, 4, 4, 5, 4);
    nb(b, 0, 6, 0, 4, 6, 4);
    pillars(b, 5, 5);
}

void corridorTurn(Build& b, bool rightTurn, bool chest) {
    corridorBase(b);
    uint16_t ns = fence(true, false, true, false), we = fence(false, true, false, true);
    if (rightTurn) {
        nb(b, 0, 2, 0, 0, 5, 4); fbox(b, 0, 3, 1, 0, 4, 1, ns); fbox(b, 0, 3, 3, 0, 4, 3, ns);
        nb(b, 4, 2, 0, 4, 5, 0);
        nb(b, 1, 2, 4, 4, 5, 4); fbox(b, 1, 3, 4, 1, 4, 4, we); fbox(b, 3, 3, 4, 3, 4, 4, we);
        if (chest) b.chest(1, 2, 3, "nether_bridge", 0);
    } else {
        nb(b, 4, 2, 0, 4, 5, 4); fbox(b, 4, 3, 1, 4, 4, 1, ns); fbox(b, 4, 3, 3, 4, 4, 3, ns);
        nb(b, 0, 2, 0, 0, 5, 0);
        nb(b, 0, 2, 4, 3, 5, 4); fbox(b, 1, 3, 4, 1, 4, 4, we); fbox(b, 3, 3, 4, 3, 4, 4, we);
        if (chest) b.chest(3, 2, 3, "nether_bridge", 0);
    }
    nb(b, 0, 6, 0, 4, 6, 4);
    pillars(b, 5, 5);
}

void corridorStairs(Build& b) {
    uint16_t sS = facing(B[B_NETHER_BRICK_STAIRS], SOUTH), ns = fence(true, false, true, false);
    for (int i = 0; i <= 9; i++) {
        int j = 7 - i > 1 ? 7 - i : 1;
        int k = j + 5 > 14 - i ? j + 5 : 14 - i;
        if (k > 13) k = 13;
        nb(b, 0, 0, i, 4, j, i);
        air(b, 1, j + 1, i, 3, k - 1, i);
        if (i <= 6) {
            b.set(1, j + 1, i, sS); b.set(2, j + 1, i, sS); b.set(3, j + 1, i, sS);
        }
        nb(b, 0, k, i, 4, k, i);
        nb(b, 0, j + 1, i, 0, k - 1, i);
        nb(b, 4, j + 1, i, 4, k - 1, i);
        if ((i & 1) == 0) {
            fbox(b, 0, j + 2, i, 0, j + 3, i, ns);
            fbox(b, 4, j + 2, i, 4, j + 3, i, ns);
        }
        for (int i1 = 0; i1 <= 4; i1++) b.columnDown(B[B_NETHER_BRICKS], i1, -1, i);
    }
}

void corridorTBalcony(Build& b) {
    nb(b, 0, 0, 0, 8, 1, 8);
    air(b, 0, 2, 0, 8, 5, 8);
    nb(b, 0, 6, 0, 8, 6, 5);
    nb(b, 0, 2, 0, 2, 5, 0); nb(b, 6, 2, 0, 8, 5, 0);
    uint16_t we = fence(false, true, false, true), ns = fence(true, false, true, false);
    fbox(b, 1, 3, 0, 1, 4, 0, we); fbox(b, 7, 3, 0, 7, 4, 0, we);
    nb(b, 0, 2, 4, 8, 2, 8);
    air(b, 1, 1, 4, 2, 2, 4); air(b, 6, 1, 4, 7, 2, 4);
    fbox(b, 1, 3, 8, 7, 3, 8, we);
    b.set(0, 3, 8, fence(false, true, true, false)); b.set(8, 3, 8, fence(false, false, true, true));
    fbox(b, 0, 3, 6, 0, 3, 7, ns); fbox(b, 8, 3, 6, 8, 3, 7, ns);
    nb(b, 0, 3, 4, 0, 5, 5); nb(b, 8, 3, 4, 8, 5, 5);
    nb(b, 1, 3, 5, 2, 5, 5); nb(b, 6, 3, 5, 7, 5, 5);
    fbox(b, 1, 4, 5, 1, 5, 5, we); fbox(b, 7, 4, 5, 7, 5, 5, we);
    for (int i = 0; i <= 5; i++)
        for (int j = 0; j <= 8; j++) b.columnDown(B[B_NETHER_BRICKS], j, -1, i);
}

}  // namespace

void buildFortress(Build& b) {
    switch (b.p.kind) {
        case PK_NF_BRIDGE_STRAIGHT: bridgeStraight(b); break;
        case PK_NF_BRIDGE_END_FILLER: bridgeEndFiller(b, b.p.b); break;
        case PK_NF_BRIDGE_CROSSING: bridgeCrossing(b); break;
        case PK_NF_ROOM_CROSSING: roomCrossing(b); break;
        case PK_NF_STAIRS_ROOM: stairsRoom(b); break;
        case PK_NF_MONSTER_THRONE: monsterThrone(b); break;
        case PK_NF_CASTLE_ENTRANCE: castleEntrance(b); break;
        case PK_NF_STALK_ROOM: stalkRoom(b); break;
        case PK_NF_CORRIDOR: corridor(b); break;
        case PK_NF_CORRIDOR_CROSSING: corridorCrossing(b); break;
        case PK_NF_CORRIDOR_RIGHT_TURN: corridorTurn(b, true, b.p.a); break;
        case PK_NF_CORRIDOR_LEFT_TURN: corridorTurn(b, false, b.p.a); break;
        case PK_NF_CORRIDOR_STAIRS: corridorStairs(b); break;
        case PK_NF_CORRIDOR_T_BALCONY: corridorTBalcony(b); break;
        default: break;
    }
}

}  // namespace sb
}  // namespace vanilla
}  // namespace mc
