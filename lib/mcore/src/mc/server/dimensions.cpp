// Moving players between the overworld, the Nether and the End: the Respawn packet,
// arrival spots (with a platform when there is none), and portal blocks.
#include <math.h>
#include <string.h>
#include "mc/registry.h"
#include "mc/server/server.h"

namespace mc {

static constexpr int END_PLATFORM_X = 100, END_PLATFORM_Y = 48, END_PLATFORM_Z = 0;   // as vanilla
static constexpr int NETHER_PORTAL_TICKS = 80;   // standing in a portal in survival
// after arriving: no portal travel until the player has left the portal for this long
// (Player#getDimensionChangingDelay; 300 is for other entities)
static constexpr int PORTAL_COOLDOWN = 10;

const char* dimensionName(uint8_t dim) {
    static const char* const NAMES[NUM_DIMS] = {"overworld", "the_nether", "the_end"};
    return dim < NUM_DIMS ? NAMES[dim] : "?";
}

int parseDimension(const char* s) {
    if (!strncmp(s, "minecraft:", 10)) s += 10;
    if (!strcmp(s, "overworld")) return DIM_OVERWORLD;
    if (!strcmp(s, "the_nether") || !strcmp(s, "nether")) return DIM_NETHER;
    if (!strcmp(s, "the_end") || !strcmp(s, "end")) return DIM_END;
    return -1;
}

void Server::writeSpawnInfo(Writer& w, const Player& p) {
    uint8_t dim = p.e.dim;
    w.varint(DIMENSION_TYPE_ID[dim]);
    w.string(DIMENSION_NAME[dim]);
    w.i64((int64_t)mix64(meta.seed));
    w.i8((int8_t)p.gamemode);
    w.u8(0xFF);   // no previous game mode
    w.boolean(false);   // debug world
    w.boolean(meta.worldType == WORLD_FLAT && dim == DIM_OVERWORLD);
    w.boolean(false);   // no death location
    w.varint(0);        // portal cooldown
    w.varint(dim == DIM_NETHER ? 32 : 63);   // sea level
}

void Server::sendRespawn(Player& p) {
    {
        Packet pk(pkt::s2c::Respawn);
        writeSpawnInfo(pk.w, p);
        pk.w.u8(0);   // keep nothing
        p.conn.send(pk);
    }
    {   // the client waits for this and the chunks around it before it shows the world
        Packet pk(pkt::s2c::GameStateChange);
        pk.w.u8(13);
        pk.w.f32(0);
        p.conn.send(pk);
    }
    // the client dropped all chunks and entities
    p.resetView();
    forgetEntities(p);
    p.sendAbilities();
}

// The client state a Respawn packet resets, sent again after it (and after the teleport).
void Server::resendPlayerState(Player& p) {
    Server& s = *this;
    p.sendHealth();
    p.sendXp();
    p.sendInventory();
    {
        Packet pk(pkt::s2c::HeldItemSlot);
        pk.w.varint(p.held);
        p.conn.send(pk);
    }
    {
        Packet pk(pkt::s2c::SpawnPosition);
        pk.w.u64(packPos(s.meta.spawnX, s.meta.spawnY, s.meta.spawnZ));
        pk.w.f32(0);
        p.conn.send(pk);
    }
    p.sendTime();
    if (s.meta.raining && p.e.dim == DIM_OVERWORLD) s.sendWeather(&p);
    p.e.sx = p.e.x; p.e.sy = p.e.y; p.e.sz = p.e.z;
    p.e.sinceTeleport = 400;  // force a teleport packet to everybody
    p.e.metaDirty = true;
}

void Server::changeDimension(Player& p, uint8_t dim, double x, double y, double z, float yaw, float pitch) {
    if (dim >= NUM_DIMS) return;
    uint8_t from = p.e.dim;
    if (p.winKind != WK_NONE) {
        InDim in(*this, from);
        closeWindow(p, true);
    }
    p.e.dim = dim;
    p.e.vx = p.e.vy = p.e.vz = 0;
    p.e.fallDistance = 0;
    p.e.fireTicks = 0;
    p.portalTicks = 0;
    p.portalCooldown = PORTAL_COOLDOWN;
    sendRespawn(p);
    p.teleport(x, y, z, yaw, pitch);
    p.lastX = x; p.lastY = y; p.lastZ = z;
    resendPlayerState(p);
    MC_LOGI("%s: %s -> %s at %.1f %.1f %.1f", p.name, dimensionName(from), dimensionName(dim), x, y, z);
}

// Sets every block of a box (in curDim).
static void fillBox(Server& s, int x0, int y0, int z0, int x1, int y1, int z1, uint16_t st) {
    for (int x = x0; x <= x1; x++)
        for (int y = y0; y <= y1; y++)
            for (int z = z0; z <= z1; z++)
                if (s.blockAt(x, y, z) != st) s.setBlock(x, y, z, st);
}

// Room for a player at (x, y, z) in curDim: two free blocks over solid, dry ground.
static bool standable(Server& s, int x, int y, int z) {
    uint16_t below = s.blockAt(x, y - 1, z);
    return stateCollides(below) && !stateIsFluid(below) && stateIsAir(s.blockAt(x, y, z)) &&
           stateIsAir(s.blockAt(x, y + 1, z));
}

// The block column p arrives at (or searches around) in dim.
void Server::arrivalCentre(const Player& p, uint8_t dim, int& bx, int& bz) const {
    if (dim == DIM_END) {
        bx = END_PLATFORM_X;
        bz = END_PLATFORM_Z;
    } else if (dim == DIM_OVERWORLD) {
        if (p.e.dim == DIM_NETHER) {   // the Nether's coordinates are scaled 1:8
            bx = (int)floor(p.e.x * 8);
            bz = (int)floor(p.e.z * 8);
        } else {   // from the End: home
            bx = p.hasSpawn ? p.spawnX : meta.spawnX;
            bz = p.hasSpawn ? p.spawnZ : meta.spawnZ;
        }
        int lim = (int)meta.radius * 16 - 2;
        bx = bx < -lim ? -lim : (bx > lim ? lim : bx);
        bz = bz < -lim ? -lim : (bz > lim ? lim : bz);
    } else {
        bx = p.e.dim == DIM_OVERWORLD ? (int)floor(p.e.x / 8) : 0;
        bz = p.e.dim == DIM_OVERWORLD ? (int)floor(p.e.z / 8) : 0;
    }
}

// Nether arrivals search up to this many blocks around the centre.
static constexpr int NETHER_SEARCH = 8;

bool Server::arrivalSpot(Player& p, uint8_t dim, double& x, double& y, double& z, float& yaw) {
    InDim in(*this, dim);
    yaw = p.e.yaw;
    int cx, cz;
    arrivalCentre(p, dim, cx, cz);
    if (dim == DIM_END) {
        // vanilla's obsidian platform, rebuilt (and cleared above) on every arrival
        fillBox(*this, END_PLATFORM_X - 2, END_PLATFORM_Y, END_PLATFORM_Z - 2, END_PLATFORM_X + 2, END_PLATFORM_Y,
                END_PLATFORM_Z + 2, bs::Obsidian);
        fillBox(*this, END_PLATFORM_X - 2, END_PLATFORM_Y + 1, END_PLATFORM_Z - 2, END_PLATFORM_X + 2,
                END_PLATFORM_Y + 3, END_PLATFORM_Z + 2, bs::Air);
        x = END_PLATFORM_X + 0.5; y = END_PLATFORM_Y + 1; z = END_PLATFORM_Z + 0.5;
        yaw = 90;
        return true;
    }
    if (dim == DIM_OVERWORLD) {
        int bx = cx, bz = cz;
        Chunk* c = world.load(DIM_OVERWORLD, bx >> 4, bz >> 4);
        x = bx + 0.5; z = bz + 0.5;
        y = c ? c->height(bx & 15, bz & 15) : meta.spawnY;
        if (y < 1) y = meta.spawnY;
        return c != nullptr;
    }
    // the Nether: the overworld position / 8, in the nearest cave with room to stand
    for (int r = 0; r <= NETHER_SEARCH; r++)
        for (int dx = -r; dx <= r; dx++)
            for (int dz = -r; dz <= r; dz++) {
                if (dx != -r && dx != r && dz != -r && dz != r) continue;   // ring r only
                int bx = cx + dx, bz = cz + dz;
                if (!world.load(DIM_NETHER, bx >> 4, bz >> 4)) return false;
                // closest to y = 64 first: below it down to the lava sea, then above
                for (int k = 0; k < 2; k++)
                    for (int by = k ? 65 : 64; k ? by <= 120 : by >= 33; by += k ? 1 : -1)
                        if (standable(*this, bx, by, bz)) {
                            x = bx + 0.5; y = by; z = bz + 0.5;
                            return true;
                        }
            }
    // nothing: an obsidian platform with room above it
    fillBox(*this, cx - 1, 63, cz - 1, cx + 1, 63, cz + 1, bs::Obsidian);
    fillBox(*this, cx - 1, 64, cz - 1, cx + 1, 66, cz + 1, bs::Air);
    x = cx + 0.5; y = 64; z = cz + 0.5;
    return true;
}

// Starts background loads of the chunks the arrival needs; true when all are resident.
bool Server::arrivalReady(const Player& p, uint8_t dim) {
    if (p.travelPortal) return portalArrivalReady(p, dim);
    int bx, bz;
    arrivalCentre(p, dim, bx, bz);
    int r = dim == DIM_NETHER ? NETHER_SEARCH : 2;   // the End's platform is 5 x 5
    bool ready = true;
    for (int cx = (bx - r) >> 4; cx <= (bx + r) >> 4; cx++)
        for (int cz = (bz - r) >> 4; cz <= (bz + r) >> 4; cz++)
            if (!chunkJobs.acquire(dim, cx, cz)) ready = false;
    return ready;
}

bool Server::travel(Player& p, uint8_t dim, bool viaPortal) {
    if (dim >= NUM_DIMS) return false;
    // through a nether portal between the overworld and the Nether: to a linked portal
    p.travelPortal = viaPortal && dim != DIM_END && p.e.dim != DIM_END;
    // the destination's chunks are loaded (or generated) on the workers first: doing it
    // here would stall the game loop for every new chunk
    if (!arrivalReady(p, dim)) {
        p.travelTo = (int8_t)dim;
        p.travelWait = 0;
        return true;
    }
    p.travelTo = -1;
    double x, y, z;
    float yaw;
    bool ok = p.travelPortal ? portalArrival(p, dim, x, y, z, yaw) : arrivalSpot(p, dim, x, y, z, yaw);
    p.travelPortal = false;
    if (!ok) return false;
    changeDimension(p, dim, x, y, z, yaw, p.e.pitch);
    return true;
}

// A travel waiting for its chunks: go once they are there (after 10 s regardless; the
// arrival then loads what is missing itself).
void Server::tickTravel(Player& p) {
    if (p.travelTo < 0) return;
    uint8_t dim = (uint8_t)p.travelTo;
    if (p.dead) {
        p.travelTo = -1;
        p.travelPortal = false;
        return;
    }
    if (++p.travelWait < 200 && !arrivalReady(p, dim)) return;
    p.travelTo = -1;
    double x, y, z;
    float yaw;
    bool ok = p.travelPortal ? portalArrival(p, dim, x, y, z, yaw) : arrivalSpot(p, dim, x, y, z, yaw);
    p.travelPortal = false;
    if (ok) changeDimension(p, dim, x, y, z, yaw, p.e.pitch);
    else p.sendSystem("The destination is not available", "red");
}

// Called every tick for each player (in its dimension): portal blocks in the player's box.
void Server::tickPortal(Player& p) {
    tickTravel(p);
    if (p.portalCooldown > 0) p.portalCooldown--;
    if (p.travelTo >= 0) return;
    if (p.dead || p.gamemode == GM_SPECTATOR) {
        p.portalTicks = 0;
        return;
    }
    const Entity& e = p.e;
    double hw = e.width / 2.0;
    int x0 = (int)floor(e.x - hw), x1 = (int)floor(e.x + hw);
    int y0 = (int)floor(e.y), y1 = (int)floor(e.y + e.height);
    int z0 = (int)floor(e.z - hw), z1 = (int)floor(e.z + hw);
    bool nether = false, end = false;
    for (int x = x0; x <= x1; x++)
        for (int y = y0; y <= y1; y++)
            for (int z = z0; z <= z1; z++) {
                uint16_t id = blockIdOf(blockAt(x, y, z));
                nether |= id == blk::NetherPortal;
                end |= id == blk::EndPortal;
            }
    if (!nether && !end) {
        p.portalTicks = 0;
        return;
    }
    if (p.portalCooldown > 0) {   // still standing in the portal it arrived through
        p.portalCooldown = PORTAL_COOLDOWN;
        return;
    }
    if (end) {
        travel(p, p.e.dim == DIM_END ? DIM_OVERWORLD : DIM_END);
        return;
    }
    if (++p.portalTicks >= (p.gamemode == GM_CREATIVE ? 1 : NETHER_PORTAL_TICKS))
        travel(p, p.e.dim == DIM_NETHER ? DIM_OVERWORLD : DIM_NETHER, true);
}

}  // namespace mc
