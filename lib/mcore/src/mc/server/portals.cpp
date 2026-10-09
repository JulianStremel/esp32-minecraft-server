// Nether portals: lighting an obsidian frame, breaking the portal with its frame, and
// linking: a portal leads to the nearest known portal around the scaled position in
// the other dimension, or builds one there (vanilla's PortalShape and PortalForcer, in
// a simpler form). Known portals are kept in WorldState (saved with the world).
#include <math.h>
#include <string.h>
#include "mc/registry.h"
#include "mc/server/server.h"

namespace mc {

static constexpr int PORTAL_MIN_W = 2, PORTAL_MAX_W = 21, PORTAL_MIN_H = 3, PORTAL_MAX_H = 21;
// vanilla's search for an existing portal: 128 blocks in the overworld, 16 in the Nether
static int searchRadius(uint8_t dim) { return dim == DIM_NETHER ? 16 : 128; }
static constexpr int BUILD_SEARCH = 16;   // where a new portal may be built, around the target

static bool isPortal(uint16_t st) { return blockIdOf(st) == blk::NetherPortal; }
static bool isFrame(uint16_t st) { return blockIdOf(st) == blk::Obsidian; }
// what a frame may be lit around (vanilla: air, fire, or portal)
static bool isOpen(uint16_t st) { return stateIsAir(st) || blockIdOf(st) == blk::Fire || isPortal(st); }

uint16_t portalState(uint8_t axis) { return setPropStr(BLOCKS[blk::NetherPortal].defState, "axis", axis ? "z" : "x"); }

// ------------------------------------------------------------------ frames
// A complete frame around (x, y, z) on `axis`: the inside's lower corner, width and
// height. false if there is none.
static bool findFrame(Server& s, int x, int y, int z, uint8_t axis, int& cx, int& cy, int& cz, int& w, int& h) {
    int ax = axis ? 0 : 1, az = axis ? 1 : 0;
    if (!isOpen(s.blockAt(x, y, z))) return false;
    // down to the bottom of the frame
    int by = y;
    for (int k = 0; k < PORTAL_MAX_H && by > dimMinY(s.curDim) + 1 && isOpen(s.blockAt(x, by - 1, z)); k++) by--;
    if (!isFrame(s.blockAt(x, by - 1, z))) return false;
    // to the negative end along the axis
    int lx = x, lz = z;
    for (int k = 0; k < PORTAL_MAX_W; k++) {
        if (!isOpen(s.blockAt(lx - ax, by, lz - az)) || !isFrame(s.blockAt(lx - ax, by - 1, lz - az))) break;
        lx -= ax;
        lz -= az;
    }
    if (!isFrame(s.blockAt(lx - ax, by, lz - az))) return false;
    // width: open over obsidian, up to the other side
    int width = 0;
    while (width <= PORTAL_MAX_W && isOpen(s.blockAt(lx + ax * width, by, lz + az * width)) &&
           isFrame(s.blockAt(lx + ax * width, by - 1, lz + az * width)))
        width++;
    if (width < PORTAL_MIN_W || width > PORTAL_MAX_W || !isFrame(s.blockAt(lx + ax * width, by, lz + az * width))) return false;
    // height: rows with both sides of obsidian and an open inside, closed by an obsidian row
    int height = 0;
    for (; height <= PORTAL_MAX_H; height++) {
        int yy = by + height;
        bool open = true;
        for (int i = 0; i < width && open; i++) open = isOpen(s.blockAt(lx + ax * i, yy, lz + az * i));
        if (!open) break;
        if (!isFrame(s.blockAt(lx - ax, yy, lz - az)) || !isFrame(s.blockAt(lx + ax * width, yy, lz + az * width)))
            return false;
    }
    if (height < PORTAL_MIN_H || height > PORTAL_MAX_H) return false;
    for (int i = 0; i < width; i++)
        if (!isFrame(s.blockAt(lx + ax * i, by + height, lz + az * i))) return false;
    cx = lx; cy = by; cz = lz; w = width; h = height;
    return true;
}

bool Server::lightPortal(int x, int y, int z) {
    for (uint8_t axis = 0; axis < 2; axis++) {
        int cx, cy, cz, w, h;
        if (!findFrame(*this, x, y, z, axis, cx, cy, cz, w, h)) continue;
        uint16_t st = portalState(axis);
        int ax = axis ? 0 : 1, az = axis ? 1 : 0;
        for (int i = 0; i < w; i++)
            for (int j = 0; j < h; j++) world.setBlock(curDim, cx + ax * i, cy + j, cz + az * i, st);
        PortalRef r;
        r.dim = curDim; r.axis = axis; r.x = cx; r.y = (int16_t)cy; r.z = cz;
        wstate.addPortal(r);
        playSound("block.portal.trigger", x + 0.5, y + 0.5, z + 0.5, 1, 1, 4);
        return true;
    }
    return false;
}

// A portal block next to (x, y, z), in its own plane, loses its frame when (x, y, z)
// becomes anything but portal or obsidian (vanilla: NetherPortalBlock#updateShape).
// The whole portal goes, removed in one flat pass (no recursion).
void Server::checkPortalsAround(int x, int y, int z) {
    uint16_t now = blockAt(x, y, z);
    if (isPortal(now) || isFrame(now)) return;
    static const int8_t D[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const auto& d : D) {
        int nx = x + d[0], ny = y + d[1], nz = z + d[2];
        uint16_t st = blockAt(nx, ny, nz);
        if (!isPortal(st)) continue;
        bool zAxis = getPropStr(st, "axis") && !strcmp(getPropStr(st, "axis"), "z");
        if ((zAxis && d[0]) || (!zAxis && d[2])) continue;   // across the portal's plane: no effect
        // flood the connected portal blocks in its plane
        struct P { int32_t x, z; int16_t y; };
        static const int MAXP = PORTAL_MAX_W * PORTAL_MAX_H;
        P* q = (P*)plat::bigAlloc(sizeof(P) * MAXP);
        if (!q) return;
        int head = 0, tail = 0;
        q[tail++] = {nx, nz, (int16_t)ny};
        world.setBlock(curDim, nx, ny, nz, 0);
        int ax = zAxis ? 0 : 1, az = zAxis ? 1 : 0;
        const int8_t N[4][3] = {{(int8_t)ax, 0, (int8_t)az}, {(int8_t)-ax, 0, (int8_t)-az}, {0, 1, 0}, {0, -1, 0}};
        while (head < tail) {
            P p = q[head++];
            for (const auto& n : N) {
                int qx = p.x + n[0], qy = p.y + n[1], qz = p.z + n[2];
                if (!isPortal(blockAt(qx, qy, qz)) || tail >= MAXP) continue;
                world.setBlock(curDim, qx, qy, qz, 0);
                q[tail++] = {qx, qz, (int16_t)qy};
            }
        }
        plat::bigFree(q);
        playSound("block.glass.break", nx + 0.5, ny + 0.5, nz + 0.5, 0.5f, 1, 4);
    }
}

// ------------------------------------------------------------------ linking
// The block a traveller through a nether portal aims at in dim.
static void scaledTarget(const Server& s, const Player& p, uint8_t dim, int& bx, int& bz) {
    double f = dim == DIM_NETHER ? 1.0 / 8 : 8.0;
    bx = (int)floor(p.e.x * f);
    bz = (int)floor(p.e.z * f);
    int lim = (int)s.meta.radius * 16 - 16;
    if (dim != DIM_NETHER) {
        bx = bx < -lim ? -lim : (bx > lim ? lim : bx);
        bz = bz < -lim ? -lim : (bz > lim ? lim : bz);
    }
}

int Server::nearestPortal(uint8_t dim, int bx, int bz) const {
    int best = -1;
    int64_t bd = 0;
    int r = searchRadius(dim);
    for (int i = 0; i < wstate.nPortals; i++) {
        const PortalRef& q = wstate.portals[i];
        if (q.dim != dim || abs(q.x - bx) > r || abs(q.z - bz) > r) continue;
        int64_t d = (int64_t)(q.x - bx) * (q.x - bx) + (int64_t)(q.z - bz) * (q.z - bz);
        if (best < 0 || d < bd) { best = i; bd = d; }
    }
    return best;
}

// Starts loading what a portal arrival in dim needs; true once it is all resident.
bool Server::portalArrivalReady(const Player& p, uint8_t dim) {
    int bx, bz;
    scaledTarget(*this, p, dim, bx, bz);
    int i = nearestPortal(dim, bx, bz);
    if (i >= 0) {   // the linked portal's chunk
        const PortalRef& q = wstate.portals[i];
        return chunkJobs.acquire(dim, q.x >> 4, q.z >> 4) != nullptr;
    }
    bool ready = true;   // the area a new portal may be built in
    for (int cx = (bx - BUILD_SEARCH) >> 4; cx <= (bx + BUILD_SEARCH) >> 4; cx++)
        for (int cz = (bz - BUILD_SEARCH) >> 4; cz <= (bz + BUILD_SEARCH) >> 4; cz++)
            if (!chunkJobs.acquire(dim, cx, cz)) ready = false;
    return ready;
}

// Room for a 4 x 5 frame (axis 0: along x) with its inside at (x, y, z): air (or
// replaceable plants) above solid, dry ground, also one block to each side of it.
static bool roomForPortal(Server& s, int x, int y, int z, uint8_t axis) {
    int ax = axis ? 0 : 1, az = axis ? 1 : 0, px = az, pz = ax;   // p: across the portal
    for (int i = -1; i <= 2; i++)
        for (int k = -1; k <= 1; k++) {
            int bx = x + ax * i + px * k, bz = z + az * i + pz * k;
            uint16_t ground = s.blockAt(bx, y - 1, bz);
            if (!stateCollides(ground) || stateIsFluid(ground) || blockIdOf(ground) == blk::Lava) return false;
            for (int j = 0; j < 4; j++) {
                uint16_t st = s.blockAt(bx, y + j, bz);
                if (!stateIsAir(st) && !(blockOf(st).flags & BF_REPLACEABLE)) return false;
                if (stateIsFluid(st)) return false;
            }
        }
    return true;
}

// Builds a 4 x 5 obsidian frame with a 2 x 3 portal, its inside's lower corner at
// (x, y, z); with an obsidian floor (and room) when it is built in the open.
static void buildPortal(Server& s, int x, int y, int z, uint8_t axis, bool platform) {
    int ax = axis ? 0 : 1, az = axis ? 1 : 0, px = az, pz = ax;
    if (platform)
        for (int i = -1; i <= 2; i++)
            for (int k = -1; k <= 1; k++) {
                int bx = x + ax * i + px * k, bz = z + az * i + pz * k;
                s.setBlock(bx, y - 1, bz, bs::Obsidian);
                for (int j = 0; j < 4; j++)
                    if (k != 0) s.setBlock(bx, y + j, bz, 0);
            }
    for (int i = -1; i <= 2; i++)
        for (int j = -1; j <= 3; j++) {
            int bx = x + ax * i, bz = z + az * i;
            bool edge = i == -1 || i == 2 || j == -1 || j == 3;
            s.world.setBlock(s.curDim, bx, y + j, bz, edge ? bs::Obsidian : portalState(axis));
        }
}

bool Server::portalArrival(Player& p, uint8_t dim, double& x, double& y, double& z, float& yaw) {
    InDim in(*this, dim);
    int bx, bz;
    scaledTarget(*this, p, dim, bx, bz);
    // a known portal: check it is still there (it may have been broken)
    for (int tries = 0; tries < 4; tries++) {
        int i = nearestPortal(dim, bx, bz);
        if (i < 0) break;
        PortalRef q = wstate.portals[i];
        if (!world.load(dim, q.x >> 4, q.z >> 4)) return false;
        if (isPortal(blockAt(q.x, q.y, q.z))) {
            int ax = q.axis ? 0 : 1, az = q.axis ? 1 : 0;
            x = q.x + ax * 1.0 + (q.axis ? 0.5 : 0.0);   // between the two inside columns
            z = q.z + az * 1.0 + (q.axis ? 0.0 : 0.5);
            y = q.y;
            yaw = q.axis ? 90.0f : 0.0f;
            return true;
        }
        wstate.removePortal(i);
    }
    // none: build one. The nearest place with room, scanning down from the top; in the
    // Nether below its ceiling, above its lava sea.
    int top = dim == DIM_NETHER ? 120 : dimMaxY(dim) - 5, bottom = dim == DIM_NETHER ? 32 : dimMinY(dim) + 2;
    uint8_t axis = (uint8_t)(fabs(p.e.yaw - 90) < 45 || fabs(p.e.yaw + 90) < 45 ? 1 : 0);
    for (int r = 0; r <= BUILD_SEARCH; r++)
        for (int dx = -r; dx <= r; dx++)
            for (int dz = -r; dz <= r; dz++) {
                if (dx != -r && dx != r && dz != -r && dz != r) continue;
                int cx = bx + dx, cz = bz + dz;
                if (!world.load(dim, cx >> 4, cz >> 4)) return false;
                int start = dim == DIM_NETHER ? top : world.heightAt(dim, cx, cz);
                if (start > top) start = top;
                for (int yy = start; yy >= bottom; yy--) {
                    for (uint8_t a = 0; a < 2; a++) {
                        uint8_t ax2 = (uint8_t)((axis + a) & 1);
                        if (!roomForPortal(*this, cx, yy, cz, ax2)) continue;
                        buildPortal(*this, cx, yy, cz, ax2, false);
                        PortalRef q;
                        q.dim = dim; q.axis = ax2; q.x = cx; q.y = (int16_t)yy; q.z = cz;
                        wstate.addPortal(q);
                        return portalArrival(p, dim, x, y, z, yaw);
                    }
                    if (dim != DIM_NETHER) break;   // the overworld: only on the surface
                }
            }
    // nowhere: in the open at the target, on a platform (vanilla does the same)
    int yy = dim == DIM_NETHER ? 70 : world.heightAt(dim, bx, bz);
    if (yy < 70 && dim == DIM_NETHER) yy = 70;
    if (yy > dimMaxY(dim) - 5) yy = dimMaxY(dim) - 5;
    if (yy < dimMinY(dim) + 5) yy = 70;
    buildPortal(*this, bx, yy, bz, axis, true);
    PortalRef q;
    q.dim = dim; q.axis = axis; q.x = bx; q.y = (int16_t)yy; q.z = bz;
    wstate.addPortal(q);
    return portalArrival(p, dim, x, y, z, yaw);
}

}  // namespace mc
