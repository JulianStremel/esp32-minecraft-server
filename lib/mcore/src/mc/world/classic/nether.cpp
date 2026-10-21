// The classic generator's Nether fortresses (not vanilla's layout): a bridge crossing over
// the lava sea; forward, bridges to the castle (its entrance, corridors, a crossing, the
// nether wart's stalk room); left, bridges to the blaze spawner's throne; right, bridges
// that end in a broken stub. The pieces are vanilla's fortress pieces (fortress.cpp
// builds them), joined by their doors as vanilla joins them.
#include "mc/world/classic/classic_structures.h"
#include "mc/world/generator.h"
#include "mc/world/noise.h"

namespace mc {
namespace classic {

using namespace vanilla::sb;

namespace {

struct Door { int x, y, z, dir; };

Door forward(const SBox& b, int dir, int ox, int oy) {
    switch (dir) {
        case NORTH: return {b.x0 + ox, b.y0 + oy, b.z0 - 1, NORTH};
        case SOUTH: return {b.x0 + ox, b.y0 + oy, b.z1 + 1, SOUTH};
        case WEST: return {b.x0 - 1, b.y0 + oy, b.z0 + ox, WEST};
        default: return {b.x1 + 1, b.y0 + oy, b.z0 + ox, EAST};
    }
}
Door left(const SBox& b, int dir, int oy, int ox) {
    if (dir == NORTH || dir == SOUTH) return {b.x0 - 1, b.y0 + oy, b.z0 + ox, WEST};
    return {b.x0 + ox, b.y0 + oy, b.z0 - 1, NORTH};
}
Door right(const SBox& b, int dir, int oy, int ox) {
    if (dir == NORTH || dir == SOUTH) return {b.x1 + 1, b.y0 + oy, b.z0 + ox, EAST};
    return {b.x0 + ox, b.y0 + oy, b.z1 + 1, SOUTH};
}

struct Builder {
    Rng& r;
    Piece* out;
    int n = 0, max;
    bool collides(const SBox& b) const {
        for (int i = 0; i < n; i++)
            if (out[i].box.hits(b)) return true;
        return false;
    }
    // a vanilla fortress piece entered by door d (NetherFortressPieces' boxes)
    const Piece* add(int kind, const Door& d) {
        SBox b;
        switch (kind) {
            case PK_NF_BRIDGE_STRAIGHT: b = orientBox(d.x, d.y, d.z, -1, -3, 0, 5, 10, 19, d.dir); break;
            case PK_NF_BRIDGE_END_FILLER: b = orientBox(d.x, d.y, d.z, -1, -3, 0, 5, 10, 8, d.dir); break;
            case PK_NF_BRIDGE_CROSSING: b = orientBox(d.x, d.y, d.z, -8, -3, 0, 19, 10, 19, d.dir); break;
            case PK_NF_MONSTER_THRONE: b = orientBox(d.x, d.y, d.z, -2, 0, 0, 7, 8, 9, d.dir); break;
            case PK_NF_CASTLE_ENTRANCE: case PK_NF_STALK_ROOM: b = orientBox(d.x, d.y, d.z, -5, -3, 0, 13, 14, 13, d.dir); break;
            case PK_NF_CORRIDOR: case PK_NF_CORRIDOR_CROSSING: case PK_NF_CORRIDOR_RIGHT_TURN: case PK_NF_CORRIDOR_LEFT_TURN:
                b = orientBox(d.x, d.y, d.z, -1, 0, 0, 5, 7, 5, d.dir);
                break;
            default: return nullptr;
        }
        if (b.y0 <= 32 || b.y1 >= 120 || collides(b) || n >= max) return nullptr;
        Piece& p = out[n++];
        p = {};
        p.box = b;
        p.kind = (uint8_t)kind;
        p.orient = (uint8_t)d.dir;
        if (kind == PK_NF_CORRIDOR_RIGHT_TURN || kind == PK_NF_CORRIDOR_LEFT_TURN) p.a = r.range(3) == 0 ? 1 : 0;   // a chest
        if (kind == PK_NF_BRIDGE_END_FILLER) p.b = (int32_t)r.u32();   // how it breaks off
        return &p;
    }
    // straight bridges from door d; the door after them (or none: d.dir < 0)
    Door bridges(Door d, int count) {
        for (int i = 0; i < count; i++) {
            const Piece* p = add(PK_NF_BRIDGE_STRAIGHT, d);
            if (!p) return {0, 0, 0, -1};
            d = forward(p->box, d.dir, 1, 3);
        }
        return d;
    }
};

}  // namespace

int netherFortressPieces(uint64_t seed, int cx, int cz, Piece* out, int max) {
    Rng r(hash3(seed, cx, cz, 0xF0F7));
    Builder b = {r, out, 0, max};
    int dir = r.range(4);
    const Piece* crossing = b.add(PK_NF_BRIDGE_CROSSING, Door{cx * 16 + 2, 64, cz * 16 + 2, dir});
    if (!crossing) return 0;
    SBox cb = crossing->box;
    // forward: the castle
    Door d = b.bridges(forward(cb, dir, 8, 3), 1 + r.range(3));
    if (d.dir >= 0) {
        const Piece* gate = b.add(PK_NF_CASTLE_ENTRANCE, d);
        if (gate) {
            Door c = forward(gate->box, d.dir, 5, 3);
            const Piece* p = b.add(PK_NF_CORRIDOR, c);
            if (p) p = b.add(PK_NF_CORRIDOR_CROSSING, forward(p->box, c.dir, 1, 0));
            if (p) {
                SBox xb = p->box;
                int xd = c.dir;
                const Piece* stalk = b.add(PK_NF_STALK_ROOM, forward(xb, xd, 1, 0));
                (void)stalk;
                const Piece* t = b.add(r.range(2) ? PK_NF_CORRIDOR_LEFT_TURN : PK_NF_CORRIDOR, left(xb, xd, 0, 1));
                (void)t;
                b.add(PK_NF_CORRIDOR_RIGHT_TURN, right(xb, xd, 0, 1));
            }
        }
    }
    // left: the blaze spawner's throne
    Door l = b.bridges(left(cb, dir, 3, 8), 1 + r.range(2));
    if (l.dir >= 0) b.add(PK_NF_MONSTER_THRONE, l);
    // right: bridges that break off
    Door rr = b.bridges(right(cb, dir, 3, 8), 1 + r.range(3));
    if (rr.dir >= 0) b.add(PK_NF_BRIDGE_END_FILLER, rr);
    return b.n;
}

}  // namespace classic
}  // namespace mc
