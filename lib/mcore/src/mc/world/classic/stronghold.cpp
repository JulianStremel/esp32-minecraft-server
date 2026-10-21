// The classic generator's strongholds (not vanilla's layout): an entrance in a broken
// arch on the surface, a flight of stairs down to a hall about y 28, from it a corridor
// to the portal room (the End portal's twelve frames), one to a library, one to a prison;
// the rooms are vanilla's stronghold pieces (stronghold.cpp builds them), joined by their
// doors as vanilla joins them (StructurePiece#generateSmallDoorChild...).
#include <string.h>
#include "mc/registry.h"
#include "mc/world/classic/classic_structures.h"
#include "mc/world/generator.h"
#include "mc/world/noise.h"

namespace mc {
namespace classic {

using namespace vanilla::sb;

namespace {

struct Door { int x, y, z, dir; };

// generateSmallDoorChildForward / Left / Right from a piece's box (sx: along the side,
// sy: up from its floor)
Door forward(const SBox& b, int dir, int sx, int sy) {
    switch (dir) {
        case NORTH: return {b.x0 + sx, b.y0 + sy, b.z0 - 1, NORTH};
        case SOUTH: return {b.x0 + sx, b.y0 + sy, b.z1 + 1, SOUTH};
        case WEST: return {b.x0 - 1, b.y0 + sy, b.z0 + sx, WEST};
        default: return {b.x1 + 1, b.y0 + sy, b.z0 + sx, EAST};
    }
}
Door left(const SBox& b, int dir, int sy, int sz) {
    switch (dir) {
        case NORTH: case SOUTH: return {b.x0 - 1, b.y0 + sy, b.z0 + sz, WEST};
        default: return {b.x0 + sz, b.y0 + sy, b.z0 - 1, NORTH};
    }
}
Door right(const SBox& b, int dir, int sy, int sz) {
    switch (dir) {
        case NORTH: case SOUTH: return {b.x1 + 1, b.y0 + sy, b.z0 + sz, EAST};
        default: return {b.x0 + sz, b.y0 + sy, b.z1 + 1, SOUTH};
    }
}

struct Builder {
    Rng& r;
    Piece* out;
    int n = 0, max;

    bool collides(const SBox& b) const {
        for (int i = 0; i < n; i++)
            if (out[i].kind != CP_ENTRANCE && out[i].box.hits(b)) return true;
        return false;
    }
    int door() {
        static const int DOORS[5] = {0, 0, 1, 2, 3};   // an opening (twice), a wooden door, grates, an iron door
        return DOORS[r.range(5)];
    }
    // a vanilla stronghold piece entered by door d (StrongholdPieces' boxes); nullptr if it
    // does not fit
    const Piece* add(int kind, const Door& d) {
        SBox b;
        switch (kind) {
            case PK_SH_STRAIGHT: b = orientBox(d.x, d.y, d.z, -1, -1, 0, 5, 5, 7, d.dir); break;
            case PK_SH_PRISON_HALL: b = orientBox(d.x, d.y, d.z, -1, -1, 0, 9, 5, 11, d.dir); break;
            case PK_SH_ROOM_CROSSING: b = orientBox(d.x, d.y, d.z, -4, -1, 0, 11, 7, 11, d.dir); break;
            case PK_SH_STRAIGHT_STAIRS_DOWN: b = orientBox(d.x, d.y, d.z, -1, -7, 0, 5, 11, 8, d.dir); break;
            case PK_SH_CHEST_CORRIDOR: b = orientBox(d.x, d.y, d.z, -1, -1, 0, 5, 5, 7, d.dir); break;
            case PK_SH_LIBRARY: b = orientBox(d.x, d.y, d.z, -4, -1, 0, 14, 11, 15, d.dir); break;
            case PK_SH_PORTAL_ROOM: b = orientBox(d.x, d.y, d.z, -4, -1, 0, 11, 8, 16, d.dir); break;
            default: return nullptr;
        }
        if (b.y0 <= -54 || collides(b) || n >= max) return nullptr;
        Piece& p = out[n++];
        p = {};
        p.box = b;
        p.kind = (uint8_t)kind;
        p.orient = (uint8_t)d.dir;
        int a = kind == PK_SH_PORTAL_ROOM ? 0 : door();
        if (kind == PK_SH_ROOM_CROSSING) a |= r.range(5) << 8;   // a fountain, a storeroom, ...
        if (kind == PK_SH_LIBRARY) a |= 8;                       // the tall one with its gallery
        p.a = (int16_t)a;
        return &p;
    }
    // straight corridors, then a room; false if the room does not fit
    bool corridorTo(Door d, int count, int room) {
        for (int i = 0; i < count; i++) {
            const Piece* p = add(r.range(4) == 0 ? PK_SH_CHEST_CORRIDOR : PK_SH_STRAIGHT, d);
            if (!p) return false;
            d = forward(p->box, d.dir, 1, 1);
        }
        return add(room, d) != nullptr;
    }
};

}  // namespace

int strongholdPieces(const Generator& g, uint64_t seed, int cx, int cz, Piece* out, int max) {
    Rng r(hash3(seed, cx, cz, 0x57A0));
    int x = cx * 16 + 2, z = cz * 16 + 2;
    ColumnInfo col = g.column(x, z);
    if (col.height < SEA_LEVEL || col.river) return 0;   // the entrance stands on dry land
    Builder b = {r, out, 0, max};
    // the entrance: a broken arch around the top of the stairs
    Piece* e = &out[b.n++];
    *e = {};
    e->kind = CP_ENTRANCE;
    int dir = r.range(4);
    // the first flight starts so that its ceiling is just under the ground
    Door d = {x, col.height - 4, z, dir};
    const Piece* last = nullptr;
    for (int i = 0; i < 14 && (d.y > 34 || i < 2); i++) {
        const Piece* p = b.add(PK_SH_STRAIGHT_STAIRS_DOWN, d);
        if (!p) return 0;
        if (!last) {   // the shaft over the first flight's top three steps, up to the ground
            int x0 = d.x, x1 = d.x + 2, z0 = d.z, z1 = d.z + 2;
            if (dir == NORTH) { z0 = d.z - 3; z1 = d.z - 1; }
            else if (dir == SOUTH) { z0 = d.z + 1; z1 = d.z + 3; }
            else if (dir == WEST) { x0 = d.x - 3; x1 = d.x - 1; }
            else { x0 = d.x + 1; x1 = d.x + 3; }
            e->box = {x0, d.y, z0, x1, col.height, z1};
            e->orient = (uint8_t)dir;
        }
        last = p;
        d = forward(p->box, dir, 1, 1);
    }
    // the hall
    const Piece* hall = b.add(PK_SH_ROOM_CROSSING, d);
    if (!hall) return 0;
    SBox hb = hall->box;
    // forward to the portal room; left a library, right the prison (each where it fits)
    if (!b.corridorTo(forward(hb, dir, 4, 1), 2 + r.range(2), PK_SH_PORTAL_ROOM)) return 0;
    b.corridorTo(left(hb, dir, 1, 4), 1 + r.range(2), PK_SH_LIBRARY);
    b.corridorTo(right(hb, dir, 1, 4), 1 + r.range(2), PK_SH_PRISON_HALL);
    return b.n;
}

}  // namespace classic
}  // namespace mc
