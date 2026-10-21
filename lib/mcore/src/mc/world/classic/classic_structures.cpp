// The classic generator's structures over chunks (classic_structures.h): placement on a
// grid of regions, the starts' cache, their pieces into each chunk.
#include "mc/world/classic/classic_structures.h"
#include <stdlib.h>
#include <string.h>
#include "mc/platform.h"
#include "mc/registry.h"
#include "mc/world/generator.h"
#include "mc/world/noise.h"
#include "mc/world/vanilla/templates.h"

namespace mc {
namespace classic {

using vanilla::sb::Piece;
using vanilla::sb::SBox;

namespace {
constexpr int CACHE = 256;
struct Placement { const char* name; uint8_t dim; int spacing, separation; uint32_t salt; int reach; };
// reach: how many chunks from its start chunk a structure's pieces can lie
const Placement PLACEMENTS[T_NUM] = {
    {"village", DIM_OVERWORLD, 32, 8, 0x5111A6Eu, 6},
    {"stronghold", DIM_OVERWORLD, 80, 30, 0x57A0B0Du, 9},   // about one every 1300 blocks
};
constexpr int MAX_PIECES = 400;

int floorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
bool solid(uint16_t st) { return !stateIsAir(st) && !stateIsFluid(st); }
}  // namespace

Structures::~Structures() {
    if (cache_)
        for (int i = 0; i < CACHE; i++) plat::bigFree(cache_[i].start);
    plat::bigFree(cache_);
    if (lock_) plat::mutexDestroy(lock_);
}

bool Structures::init(uint64_t seed, uint8_t dim, const Generator* gen) {
    seed_ = seed;
    dim_ = dim;
    gen_ = gen;
    if (!lock_) lock_ = plat::mutexCreate();
    if (!cache_) {
        cache_ = (Entry*)plat::bigAlloc(sizeof(Entry) * CACHE);
        if (!cache_) return false;
        for (int i = 0; i < CACHE; i++) cache_[i] = {0, 0, -1, nullptr};
    }
    return lock_ != nullptr;
}

bool Structures::candidate(int type, int rx, int rz, int& cx, int& cz) const {
    const Placement& p = PLACEMENTS[type];
    Rng r(hash3(seed_ ^ p.salt, rx, rz, 0xC1A5));
    int lim = p.spacing - p.separation;
    cx = rx * p.spacing + r.range(lim);
    cz = rz * p.spacing + r.range(lim);
    return true;
}

Structures::Start* Structures::evaluate(int type, int cx, int cz) const {
    Piece* tmp = (Piece*)plat::bigAlloc(sizeof(Piece) * MAX_PIECES);
    if (!tmp) return nullptr;
    int n = 0;
    if (type == T_VILLAGE) n = villagePieces(*gen_, seed_, cx, cz, tmp, MAX_PIECES);
    else if (type == T_STRONGHOLD) n = strongholdPieces(*gen_, seed_, cx, cz, tmp, MAX_PIECES);
    Start* s = nullptr;
    if (n > 0) {
        size_t size = sizeof(Start) + sizeof(Piece) * (n - 1);
        s = (Start*)plat::bigAlloc(size);
        if (s) {
            memset(s, 0, size);
            s->type = (uint8_t)type;
            s->cx = cx;
            s->cz = cz;
            s->n = (uint16_t)n;
            memcpy(s->pieces, tmp, sizeof(Piece) * n);
            s->box = tmp[0].box;
            for (int i = 1; i < n; i++) {
                const SBox& b = tmp[i].box;
                if (b.x0 < s->box.x0) s->box.x0 = b.x0;
                if (b.y0 < s->box.y0) s->box.y0 = b.y0;
                if (b.z0 < s->box.z0) s->box.z0 = b.z0;
                if (b.x1 > s->box.x1) s->box.x1 = b.x1;
                if (b.y1 > s->box.y1) s->box.y1 = b.y1;
                if (b.z1 > s->box.z1) s->box.z1 = b.z1;
            }
        }
    }
    plat::bigFree(tmp);
    return s;
}

const Structures::Start* Structures::start(int type, int cx, int cz) {
    uint32_t h = ((uint32_t)cx * 73856093u ^ (uint32_t)cz * 19349663u ^ (uint32_t)type * 83492791u) % CACHE;
    {
        LockGuard g(lock_);
        Entry& e = cache_[h];
        if (e.type == type && e.cx == cx && e.cz == cz) return e.start;
    }
    Start* s = evaluate(type, cx, cz);   // outside the lock: it samples the terrain
    LockGuard g(lock_);
    Entry& e = cache_[h];
    if (e.type == type && e.cx == cx && e.cz == cz) {   // another worker was faster: keep its copy
        plat::bigFree(s);
        return e.start;
    }
    plat::bigFree(e.start);
    e = {cx, cz, (int16_t)type, s};
    return s;
}

// the pieces of a classic structure, clipped to the chunk
static void placePiece(Chunk& c, const Piece& p, const SBox& clip, uint64_t seed, const ColumnInfo* cols, int k) {
    auto top = [&](int X, int Z) { return cols[(Z & 15) * 16 + (X & 15)].height; };
    switch (p.kind) {
        case vanilla::sb::PK_TEMPLATE: {
            // a building: its box cleared above its floor, a foundation under it down to the ground
            uint16_t found = (p.b >> 30 & 1) ? bs::Sandstone : bs::Cobblestone;
            for (int X = p.box.x0 > clip.x0 ? p.box.x0 : clip.x0; X <= p.box.x1 && X <= clip.x1; X++)
                for (int Z = p.box.z0 > clip.z0 ? p.box.z0 : clip.z0; Z <= p.box.z1 && Z <= clip.z1; Z++) {
                    for (int y = p.box.y0 + 1; y <= p.box.y1 + 2 && c.hasY(y); y++) c.set(X & 15, y, Z & 15, bs::Air);
                    for (int y = p.box.y0 - 1; y > c.minY() && y >= p.box.y0 - 12; y--) {
                        if (solid(c.get(X & 15, y, Z & 15))) break;
                        c.set(X & 15, y, Z & 15, found);
                    }
                }
            Piece q = p;
            q.b &= ~(1 << 30);
            vanilla::sb::buildTemplatePiece(c, clip, q, (int64_t)hash3(seed, p.box.x0, p.box.z0, k));
            break;
        }
        case CP_ROAD: {
            uint16_t path = p.b == 1 ? bs::SmoothSandstone : p.b == 2 ? bs::Gravel : bs::DirtPath;
            for (int X = p.box.x0 > clip.x0 ? p.box.x0 : clip.x0; X <= p.box.x1 && X <= clip.x1; X++)
                for (int Z = p.box.z0 > clip.z0 ? p.box.z0 : clip.z0; Z <= p.box.z1 && Z <= clip.z1; Z++) {
                    int y = top(X, Z);
                    if (y < SEA_LEVEL - 1) {   // over water: a plank bridge at the sea's surface
                        c.set(X & 15, SEA_LEVEL - 1, Z & 15, bs::OakPlanks);
                        continue;
                    }
                    uint16_t at = c.get(X & 15, y, Z & 15);
                    if (blockIdOf(at) == blockIdOf(bs::Water)) {
                        c.set(X & 15, y, Z & 15, bs::OakPlanks);
                        continue;
                    }
                    c.set(X & 15, y, Z & 15, path);
                    for (int dy = 1; dy <= 3 && c.hasY(y + dy); dy++) {   // plants and leaves over the road
                        uint16_t above = c.get(X & 15, y + dy, Z & 15);
                        if (!stateIsAir(above) && blockIdOf(above) != blockIdOf(bs::OakPlanks)) c.set(X & 15, y + dy, Z & 15, bs::Air);
                    }
                }
            break;
        }
        case CP_LAMP: {
            int X = p.box.x0, Z = p.box.z0;
            if (!clip.inside(X, clip.y0 + 1, Z)) break;
            int y = top(X, Z) + 1;
            if (!c.hasY(y + 3) || y <= SEA_LEVEL - 1) break;
            c.set(X & 15, y, Z & 15, bs::OakFence);
            c.set(X & 15, y + 1, Z & 15, bs::OakFence);
            c.set(X & 15, y + 2, Z & 15, bs::Lantern);
            break;
        }
        case CP_BELL: {
            int X = p.box.x0, Z = p.box.z0, y = p.box.y0;
            if (!clip.inside(X, y, Z)) break;
            c.set(X & 15, y, Z & 15, setPropStr(bs::Bell, "attachment", "floor"));
            break;
        }
        case CP_VILLAGERS: {
            int X0 = p.box.x0, Z0 = p.box.z0, y = p.box.y0;
            int type = findEntityType("villager");
            if (type < 0) break;
            Rng r(hash3(seed, X0, Z0, 0x7111));
            for (int i = 0; i < p.a; i++) {
                int X = X0 + r.between(-4, 4), Z = Z0 + r.between(-4, 4);
                if (!clip.inside(X, y, Z)) continue;
                SavedEntity e;
                e.kind = 3;
                e.type = (uint16_t)type;
                e.x = X + 0.5;
                e.y = top(X, Z) + 1;
                e.z = Z + 0.5;
                e.health = 20;
                c.addEntity(e);
            }
            break;
        }
        case CP_ENTRANCE: {
            static const char* const FACE[4] = {"north", "east", "south", "west"};
            int dir = p.orient & 3, back = (dir + 2) & 3;
            const int DX[4] = {0, 1, 0, -1}, DZ[4] = {-1, 0, 1, 0};
            Rng r(hash3(seed, p.box.x0, p.box.z0, 0xE47));
            for (int X = p.box.x0 - 1; X <= p.box.x1 + 1; X++)
                for (int Z = p.box.z0 - 1; Z <= p.box.z1 + 1; Z++) {
                    bool inner = X >= p.box.x0 && X <= p.box.x1 && Z >= p.box.z0 && Z <= p.box.z1;
                    uint16_t brick = r.range(3) ? bs::StoneBricks : r.range(2) ? bs::MossyStoneBricks : bs::CrackedStoneBricks;
                    if (!clip.inside(X, p.box.y1, Z)) continue;
                    if (inner) {   // the shaft
                        for (int y = p.box.y0 + 3; y <= p.box.y1 + 3 && c.hasY(y); y++) c.set(X & 15, y, Z & 15, bs::Air);
                        // a ladder down the back row's middle, against the side the stairs come from
                        bool backRow = (dir == vanilla::sb::NORTH && Z == p.box.z1) || (dir == vanilla::sb::SOUTH && Z == p.box.z0) ||
                                       (dir == vanilla::sb::WEST && X == p.box.x1) || (dir == vanilla::sb::EAST && X == p.box.x0);
                        bool middle = (dir == vanilla::sb::NORTH || dir == vanilla::sb::SOUTH) ? X == p.box.x0 + 1 : Z == p.box.z0 + 1;
                        if (backRow && middle)
                            for (int y = p.box.y0 + 3; y <= p.box.y1; y++)
                                c.set(X & 15, y, Z & 15, setPropStr(bs::Ladder, "facing", FACE[dir]));
                    } else {   // the rim at the ground, the walls of the shaft under it
                        for (int y = p.box.y0 + 3; y <= p.box.y1; y++)
                            if (!solid(c.get(X & 15, y, Z & 15)) || y == p.box.y1) c.set(X & 15, y, Z & 15, brick);
                        // the arch: two pillars at the far corners, a lintel between them
                        bool farRow = (X - (p.box.x0 + p.box.x1) / 2) * DX[dir] + (Z - (p.box.z0 + p.box.z1) / 2) * DZ[dir] == 2;
                        bool corner = (X == p.box.x0 - 1 || X == p.box.x1 + 1) && (Z == p.box.z0 - 1 || Z == p.box.z1 + 1);
                        if (farRow && corner)
                            for (int y = p.box.y1 + 1; y <= p.box.y1 + 3 && c.hasY(y); y++) c.set(X & 15, y, Z & 15, brick);
                        if (farRow && !corner && r.range(3) && c.hasY(p.box.y1 + 3)) c.set(X & 15, p.box.y1 + 3, Z & 15, brick);
                    }
                }
            (void)back;
            break;
        }
        case vanilla::sb::PK_SH_STRAIGHT: case vanilla::sb::PK_SH_PRISON_HALL: case vanilla::sb::PK_SH_ROOM_CROSSING:
        case vanilla::sb::PK_SH_STRAIGHT_STAIRS_DOWN: case vanilla::sb::PK_SH_CHEST_CORRIDOR: case vanilla::sb::PK_SH_LIBRARY:
        case vanilla::sb::PK_SH_PORTAL_ROOM: {
            vanilla::sb::Build b(c, p, (int64_t)hash3(seed, p.box.x0, p.box.z0, k));
            vanilla::sb::buildStronghold(b);
            break;
        }
        default: break;
    }
}

void Structures::place(Chunk& c, const ColumnInfo* cols) {
    const SBox clip = {c.cx * 16, c.minY(), c.cz * 16, c.cx * 16 + 15, c.maxY(), c.cz * 16 + 15};
    for (int type = 0; type < T_NUM; type++) {
        const Placement& pl = PLACEMENTS[type];
        if (pl.dim != dim_) continue;
        for (int rz = floorDiv(c.cz - pl.reach, pl.spacing); rz <= floorDiv(c.cz + pl.reach, pl.spacing); rz++)
            for (int rx = floorDiv(c.cx - pl.reach, pl.spacing); rx <= floorDiv(c.cx + pl.reach, pl.spacing); rx++) {
                int scx, scz;
                candidate(type, rx, rz, scx, scz);
                if (abs(scx - c.cx) > pl.reach || abs(scz - c.cz) > pl.reach) continue;
                const Start* s = start(type, scx, scz);
                if (!s || !s->box.hits(clip)) continue;
                for (int k = 0; k < s->n; k++)
                    if (s->pieces[k].box.hits(clip)) placePiece(c, s->pieces[k], clip, seed_, cols, k);
            }
    }
}

bool Structures::locate(const char* name, int x, int z, int radius, int& cx, int& cz) {
    for (int type = 0; type < T_NUM; type++) {
        const Placement& pl = PLACEMENTS[type];
        if (strcmp(pl.name, name) || pl.dim != dim_) continue;
        int bx = x >> 4, bz = z >> 4, rr = radius / 16 / pl.spacing + 1;
        long best = -1;
        // ring by ring of regions; the nearest found in a ring (and the next, which may hold a nearer one)
        for (int ring = 0; ring <= rr; ring++) {
            for (int rz = floorDiv(bz, pl.spacing) - ring; rz <= floorDiv(bz, pl.spacing) + ring; rz++)
                for (int rx = floorDiv(bx, pl.spacing) - ring; rx <= floorDiv(bx, pl.spacing) + ring; rx++) {
                    if (abs(rz - floorDiv(bz, pl.spacing)) != ring && abs(rx - floorDiv(bx, pl.spacing)) != ring) continue;
                    int scx, scz;
                    candidate(type, rx, rz, scx, scz);
                    long d = (long)(scx - bx) * (scx - bx) + (long)(scz - bz) * (scz - bz);
                    if (best >= 0 && d >= best) continue;
                    if (!start(type, scx, scz)) continue;
                    best = d;
                    cx = scx;
                    cz = scz;
                }
            if (best >= 0 && ring > 0) return true;
        }
        return best >= 0;
    }
    return false;
}

}  // namespace classic
}  // namespace mc
