// Mobs and dropped items live with their chunk, as in vanilla:
// - more than simulationDistance + 1 chunks from every player they leave the game and are
//   kept on their chunk (stashed), which is stored with them; within simulationDistance of
//   a player they come back. So the game only runs the entities near players, and a chunk
//   that is evicted (far from everyone) already holds its entities.
// - saving a chunk (autosave, /save-all, shutdown) stores copies of the entities in it that
//   are in the game; they stay in the game.
// - a chunk loaded from storage brings its entities back as stashed ones.
// Not saved: projectiles, falling blocks, primed TNT, fireballs, the dragon and its
// crystals and clouds (the dragon fight keeps a record of its own).
#include <math.h>
#include <stdlib.h>
#include "mc/platform.h"
#include "mc/registry.h"
#include "mc/server/server.h"

namespace mc {

static bool saveable(const Entity& e) {
    if (e.kind == EK_NONE || e.removed) return false;
    if (e.kind == EK_ITEM) return !e.item.empty();
    if (e.kind == EK_MOB) return e.health > 0 && e.type != ent::EnderDragon;
    return false;
}

static SavedEntity toSaved(const Entity& e) {
    SavedEntity s;
    s.kind = e.kind;
    s.type = e.type;
    s.variant = e.variant;
    s.size = e.size;
    s.x = e.x;
    s.y = e.y;
    s.z = e.z;
    s.vx = (float)e.vx;
    s.vy = (float)e.vy;
    s.vz = (float)e.vz;
    s.yaw = e.yaw;
    s.pitch = e.pitch;
    s.health = e.health;
    s.fireTicks = e.fireTicks;
    s.pickupDelay = e.pickupDelay;
    s.age = e.age;
    s.item = e.item;
    return s;
}

static void chunkOf(const Entity& e, int& cx, int& cz) {
    cx = (int)floor(e.x) >> 4;
    cz = (int)floor(e.z) >> 4;
}

// Chebyshev distance in chunks to the nearest player in the dimension (a large number
// if there is none).
static int playerDistance(Server& s, uint8_t dim, int cx, int cz) {
    int best = 1 << 30;
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        const Player& p = s.players[i];
        if (!p.inPlay() || p.e.dim != dim) continue;
        int pcx = (int)floor(p.e.x) >> 4, pcz = (int)floor(p.e.z) >> 4;
        int d = abs(cx - pcx) > abs(cz - pcz) ? abs(cx - pcx) : abs(cz - pcz);
        if (d < best) best = d;
    }
    return best;
}

Entity* Server::restoreEntity(const SavedEntity& s, uint8_t dim) {
    InDim in(*this, dim);
    Entity* e = nullptr;
    if (s.kind == EK_ITEM) {
        if (s.item.empty()) return nullptr;
        e = spawnEntity(EK_ITEM, ent::Item, s.x, s.y, s.z);
        if (!e) return nullptr;
        e->item = s.item;
        e->pickupDelay = s.pickupDelay;
        // items despawn 5 minutes after they were dropped, the time stored counting
        cancelEntityTimers(*e);
        scheduleEntityTimer(*e, ET_DESPAWN, s.age < 6000 ? (int)(6000 - s.age) : 1);
    } else if (s.kind == EK_MOB) {
        e = spawnMob(s.type, s.x, s.y, s.z);
        if (!e) return nullptr;
        e->health = s.health;
        e->variant = s.variant;
        if (s.type == ent::MagmaCube) setMagmaCubeSize(*e, s.size);
    } else {
        return nullptr;
    }
    e->vx = s.vx;
    e->vy = s.vy;
    e->vz = s.vz;
    e->yaw = e->headYaw = s.yaw;
    e->pitch = s.pitch;
    e->fireTicks = s.fireTicks;
    e->age = s.age;
    e->metaDirty = true;
    return e;
}

static bool inChunk(const Entity& e, const Chunk& c) {
    if (!saveable(e) || e.dim != c.dim) return false;
    int cx, cz;
    chunkOf(e, cx, cz);
    return cx == c.cx && cz == c.cz;
}

int Server::attachEntities(Chunk& target) {
    int n = 0;
    for (int i = 0; i < MC_MAX_ENTITIES; i++) n += inChunk(entities[i], target);
    target.clearLiveEntities();
    if (n) {
        // in PSRAM, not on the game loop's stack
        SavedEntity* list = new SavedEntity[n];
        if (list) {
            int k = 0;
            for (int i = 0; i < MC_MAX_ENTITIES && k < n; i++)
                if (inChunk(entities[i], target)) list[k++] = toSaved(entities[i]);
            target.setLiveEntities(list, k);
            delete[] list;
        }
    }
    return target.entCount + target.liveCount;
}

bool Server::stash(Entity& e) {
    int cx, cz;
    chunkOf(e, cx, cz);
    Chunk* c = world.peek(e.dim, cx, cz);
    if (!c || c->readOnly || !c->addEntity(toSaved(e))) return false;
    c->dirty = true;   // its stored copy must get them
    removeEntity(e);
    entityStats.stashed++;
    return true;
}

void Server::stashFarEntities() {
    const int keep = cfg.simulationDistance + 1;
    for (int i = 0; i < MC_MAX_ENTITIES; i++) {
        Entity& e = entities[i];
        if (!saveable(e)) continue;
        int cx, cz;
        chunkOf(e, cx, cz);
        if (playerDistance(*this, e.dim, cx, cz) > keep) stash(e);
    }
    // ... and back in the game near players
    for (int k = 0; k < world.tableSize(); k++) {
        Chunk* c = world.slot(k);
        if (!c || !c->entCount || playerDistance(*this, c->dim, c->cx, c->cz) > cfg.simulationDistance) continue;
        int done = 0;
        while (done < c->entCount && restoreEntity(c->ents[done], c->dim)) done++;
        if (!done) continue;   // no room for entities: later
        entityStats.unstashed += (uint32_t)done;
        // the rest stays stashed
        int left = c->entCount - done;
        SavedEntity* rest = left ? new SavedEntity[left] : nullptr;
        if (left && !rest) continue;
        for (int i = 0; i < left; i++) rest[i] = c->ents[done + i];
        c->clearEntities();
        c->ents = rest;
        c->entCount = (uint16_t)left;
        c->dirty = true;   // its stored copy must not bring them back a second time
    }
}

void Server::beforeEviction(Chunk& c) {
    for (int i = 0; i < MC_MAX_ENTITIES; i++)
        if (inChunk(entities[i], c) && !stash(entities[i])) removeEntity(entities[i]);
}

void Server::markEntityChunksDirty() {
    for (int i = 0; i < MC_MAX_ENTITIES; i++) {
        const Entity& e = entities[i];
        if (!saveable(e)) continue;
        int cx, cz;
        chunkOf(e, cx, cz);
        Chunk* c = world.peek(e.dim, cx, cz);
        if (c && !c->readOnly) c->dirty = true;
    }
}

}  // namespace mc
