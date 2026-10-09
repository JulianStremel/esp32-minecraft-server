// Sleeping (vanilla's Player#startSleepInBed and ServerLevel's sleep status): a player
// lies down in a bed at night or in a thunderstorm; the night passes only when every
// player who is not a spectator has slept for 100 ticks (playersSleepingPercentage 100,
// the default). Then it is morning, the weather clears and everyone wakes up.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "mc/registry.h"
#include "mc/server/server.h"

namespace mc {

static const int SLEEP_TICKS = 100;   // asleep this long before the night may pass

// Vanilla allows sleeping when the sky has darkened enough: from 12542 to 23459 in clear
// weather, from 12010 to 23991 in rain, at any time in a thunderstorm.
static bool sleepTime(const WorldMeta& m) {
    int64_t t = m.timeOfDay % 24000;
    if (m.raining == 2) return true;
    if (m.raining) return t >= 12010 && t <= 23991;
    return t >= 12542 && t <= 23459;
}

static void setOccupied(Server& s, int x, int y, int z, bool occupied) {
    uint16_t st = s.blockAt(x, y, z);
    if (!strstr(BLOCKS[blockIdOf(st)].name, "_bed")) return;
    int f = 0;
    static const char* const DIRS[] = {"down", "up", "north", "south", "west", "east"};
    const char* facing = getPropStr(st, "facing");
    for (int i = 0; i < 6; i++)
        if (facing && !strcmp(DIRS[i], facing)) f = i;
    int sgn = !strcmp(getPropStr(st, "part"), "head") ? -1 : 1;
    s.world.setBlock(s.curDim, x, y, z, setBool(st, "occupied", occupied), true, 3);
    int ox = x + FACE_DX[f] * sgn, oz = z + FACE_DZ[f] * sgn;
    uint16_t other = s.blockAt(ox, y, oz);
    if (blockIdOf(other) == blockIdOf(st)) s.world.setBlock(s.curDim, ox, y, oz, setBool(other, "occupied", occupied), true, 3);
}

// Uses the bed whose head is at (hx, y, hz). Returns the message for the player (nullptr:
// asleep).
const char* Server::trySleep(Player& p, int hx, int y, int hz) {
    if (p.sleeping) return nullptr;
    // reachable: within 3 blocks across and 2 up or down (Player#isReachableBedBlock)
    if (fabs(p.e.x - (hx + 0.5)) > 3 || fabs(p.e.y - y) > 2 || fabs(p.e.z - (hz + 0.5)) > 3)
        return "You may not rest now; the bed is too far away";
    if (stateCollides(blockAt(hx, y + 1, hz))) return "This bed is obstructed";
    // it becomes the spawn point either way
    bool newSpawn = !p.hasSpawn || p.spawnX != hx || p.spawnY != y || p.spawnZ != hz;
    p.hasSpawn = true;
    p.spawnX = hx; p.spawnY = y; p.spawnZ = hz;
    if (newSpawn) p.sendSystem("Respawn point set", nullptr);
    if (!sleepTime(meta)) return "You can sleep only at night or during thunderstorms";
    if (getBool(blockAt(hx, y, hz), "occupied")) return "This bed is occupied";
    if (p.gamemode != GM_CREATIVE) {   // monsters within 8 blocks across and 5 up or down
        for (int i = 0; i < MC_MAX_ENTITIES; i++) {
            const Entity& m = entities[i];
            if (m.kind != EK_MOB || m.removed || m.health <= 0 || !m.hostile || m.dim != p.e.dim) continue;
            if (fabs(m.x - (hx + 0.5)) <= 8 && fabs(m.y - y) <= 5 && fabs(m.z - (hz + 0.5)) <= 8)
                return "You may not rest now; there are monsters nearby";
        }
    }
    p.sleeping = true;
    p.sleepTicks = 0;
    p.sleepX = hx; p.sleepY = y; p.sleepZ = hz;
    p.e.pose = POSE_SLEEPING;
    p.e.vx = p.e.vy = p.e.vz = 0;
    p.e.metaDirty = true;
    setOccupied(*this, hx, y, hz, true);
    p.teleport(hx + 0.5, y + 0.6875, hz + 0.5, p.e.yaw, p.e.pitch);
    announceSleepers();
    return nullptr;
}

void Server::wakeUp(Player& p) {
    if (!p.sleeping) return;
    p.sleeping = false;
    p.sleepTicks = 0;
    p.e.pose = POSE_STANDING;
    p.e.metaDirty = true;
    {   // the wake-up animation (2), to everyone near and the sleeper itself (its client
        // closes the sleep screen on it)
        Packet pk(pkt::s2c::Animation);
        pk.w.varint(p.e.id);
        pk.w.u8(2);
        broadcastNearIn(p.e.dim, pk, (int)floor(p.e.x) >> 4, (int)floor(p.e.z) >> 4);
    }
    {
        InDim in(*this, p.e.dim);
        setOccupied(*this, p.sleepX, p.sleepY, p.sleepZ, false);
    }
    // up on the bed (it is 9/16 high)
    p.teleport(p.sleepX + 0.5, p.sleepY + 0.5625, p.sleepZ + 0.5, p.e.yaw, p.e.pitch);
}

// "x/y players sleeping" on everyone's action bar (in the overworld)
void Server::announceSleepers() {
    int asleep = 0, total = 0;
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        const Player& q = players[i];
        if (!q.inPlay() || q.gamemode == GM_SPECTATOR || q.e.dim != DIM_OVERWORLD) continue;
        total++;
        asleep += q.sleeping;
    }
    if (!asleep) return;
    char msg[48];
    snprintf(msg, sizeof(msg), "%d/%d players sleeping", asleep, total);
    for (int i = 0; i < MC_MAX_PLAYERS; i++)
        if (players[i].inPlay() && players[i].e.dim == DIM_OVERWORLD) players[i].sendActionBar(msg);
}

void Server::tickSleep() {
    int asleep = 0, ready = 0, total = 0;
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& q = players[i];
        if (!q.inPlay()) continue;
        if (q.sleeping) {
            InDim in(*this, q.e.dim);
            // the bed is gone, or the player went elsewhere: awake
            if (q.dead || q.e.dim != DIM_OVERWORLD || !strstr(BLOCKS[blockIdOf(blockAt(q.sleepX, q.sleepY, q.sleepZ))].name, "_bed")) {
                wakeUp(q);
                continue;
            }
            if (q.sleepTicks < SLEEP_TICKS) q.sleepTicks++;
        }
        if (q.gamemode == GM_SPECTATOR || q.e.dim != DIM_OVERWORLD) continue;
        total++;
        asleep += q.sleeping;
        ready += q.sleeping && q.sleepTicks >= SLEEP_TICKS;
    }
    if (!total || ready < total) return;
    // everyone slept: the next morning, clear weather, and up
    int64_t t = meta.timeOfDay % 24000;
    meta.timeOfDay += 24000 - t;
    if (meta.raining) {
        meta.raining = 0;
        meta.weatherTimer = 12000;
        sendWeather(nullptr);
    }
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        if (!players[i].inPlay()) continue;
        wakeUp(players[i]);
        players[i].sendTime();
    }
    (void)asleep;
}

}  // namespace mc
