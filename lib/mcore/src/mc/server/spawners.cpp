// Monster spawners (vanilla BaseSpawner): while a player is within 16 blocks, every 10 to
// 40 seconds up to 4 of the spawner's mob within 4 blocks where there is room, none
// while 6 of its kind are near. Placed by the structures (strongholds' silverfish).
#include <math.h>
#include "mc/registry.h"
#include "mc/server/mob_util.h"
#include "mc/server/server.h"

namespace mc {

namespace {
bool playerNear(Server& s, uint8_t dim, double x, double y, double z, double range) {
    for (int k = 0; k < MC_MAX_PLAYERS; k++) {
        const Player& p = s.players[k];
        if (!p.inPlay() || p.dead || p.gamemode == GM_SPECTATOR || p.e.dim != dim) continue;
        double dx = p.e.x - x, dy = p.e.y - y, dz = p.e.z - z;
        if (dx * dx + dy * dy + dz * dz < range * range) return true;
    }
    return false;
}
}  // namespace

void Server::tickSpawners() {
    Rng& r = mobs::rng();
    for (int i = 0; i < world.tableSize(); i++) {
        Chunk* c = world.slot(i);
        if (!c || !c->spawners()) continue;
        InDim in(*this, c->dim);
        for (TileEntity* t = c->tiles(); t; t = t->next) {
            if (t->type != TILE_SPAWNER || !t->spawnEntity) continue;
            double sx = c->cx * 16 + t->lx + 0.5, sy = t->y + 0.5, sz = c->cz * 16 + t->lz + 0.5;
            if (!playerNear(*this, c->dim, sx, sy, sz, 16)) continue;
            if (t->spawnDelay == -1) t->spawnDelay = (int16_t)(200 + r.range(600));
            if (t->spawnDelay > 0) {
                t->spawnDelay--;
                continue;
            }
            bool spawned = false, crowded = false;
            for (int k = 0; k < 4 && !crowded; k++) {
                double x = sx - 0.5 + (r.unit() - r.unit()) * 4 + 0.5, z = sz - 0.5 + (r.unit() - r.unit()) * 4 + 0.5;
                int y = t->y + r.range(3) - 1;
                int bx = (int)floor(x), bz = (int)floor(z);
                if (stateCollides(blockAt(bx, y, bz)) || stateCollides(blockAt(bx, y + 1, bz)) || stateIsFluid(blockAt(bx, y, bz)))
                    continue;
                if (cfg.difficulty == 0) continue;   // monsters only (the structures' spawners hold monsters)
                int near = 0;
                for (int m = 0; m < MC_MAX_ENTITIES; m++) {
                    const Entity& e = entities[m];
                    if (e.kind != EK_MOB || e.removed || e.dim != c->dim || e.type != t->spawnEntity) continue;
                    if (fabs(e.x - sx) <= 4.5 && fabs(e.y - sy) <= 4.5 && fabs(e.z - sz) <= 4.5) near++;
                }
                if (near >= 6) {
                    crowded = true;
                    break;
                }
                if (spawnMob(t->spawnEntity, x, y, z)) {
                    spawned = true;
                    Packet pk(pkt::s2c::WorldEvent);   // the spawner's flames
                    pk.w.i32(2004);
                    pk.w.u64(packPos(bx, y, bz));
                    pk.w.i32(0);
                    pk.w.boolean(false);
                    broadcastNear(pk, bx >> 4, bz >> 4, nullptr);
                }
            }
            if (spawned || crowded) t->spawnDelay = (int16_t)(200 + r.range(600));
        }
    }
}

}  // namespace mc
