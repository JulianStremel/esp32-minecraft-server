// Eyes of ender (vanilla EnderEyeItem, EyeOfEnder, EndPortalFrameBlock): an eye set into
// an end portal frame, the portal opening inside a ring of twelve frames with their eyes,
// each facing in; an eye thrown flies towards the nearest stronghold (its place from the
// world's generator: a vanilla-structures world, version 11 on), then drops back as an
// item (4 times in 5) or breaks.
#include <math.h>
#include <string.h>
#include "mc/registry.h"
#include "mc/server/mob_util.h"
#include "mc/server/server.h"

namespace mc {

namespace {
void worldEvent(Server& s, int type, int x, int y, int z, int data, bool global = false) {
    Packet pk(pkt::s2c::WorldEvent);
    pk.w.i32(type);
    pk.w.u64(packPos(x, y, z));
    pk.w.i32(data);
    pk.w.boolean(global);
    s.broadcastNear(pk, x >> 4, z >> 4, nullptr);
}

// the ring of twelve frames around a 3 x 3 centre: (dx, dz) from the centre, and the
// facing each must have (towards the centre)
struct RingPos { int8_t dx, dz; const char* facing; };
const RingPos RING[12] = {
    {-1, -2, "south"}, {0, -2, "south"}, {1, -2, "south"}, {-1, 2, "north"}, {0, 2, "north"}, {1, 2, "north"},
    {-2, -1, "east"},  {-2, 0, "east"},  {-2, 1, "east"},  {2, -1, "west"},  {2, 0, "west"},  {2, 1, "west"},
};
bool completeRing(Server& s, int cx, int y, int cz) {
    for (const RingPos& r : RING) {
        uint16_t st = s.blockAt(cx + r.dx, y, cz + r.dz);
        if (blockIdOf(st) != blk::EndPortalFrame || !getBool(st, "eye") || strcmp(getPropStr(st, "facing"), r.facing) != 0)
            return false;
    }
    return true;
}
}  // namespace

// EnderEyeItem#useOn: into a frame without an eye; the portal when the ring is complete
bool Server::useEnderEyeOnFrame(Player& p, int x, int y, int z, uint16_t st, ItemStack& it) {
    if (it.id != itm::EnderEye || blockIdOf(st) != blk::EndPortalFrame || getBool(st, "eye")) return false;
    setBlock(x, y, z, setBool(st, "eye", true));
    if (p.isSurvivalLike()) consumeHeld(p);
    worldEvent(*this, 1503, x, y, z, 0);   // the eye's sound and particles
    for (const RingPos& r : RING) {
        int cx = x - r.dx, cz = z - r.dz;
        if (!completeRing(*this, cx, y, cz)) continue;
        for (int dz = -1; dz <= 1; dz++)
            for (int dx = -1; dx <= 1; dx++) setBlock(cx + dx, y, cz + dz, bs::EndPortal);
        worldEvent(*this, 1038, cx, y, cz, 0, true);   // the portal opening, heard everywhere
        break;
    }
    return true;
}

// EnderEyeItem#use: thrown towards the nearest stronghold (nothing if there is none)
bool Server::throwEnderEye(Player& p) {
    if (p.heldItem().id != itm::EnderEye || p.e.dim != DIM_OVERWORLD) return false;
    int cx, cz;
    int px = (int)floor(p.e.x), pz = (int)floor(p.e.z);
    if (!generatorOf(DIM_OVERWORLD).locateStructure("stronghold", px, pz, 100 * 16, cx, cz)) return false;
    InDim in(*this, p.e.dim);
    Entity* e = spawnEntity(EK_EYE, ent::EyeOfEnder, p.e.x, p.e.y + 0.9, p.e.z);
    if (!e) return false;
    e->width = e->height = 0.25f;
    e->dim = p.e.dim;
    // EyeOfEnder#signalTo: 12 blocks towards the target and 8 up, or the target itself
    double tx = cx * 16 + 4, tz = cz * 16 + 4, ty = 0;   // the stronghold's start staircase
    double dx = tx - e->x, dz = tz - e->z, d = sqrt(dx * dx + dz * dz);
    if (d > 12) {
        tx = e->x + dx / d * 12;
        tz = e->z + dz / d * 12;
        ty = e->y + 8;
    }
    e->px = tx;
    e->py = ty;
    e->pz = tz;
    e->charge = mobs::rng().range(5) > 0 ? 1 : 0;   // survives (drops back)
    playSound("entity.ender_eye.launch", p.e.x, p.e.y, p.e.z, 0.5f, 0.4f, 0);
    if (p.isSurvivalLike()) consumeHeld(p);
    return true;
}

// EyeOfEnder#tick
void Server::tickEnderEye(Entity& e) {
    double hv = sqrt(e.vx * e.vx + e.vz * e.vz);
    e.x += e.vx;
    e.y += e.vy;
    e.z += e.vz;
    double dx = e.px - e.x, dz = e.pz - e.z, dh = sqrt(dx * dx + dz * dz);
    double angle = atan2(dz, dx);
    double speed = hv + (dh - hv) * 0.0025;
    double vy = e.vy;
    if (dh < 1.0) {
        speed *= 0.8;
        vy *= 0.8;
    }
    int up = e.y < e.py ? 1 : -1;
    e.vx = cos(angle) * speed;
    e.vz = sin(angle) * speed;
    e.vy = vy + ((double)up - vy) * 0.015;
    e.velDirty = true;
    if (++e.age > 80) {
        playSound("entity.ender_eye.death", e.x, e.y, e.z, 1, 1, 0);
        if (e.charge) {
            ItemStack eye;
            eye.id = itm::EnderEye;
            eye.count = 1;
            dropItem(e.x, e.y, e.z, eye, false);
        } else {
            worldEvent(*this, 2003, (int)floor(e.x), (int)floor(e.y), (int)floor(e.z), 0);   // it breaks
        }
        removeEntity(e);
    }
}

}  // namespace mc
