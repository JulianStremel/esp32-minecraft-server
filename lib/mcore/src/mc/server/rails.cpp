// Rails (BaseRailBlock, RailState, PoweredRailBlock, DetectorRailBlock).
//
// - A rail's shape follows its neighbours when it is placed (RailState#place: it joins
//   rails that can still take a connection, makes curves and slopes up a block) and the
//   neighbours it joins turn towards it (RailState#connectTo). A junction of three rails
//   switches its curve with redstone.
// - Powered and activator rails are powered by redstone next to them or by a powered rail
//   of the same kind up to 8 rails away along the line (findPoweredRailSignal).
// - Detector rails give a signal of 15 while a minecart is on them (minecarts.cpp).
// - Rails need a sturdy block below; a slope also needs the block it rises into.
#include <string.h>
#include "mc/registry.h"
#include "mc/server/rails.h"
#include "mc/server/server.h"

namespace mc {
namespace rails {

// exits of each shape: (dx, dy, dz) of the two ends (AbstractMinecart EXITS)
static const int8_t EXITS[10][2][3] = {
    {{0, 0, -1}, {0, 0, 1}},    // north_south
    {{-1, 0, 0}, {1, 0, 0}},    // east_west
    {{-1, -1, 0}, {1, 0, 0}},   // ascending_east
    {{-1, 0, 0}, {1, -1, 0}},   // ascending_west
    {{0, 0, -1}, {0, -1, 1}},   // ascending_north
    {{0, -1, -1}, {0, 0, 1}},   // ascending_south
    {{0, 0, 1}, {1, 0, 0}},     // south_east
    {{0, 0, 1}, {-1, 0, 0}},    // south_west
    {{0, 0, -1}, {-1, 0, 0}},   // north_west
    {{0, 0, -1}, {1, 0, 0}},    // north_east
};

bool isRail(uint16_t st) {
    uint16_t id = blockIdOf(st);
    return id == blk::Rail || id == blk::PoweredRail || id == blk::DetectorRail || id == blk::ActivatorRail;
}
bool straightOnly(uint16_t st) { return isRail(st) && blockIdOf(st) != blk::Rail; }
int shapeOf(uint16_t st) { return isRail(st) ? getProp(st, "shape") : -1; }
uint16_t withShape(uint16_t st, int shape) { return setProp(st, "shape", shape); }
bool ascending(int shape) { return shape >= ASC_EAST && shape <= ASC_SOUTH; }
void exits(int shape, int a[3], int b[3]) {
    for (int k = 0; k < 3; k++) {
        a[k] = EXITS[shape][0][k];
        b[k] = EXITS[shape][1][k];
    }
}

namespace {
struct Pos { int x, y, z; };

struct RailState {
    Server& s;
    Pos pos;
    uint16_t st;
    bool straight;
    Pos conn[2];
    int n = 0;

    RailState(Server& srv, Pos p, uint16_t state) : s(srv), pos(p), st(state), straight(straightOnly(state)) {
        updateConnections(shapeOf(state));
    }
    void updateConnections(int shape) {
        n = 0;
        if (shape < 0) return;
        for (int e = 0; e < 2; e++) {
            const int8_t* d = EXITS[shape][e];
            // a slope's upper end is one block up (the exits point down to the lower end)
            int up = 0;
            if (ascending(shape)) {
                int other = EXITS[shape][e ^ 1][1];
                up = d[1] == 0 && other < 0 ? 1 : 0;
            }
            conn[n++] = {pos.x + d[0], pos.y + up, pos.z + d[2]};
        }
    }
    static bool getRail(Server& s, Pos p, Pos& out, uint16_t& st) {
        const int dy[3] = {0, 1, -1};
        for (int k = 0; k < 3; k++) {
            uint16_t b = s.blockAt(p.x, p.y + dy[k], p.z);
            if (isRail(b)) {
                out = {p.x, p.y + dy[k], p.z};
                st = b;
                return true;
            }
        }
        return false;
    }
    bool connectsTo(const RailState& o) const {
        for (int i = 0; i < n; i++)
            if (conn[i].x == o.pos.x && conn[i].z == o.pos.z) return true;
        return false;
    }
    bool hasConnection(Pos p) const {
        for (int i = 0; i < n; i++)
            if (conn[i].x == p.x && conn[i].z == p.z && (conn[i].y == p.y || conn[i].y == p.y + 1 || conn[i].y == p.y - 1))
                return true;
        return false;
    }
    void removeSoftConnections() {
        for (int i = 0; i < n;) {
            Pos rp;
            uint16_t rst;
            bool keep = false;
            if (getRail(s, conn[i], rp, rst)) {
                RailState r(s, rp, rst);
                keep = r.connectsTo(*this);
            }
            if (keep) i++;
            else {
                conn[i] = conn[n - 1];
                n--;
            }
        }
    }
    bool canConnectTo(const RailState& o) const { return connectsTo(o) || n != 2; }
    bool hasNeighborRail(Pos p) {
        Pos rp;
        uint16_t rst;
        if (!getRail(s, p, rp, rst)) return false;
        RailState r(s, rp, rst);
        r.removeSoftConnections();
        return r.canConnectTo(*this);
    }
    bool railAt(int x, int y, int z) { return isRail(s.blockAt(x, y, z)); }

    int countPotentialConnections() {
        int c = 0;
        Pos around[4] = {{pos.x, pos.y, pos.z - 1}, {pos.x, pos.y, pos.z + 1}, {pos.x - 1, pos.y, pos.z}, {pos.x + 1, pos.y, pos.z}};
        for (const Pos& p : around) c += hasNeighborRail(p);
        return c;
    }

    int slopeOf(int shape) {
        if (shape == NORTH_SOUTH) {
            if (railAt(pos.x, pos.y + 1, pos.z - 1)) shape = ASC_NORTH;
            if (railAt(pos.x, pos.y + 1, pos.z + 1)) shape = ASC_SOUTH;
        }
        if (shape == EAST_WEST) {
            if (railAt(pos.x + 1, pos.y + 1, pos.z)) shape = ASC_EAST;
            if (railAt(pos.x - 1, pos.y + 1, pos.z)) shape = ASC_WEST;
        }
        return shape;
    }

    void connectTo(RailState& o) {
        if (n < 2) conn[n++] = o.pos;
        bool fn = hasConnection({pos.x, pos.y, pos.z - 1}), fs = hasConnection({pos.x, pos.y, pos.z + 1});
        bool fw = hasConnection({pos.x - 1, pos.y, pos.z}), fe = hasConnection({pos.x + 1, pos.y, pos.z});
        int shape = -1;
        if (fn || fs) shape = NORTH_SOUTH;
        if (fw || fe) shape = EAST_WEST;
        if (!straight) {
            if (fs && fe && !fn && !fw) shape = SOUTH_EAST;
            if (fs && fw && !fn && !fe) shape = SOUTH_WEST;
            if (fn && fw && !fs && !fe) shape = NORTH_WEST;
            if (fn && fe && !fs && !fw) shape = NORTH_EAST;
        }
        if (shape < 0) shape = NORTH_SOUTH;
        shape = slopeOf(shape);
        st = withShape(st, shape);
        updateConnections(shape);
        s.setBlock(pos.x, pos.y, pos.z, st);
    }

    void place(bool powered, bool always, int defaultShape) {
        int shape = chooseShape(powered, defaultShape);
        updateConnections(shape);
        uint16_t next = withShape(st, shape);
        if (!always && s.blockAt(pos.x, pos.y, pos.z) == next) return;
        st = next;
        s.setBlock(pos.x, pos.y, pos.z, st);
        for (int i = 0; i < n; i++) {
            Pos rp;
            uint16_t rst;
            if (!getRail(s, conn[i], rp, rst)) continue;
            RailState r(s, rp, rst);
            r.removeSoftConnections();
            if (r.canConnectTo(*this)) r.connectTo(*this);
        }
    }

    // the shape RailState#place picks (reads the neighbours, changes nothing)
    int chooseShape(bool powered, int defaultShape) {
        Pos pn = {pos.x, pos.y, pos.z - 1}, ps = {pos.x, pos.y, pos.z + 1};
        Pos pw = {pos.x - 1, pos.y, pos.z}, pe = {pos.x + 1, pos.y, pos.z};
        bool fn = hasNeighborRail(pn), fs = hasNeighborRail(ps), fw = hasNeighborRail(pw), fe = hasNeighborRail(pe);
        int shape = -1;
        bool ns = fn || fs, ew = fw || fe;
        if (ns && !ew) shape = NORTH_SOUTH;
        if (ew && !ns) shape = EAST_WEST;
        bool se = fs && fe, sw = fs && fw, ne = fn && fe, nw = fn && fw;
        if (!straight) {
            if (se && !fn && !fw) shape = SOUTH_EAST;
            if (sw && !fn && !fe) shape = SOUTH_WEST;
            if (nw && !fs && !fe) shape = NORTH_WEST;
            if (ne && !fs && !fw) shape = NORTH_EAST;
        }
        if (shape < 0) {
            if (ns && ew) shape = defaultShape;
            else if (ns) shape = NORTH_SOUTH;
            else if (ew) shape = EAST_WEST;
            if (!straight) {
                if (powered) {
                    if (se) shape = SOUTH_EAST;
                    if (sw) shape = SOUTH_WEST;
                    if (ne) shape = NORTH_EAST;
                    if (nw) shape = NORTH_WEST;
                } else {
                    if (nw) shape = NORTH_WEST;
                    if (ne) shape = NORTH_EAST;
                    if (sw) shape = SOUTH_WEST;
                    if (se) shape = SOUTH_EAST;
                }
            }
        }
        if (shape == NORTH_SOUTH || shape == EAST_WEST) shape = slopeOf(shape);
        if (shape < 0) shape = defaultShape;
        return shape;
    }
};
}  // namespace

// BaseRailBlock#getStateForPlacement and #updateDir(alwaysPlace): the shape along the
// player's view, then joined to the neighbours
uint16_t placementShape(uint16_t st, bool eastWest) { return withShape(st, eastWest ? EAST_WEST : NORTH_SOUTH); }

void placed(Server& s, int x, int y, int z) {
    uint16_t st = s.blockAt(x, y, z);
    if (!isRail(st)) return;
    RailState r(s, {x, y, z}, st);
    r.place(s.redstone.bestSignal(s, x, y, z) > 0, true, shapeOf(st));
    updatePower(s, x, y, z);
}

bool supported(Server& s, uint16_t st, int x, int y, int z) {
    if (!stateFaceSturdy(s.blockAt(x, y - 1, z), 1)) return false;
    switch (shapeOf(st)) {   // a slope leans on the block it rises into (BaseRailBlock#shouldBeRemoved)
        case ASC_EAST: return stateFaceSturdy(s.blockAt(x + 1, y, z), 1);
        case ASC_WEST: return stateFaceSturdy(s.blockAt(x - 1, y, z), 1);
        case ASC_NORTH: return stateFaceSturdy(s.blockAt(x, y, z - 1), 1);
        case ASC_SOUTH: return stateFaceSturdy(s.blockAt(x, y, z + 1), 1);
        default: return true;
    }
}

// ---------------------------------------------------------------- power along the line
static bool findPowered(Server& s, int x, int y, int z, uint16_t st, bool forward, int depth);

static bool sameRailWithPower(Server& s, int x, int y, int z, uint16_t kind, bool forward, int depth, int shape) {
    uint16_t st = s.blockAt(x, y, z);
    if (blockIdOf(st) != kind) return false;
    int other = shapeOf(st);
    if (shape == EAST_WEST && (other == NORTH_SOUTH || other == ASC_NORTH || other == ASC_SOUTH)) return false;
    if (shape == NORTH_SOUTH && (other == EAST_WEST || other == ASC_EAST || other == ASC_WEST)) return false;
    if (!getBool(st, "powered")) return false;
    return s.redstone.bestSignal(s, x, y, z) > 0 || findPowered(s, x, y, z, st, forward, depth + 1);
}

static bool findPowered(Server& s, int x, int y, int z, uint16_t st, bool forward, int depth) {
    if (depth >= 8) return false;
    bool below = true;
    int shape = shapeOf(st);
    switch (shape) {
        case NORTH_SOUTH: forward ? ++z : --z; break;
        case EAST_WEST: forward ? --x : ++x; break;
        case ASC_EAST:
            if (forward) --x;
            else { ++x; ++y; below = false; }
            shape = EAST_WEST;
            break;
        case ASC_WEST:
            if (forward) { --x; ++y; below = false; }
            else ++x;
            shape = EAST_WEST;
            break;
        case ASC_NORTH:
            if (forward) ++z;
            else { --z; ++y; below = false; }
            shape = NORTH_SOUTH;
            break;
        case ASC_SOUTH:
            if (forward) { ++z; ++y; below = false; }
            else --z;
            shape = NORTH_SOUTH;
            break;
        default: return false;
    }
    uint16_t kind = blockIdOf(st);
    if (sameRailWithPower(s, x, y, z, kind, forward, depth, shape)) return true;
    return below && sameRailWithPower(s, x, y - 1, z, kind, forward, depth, shape);
}

void updatePower(Server& s, int x, int y, int z) {
    uint16_t st = s.blockAt(x, y, z), id = blockIdOf(st);
    if (id != blk::PoweredRail && id != blk::ActivatorRail) return;
    bool powered = s.redstone.bestSignal(s, x, y, z) > 0 || findPowered(s, x, y, z, st, true, 0) ||
                   findPowered(s, x, y, z, st, false, 0);
    if (powered == getBool(st, "powered")) return;
    s.setBlock(x, y, z, setBool(st, "powered", powered));
    s.redstone.neighbours(s, x, y - 1, z);
    if (ascending(shapeOf(st))) s.redstone.neighbours(s, x, y + 1, z);
}

// A neighbour changed: the power of powered and activator rails; a plain rail at a
// junction of three switches its curve with redstone (RailBlock#updateState).
void neighbourChanged(Server& s, int x, int y, int z, uint16_t st) {
    uint16_t id = blockIdOf(st);
    if (id == blk::PoweredRail || id == blk::ActivatorRail) {
        updatePower(s, x, y, z);
    } else if (id == blk::Rail) {
        // vanilla does this only when the neighbour that changed is a redstone source; here:
        // only from the junction's shape for the other power state to its shape for this one
        RailState r(s, {x, y, z}, st);
        if (r.countPotentialConnections() != 3) return;
        bool powered = s.redstone.bestSignal(s, x, y, z) > 0;
        int cur = shapeOf(st);
        int want = RailState(s, {x, y, z}, st).chooseShape(powered, cur);
        int other = RailState(s, {x, y, z}, st).chooseShape(!powered, cur);
        if (cur == other && cur != want) RailState(s, {x, y, z}, st).place(powered, false, cur);
    }
}

}  // namespace rails
}  // namespace mc
