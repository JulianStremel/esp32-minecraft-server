// Entities: pool management, client tracking, physics, items, mobs and combat.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "mc/registry.h"
#include "mc/server/server.h"
#include "mc/server/piston.h"
#include "mc/world/noise.h"

namespace mc {

static const double TRACK_RANGE = 64.0;

static Rng s_rng(0x5EED);

static void randomUuid(uint8_t u[16]) {
    for (int i = 0; i < 16; i += 4) {
        uint32_t r = plat::random32();
        memcpy(u + i, &r, 4);
    }
    u[6] = (u[6] & 0x0F) | 0x40;
    u[8] = (u[8] & 0x3F) | 0x80;
}

// ------------------------------------------------------------------ pool
Entity* Server::spawnEntity(uint8_t kind, uint16_t type, double x, double y, double z) {
    for (int i = 0; i < MC_MAX_ENTITIES; i++) {
        Entity& e = entities[i];
        if (e.kind != EK_NONE) continue;
        e = Entity();
        e.kind = kind;
        e.dim = curDim;
        e.type = type;
        e.id = newEntityId();
        randomUuid(e.uuid);
        e.x = e.sx = x;
        e.y = e.sy = y;
        e.z = e.sz = z;
        if (type < NUM_ENTITY_TYPES) {
            e.width = ENTITY_TYPES[type].width;
            e.height = ENTITY_TYPES[type].height;
        }
        if (kind == EK_ITEM) scheduleEntityTimer(e, ET_DESPAWN, 6000);   // 5 minutes, as in vanilla
        return &e;
    }
    return nullptr;
}

Entity* Server::findEntity(int32_t id) {
    for (int i = 0; i < MC_MAX_PLAYERS; i++)
        if (players[i].state == CS_PLAY && players[i].e.id == id) return &players[i].e;
    for (int i = 0; i < MC_MAX_ENTITIES; i++)
        if (entities[i].kind != EK_NONE && !entities[i].removed && entities[i].id == id) return &entities[i];
    return nullptr;
}

void Server::removeEntity(Entity& e) {
    e.removed = true;
    cancelEntityTimers(e);
}

// ------------------------------------------------------------------ mob timers (timer wheel)
void Server::scheduleEntityTimer(const Entity& e, uint16_t timer, int delay) {
    timers.schedule(TimerKey::entity(e.id, timer), worldTick() + (uint32_t)(delay > 0 ? delay : 1));
}

void Server::cancelEntityTimers(const Entity& e) {
    for (uint16_t t = ET_DESPAWN; t <= ET_WANDER; t++) timers.cancel(TimerKey::entity(e.id, t));
}

int Server::mobCount() const {
    int n = 0;
    for (int i = 0; i < MC_MAX_ENTITIES; i++)
        if (entities[i].kind == EK_MOB && !entities[i].removed) n++;
    return n;
}

Entity* Server::dropItem(double x, double y, double z, const ItemStack& st, bool scatter) {
    if (st.empty()) return nullptr;
    Entity* e = spawnEntity(EK_ITEM, ent::Item, x, y, z);
    if (!e) return nullptr;
    e->item = st;
    e->pickupDelay = 10;
    if (scatter) {
        e->vx = (s_rng.unit() - 0.5) * 0.2;
        e->vy = 0.2;
        e->vz = (s_rng.unit() - 0.5) * 0.2;
    }
    return e;
}

void Server::throwItem(Player& p, const ItemStack& st) {
    double yaw = p.e.yaw * M_PI / 180.0, pitch = p.e.pitch * M_PI / 180.0;
    Entity* e = dropItem(p.e.x, p.e.y + 1.32, p.e.z, st, false);
    if (!e) return;
    e->vx = -sin(yaw) * cos(pitch) * 0.3;
    e->vz = cos(yaw) * cos(pitch) * 0.3;
    e->vy = -sin(pitch) * 0.3 + 0.1;
    e->pickupDelay = 40;
    e->owner = p.e.id;
}

// ------------------------------------------------------------------ packets
static void writeVelocity(Writer& w, const Entity& e) {
    auto q = [](double v) {
        double c = v * 8000.0;
        if (c > 32767) c = 32767;
        if (c < -32768) c = -32768;
        return (int16_t)c;
    };
    w.i16(q(e.vx));
    w.i16(q(e.vy));
    w.i16(q(e.vz));
}

void Server::writeMetadata(Writer& w, const Entity& e, bool full) {
    w.u8(0); w.varint(0); w.u8(e.flags | (e.fireTicks > 0 ? EF_ON_FIRE : 0));
    w.u8(6); w.varint(18); w.varint(e.pose);
    if (e.kind == EK_PLAYER) {
        const Player& p = players[e.playerSlot];
        w.u8(16); w.varint(0); w.u8(p.skinParts);
        w.u8(17); w.varint(0); w.u8(p.mainHand);
        w.u8(1); w.varint(1); w.varint(e.air);
    } else if (e.kind == EK_ITEM) {
        w.u8(7); w.varint(6); writeSlot(w, e.item);
    } else if (e.kind == EK_TNT) {
        w.u8(7); w.varint(1); w.varint(e.fuse);
    } else if (e.kind == EK_MOB) {
        if (e.type == ent::Sheep) { w.u8(16); w.varint(0); w.u8(e.variant); }
        if (e.type == ent::Creeper) {
            w.u8(15); w.varint(1); w.varint(e.fuse >= 0 ? 1 : -1);
            w.u8(17); w.varint(7); w.boolean(e.fuse >= 0);
        }
    }
    (void)full;
    w.u8(0xFF);
}

void Server::sendSpawn(Player& to, Entity& e) {
    if (e.kind == EK_PLAYER) {
        Packet pk(pkt::s2c::NamedEntitySpawn);
        pk.w.varint(e.id);
        pk.w.uuid(e.uuid);
        pk.w.f64(e.x);
        pk.w.f64(e.y);
        pk.w.f64(e.z);
        pk.w.u8(angleByte(e.yaw));
        pk.w.u8(angleByte(e.pitch));
        to.conn.send(pk);
    } else if (e.kind == EK_MOB) {
        Packet pk(pkt::s2c::SpawnEntityLiving);
        pk.w.varint(e.id);
        pk.w.uuid(e.uuid);
        pk.w.varint(e.type);
        pk.w.f64(e.x);
        pk.w.f64(e.y);
        pk.w.f64(e.z);
        pk.w.u8(angleByte(e.yaw));
        pk.w.u8(angleByte(e.pitch));
        pk.w.u8(angleByte(e.headYaw));
        writeVelocity(pk.w, e);
        to.conn.send(pk);
    } else {
        int32_t data = 0;
        if (e.kind == EK_ITEM) data = 1;
        else if (e.kind == EK_FALLING_BLOCK) data = e.blockState;
        else if (e.kind == EK_ARROW) data = e.owner >= 0 ? e.owner + 1 : 0;
        Packet pk(pkt::s2c::SpawnEntity);
        pk.w.varint(e.id);
        pk.w.uuid(e.uuid);
        pk.w.varint(e.type);
        pk.w.f64(e.x);
        pk.w.f64(e.y);
        pk.w.f64(e.z);
        pk.w.u8(angleByte(e.pitch));
        pk.w.u8(angleByte(e.yaw));
        pk.w.i32(data);
        writeVelocity(pk.w, e);
        to.conn.send(pk);
    }
    to.conn.sendStreamed([&](Writer& w) {
        w.varint(pkt::s2c::EntityMetadata); w.varint(e.id);
        writeMetadata(w, e, true);
    });
    if (e.kind == EK_PLAYER || e.kind == EK_MOB) {
        Packet pk(pkt::s2c::EntityHeadRotation);
        pk.w.varint(e.id);
        pk.w.u8(angleByte(e.headYaw));
        to.conn.send(pk);
    }
    if (e.kind == EK_PLAYER) {
        Player& p = players[e.playerSlot];
        to.conn.sendStreamed([&](Writer& w) {
            w.varint(pkt::s2c::EntityEquipment); w.varint(e.id);
            const ItemStack* items[6] = {&p.inv[SLOT_HOTBAR_START + p.held], &p.inv[SLOT_OFFHAND],
                                         &p.inv[8], &p.inv[7], &p.inv[6], &p.inv[5]};
            for (int slot = 0; slot < 6; ++slot) {
                w.u8((uint8_t)(slot | (slot < 5 ? 0x80 : 0))); writeSlot(w, *items[slot]);
            }
        });
    }
}

void Server::sendDestroy(Player& to, int32_t id) {
    Packet pk(pkt::s2c::EntityDestroy);
    pk.w.varint(1);
    pk.w.varint(id);
    to.conn.send(pk);
}

void Server::broadcastMetadata(Entity& e) {
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& p = players[i];
        if (!p.inPlay()) continue;
        bool knows = e.kind == EK_PLAYER ? (p.knownPlayers & (1u << e.playerSlot)) != 0
                                         : (p.knownEntities[(&e - entities) >> 3] & (1 << ((&e - entities) & 7))) != 0;
        if (knows || (e.kind == EK_PLAYER && &p.e == &e)) p.conn.sendStreamed([&](Writer& w) {
            w.varint(pkt::s2c::EntityMetadata); w.varint(e.id); writeMetadata(w, e, false);
        });
    }
}

void Server::broadcastEquipment(Player& pl) {
    auto body = [&](Writer& w) {
        w.varint(pkt::s2c::EntityEquipment); w.varint(pl.e.id);
        const ItemStack* items[6] = {&pl.inv[SLOT_HOTBAR_START + pl.held], &pl.inv[SLOT_OFFHAND],
                                     &pl.inv[8], &pl.inv[7], &pl.inv[6], &pl.inv[5]};
        for (int slot = 0; slot < 6; ++slot) {
            w.u8((uint8_t)(slot | (slot < 5 ? 0x80 : 0))); writeSlot(w, *items[slot]);
        }
    };
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& p = players[i];
        if (&p != &pl && p.inPlay() && (p.knownPlayers & (1u << pl.slot))) p.conn.sendStreamed(body);
    }
}

void Server::broadcastAnimation(Entity& e, uint8_t anim, const Player* except) {
    Packet pk(pkt::s2c::Animation);
    pk.w.varint(e.id);
    pk.w.u8(anim);
    broadcastNear(pk, (int)floor(e.x) >> 4, (int)floor(e.z) >> 4, except);
}

void Server::broadcastStatus(Entity& e, int8_t status) {
    Packet pk(pkt::s2c::EntityStatus);
    pk.w.i32(e.id);
    pk.w.i8(status);
    broadcastNear(pk, (int)floor(e.x) >> 4, (int)floor(e.z) >> 4);
}

void Server::playSound(const char* name, double x, double y, double z, float volume, float pitch, int category) {
    Packet pk(pkt::s2c::NamedSoundEffect);
    pk.w.string(name);
    pk.w.varint(category);
    pk.w.i32((int32_t)(x * 8));
    pk.w.i32((int32_t)(y * 8));
    pk.w.i32((int32_t)(z * 8));
    pk.w.f32(volume);
    pk.w.f32(pitch);
    broadcastNear(pk, (int)floor(x) >> 4, (int)floor(z) >> 4);
}

// ------------------------------------------------------------------ tracking
static bool knownBit(const Player& p, int idx) { return (p.knownEntities[idx >> 3] >> (idx & 7)) & 1; }
static void setKnown(Player& p, int idx, bool v) {
    if (v) p.knownEntities[idx >> 3] |= (uint8_t)(1 << (idx & 7));
    else p.knownEntities[idx >> 3] &= (uint8_t)~(1 << (idx & 7));
}

static bool inRange(const Player& p, const Entity& e) {
    double dx = p.e.x - e.x, dz = p.e.z - e.z;
    if (dx * dx + dz * dz > TRACK_RANGE * TRACK_RANGE) return false;
    return p.hasChunk(e.dim, (int)floor(e.x) >> 4, (int)floor(e.z) >> 4);
}

// Sends position/rotation deltas of e to all players that know it.
static void syncMovement(Server& s, Entity& e, uint32_t knowMaskPlayers, int poolIdx) {
    int32_t dx = (int32_t)lround((e.x - e.sx) * 4096.0);
    int32_t dy = (int32_t)lround((e.y - e.sy) * 4096.0);
    int32_t dz = (int32_t)lround((e.z - e.sz) * 4096.0);
    uint8_t yaw = angleByte(e.yaw), pitch = angleByte(e.pitch), head = angleByte(e.headYaw);
    bool moved = dx || dy || dz;
    bool rotated = yaw != e.syaw || pitch != e.spitch;
    bool headChanged = head != e.shead;
    e.sinceTeleport++;
    bool far = dx > 32767 || dx < -32768 || dy > 32767 || dy < -32768 || dz > 32767 || dz < -32768;
    if (!moved && !rotated && !headChanged && !e.velDirty && e.sinceTeleport < 400) return;
    auto sendToKnowers = [&](const Packet& pk) {
        for (int i = 0; i < MC_MAX_PLAYERS; i++) {
            Player& p = s.players[i];
            if (!p.inPlay()) continue;
            bool knows = poolIdx < 0 ? (p.knownPlayers & knowMaskPlayers) != 0 : knownBit(p, poolIdx);
            if (knows) p.conn.send(pk);
        }
    };
    if (far || e.sinceTeleport >= 400) {
        Packet pk(pkt::s2c::EntityTeleport);
        pk.w.varint(e.id);
        pk.w.f64(e.x);
        pk.w.f64(e.y);
        pk.w.f64(e.z);
        pk.w.u8(yaw);
        pk.w.u8(pitch);
        pk.w.boolean(e.onGround);
        sendToKnowers(pk);
        e.sx = e.x; e.sy = e.y; e.sz = e.z;
        e.sinceTeleport = 0;
    } else if (moved && rotated) {
        Packet pk(pkt::s2c::EntityMoveLook);
        pk.w.varint(e.id);
        pk.w.i16((int16_t)dx);
        pk.w.i16((int16_t)dy);
        pk.w.i16((int16_t)dz);
        pk.w.u8(yaw);
        pk.w.u8(pitch);
        pk.w.boolean(e.onGround);
        sendToKnowers(pk);
    } else if (moved) {
        Packet pk(pkt::s2c::RelEntityMove);
        pk.w.varint(e.id);
        pk.w.i16((int16_t)dx);
        pk.w.i16((int16_t)dy);
        pk.w.i16((int16_t)dz);
        pk.w.boolean(e.onGround);
        sendToKnowers(pk);
    } else if (rotated) {
        Packet pk(pkt::s2c::EntityLook);
        pk.w.varint(e.id);
        pk.w.u8(yaw);
        pk.w.u8(pitch);
        pk.w.boolean(e.onGround);
        sendToKnowers(pk);
    }
    if (moved && !far) {
        e.sx += dx / 4096.0;
        e.sy += dy / 4096.0;
        e.sz += dz / 4096.0;
    }
    e.syaw = yaw;
    e.spitch = pitch;
    if (headChanged) {
        Packet pk(pkt::s2c::EntityHeadRotation);
        pk.w.varint(e.id);
        pk.w.u8(head);
        sendToKnowers(pk);
        e.shead = head;
    }
    if (e.velDirty) {
        Packet pk(pkt::s2c::EntityVelocity);
        pk.w.varint(e.id);
        writeVelocity(pk.w, e);
        sendToKnowers(pk);
        e.velDirty = false;
    }
}

void Server::forgetEntities(Player& p) {
    p.knownPlayers = 0;
    memset(p.knownEntities, 0, sizeof(p.knownEntities));
}

void Server::trackEntities() {
    // 1) visibility changes
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& p = players[i];
        if (!p.inPlay()) continue;
        for (int j = 0; j < MC_MAX_PLAYERS; j++) {
            if (j == i) continue;
            Player& o = players[j];
            bool visible = o.inPlay() && !(o.dead && o.e.deathTicks > 20) && inRange(p, o.e);
            bool known = (p.knownPlayers >> j) & 1;
            if (visible && !known) {
                sendSpawn(p, o.e);
                p.knownPlayers |= 1u << j;
            } else if (!visible && known) {
                if (o.inPlay()) sendDestroy(p, o.e.id);
                p.knownPlayers &= ~(1u << j);
            }
        }
        for (int k = 0; k < MC_MAX_ENTITIES; k++) {
            Entity& e = entities[k];
            bool known = knownBit(p, k);
            if (e.kind == EK_NONE) {
                if (known) setKnown(p, k, false);
                continue;
            }
            bool visible = !e.removed && inRange(p, e);
            if (visible && !known) {
                sendSpawn(p, e);
                setKnown(p, k, true);
            } else if (!visible && known) {
                sendDestroy(p, e.id);
                setKnown(p, k, false);
            }
        }
    }
    // 2) free removed entities (everyone got the destroy above)
    for (int k = 0; k < MC_MAX_ENTITIES; k++)
        if (entities[k].kind != EK_NONE && entities[k].removed) entities[k].kind = EK_NONE;
    // 3) movement / metadata
    for (int j = 0; j < MC_MAX_PLAYERS; j++) {
        Player& o = players[j];
        if (!o.inPlay()) continue;
        syncMovement(*this, o.e, 1u << j, -1);
        if (o.e.metaDirty) {
            broadcastMetadata(o.e);
            o.e.metaDirty = false;
        }
    }
    for (int k = 0; k < MC_MAX_ENTITIES; k++) {
        Entity& e = entities[k];
        if (e.kind == EK_NONE) continue;
        // items and falling blocks move smoothly on the client; sync them less often
        if ((e.kind == EK_ITEM || e.kind == EK_FALLING_BLOCK) && (ticks + k) % 4 != 0 && !e.velDirty) continue;
        syncMovement(*this, e, 0, k);
        if (e.metaDirty) {
            broadcastMetadata(e);
            e.metaDirty = false;
        }
    }
}

// ------------------------------------------------------------------ physics
static bool solidAt(Server& s, int x, int y, int z) {
    if (y < 0) return false;
    uint16_t st = s.world.getBlock(s.curDim, x, y, z, bs::Stone);  // unloaded chunks count as solid
    return stateCollides(st);
}

static bool boxCollides(Server& s, double x, double y, double z, float w, float h) {
    double hw = w / 2.0;
    int x0 = (int)floor(x - hw + 0.001), x1 = (int)floor(x + hw - 0.001);
    int y0 = (int)floor(y + 0.001), y1 = (int)floor(y + h - 0.001);
    int z0 = (int)floor(z - hw + 0.001), z1 = (int)floor(z + hw - 0.001);
    bool moving = false;
    for (int cx = (x0 - 1) >> 4; cx <= (x1 + 1) >> 4; ++cx)
        for (int cz = (z0 - 1) >> 4; cz <= (z1 + 1) >> 4; ++cz) {
            Chunk* c = s.world.peek(s.curDim, cx, cz);
            if (c && c->movingPistons()) moving = true;
        }
    if (!moving) {
        for (int bx = x0; bx <= x1; ++bx) for (int by = y0; by <= y1; ++by) for (int bz = z0; bz <= z1; ++bz)
            if (solidAt(s, bx, by, bz)) return true;
        return false;
    }
    PistonBox entityBounds{{x - hw + .001, y + .001, z - hw + .001}, {x + hw - .001, y + h - .001, z + hw - .001}};
    for (int bx = x0 - 1; bx <= x1 + 1; bx++)
        for (int by = y0 - 1; by <= y1 + 1; by++)
            for (int bz = z0 - 1; bz <= z1 + 1; bz++) {
                if (blockIdOf(s.blockAt(bx, by, bz)) == blk::MovingPiston) {
                    PistonBox boxes[16]; int n = Pistons::collision(s, {bx,by,bz}, boxes);
                    for (int i = 0; i < n; ++i) if (boxes[i].intersects(entityBounds)) return true;
                } else if (bx >= x0 && bx <= x1 && by >= y0 && by <= y1 && bz >= z0 && bz <= z1 && solidAt(s, bx, by, bz)) return true;
            }
    return false;
}

static bool inFluid(Server& s, const Entity& e, uint16_t blockId) {
    uint16_t st = s.world.getBlock(s.curDim, (int)floor(e.x), (int)floor(e.y + 0.1), (int)floor(e.z));
    return blockIdOf(st) == blockId;
}

// Moves e by its velocity with block collisions. Returns true if blocked horizontally.
static bool moveEntity(Server& s, Entity& e) {
    bool blockedH = false;
    // vertical
    double ny = e.y + e.vy;
    if (boxCollides(s, e.x, ny, e.z, e.width, e.height)) {
        if (e.vy < 0) {
            e.y = floor(ny) + 1.0;
            if (boxCollides(s, e.x, e.y, e.z, e.width, e.height)) e.y = ceil(e.y);  // safety
            e.onGround = true;
        }
        e.vy = 0;
    } else {
        e.y = ny;
        e.onGround = false;
    }
    // horizontal, per axis
    double nx = e.x + e.vx;
    if (boxCollides(s, nx, e.y, e.z, e.width, e.height)) { e.vx = 0; blockedH = true; }
    else e.x = nx;
    double nz = e.z + e.vz;
    if (boxCollides(s, e.x, e.y, nz, e.width, e.height)) { e.vz = 0; blockedH = true; }
    else e.z = nz;
    if (!e.onGround && e.vy == 0 && boxCollides(s, e.x, e.y - 0.05, e.z, e.width, 0.05f)) e.onGround = true;
    return blockedH;
}

static bool isDay(const Server& s) {
    int64_t t = s.meta.timeOfDay % 24000;
    return t < 12300 || t > 23850;
}

// ------------------------------------------------------------------ mob helpers
struct MobInfo { uint16_t type; float speed; float health; float attack; bool hostile; bool burns; };
static const MobInfo MOBS[] = {
    {ent::Pig, 0.12f, 10, 0, false, false},       {ent::Cow, 0.10f, 10, 0, false, false},
    {ent::Sheep, 0.11f, 8, 0, false, false},      {ent::Chicken, 0.12f, 4, 0, false, false},
    {ent::Zombie, 0.14f, 20, 3, true, true},      {ent::Skeleton, 0.13f, 20, 2, true, true},
    {ent::Creeper, 0.13f, 20, 0, true, false},    {ent::Spider, 0.17f, 16, 2, true, false},
};

static const MobInfo* mobInfo(uint16_t type) {
    for (const MobInfo& m : MOBS)
        if (m.type == type) return &m;
    return nullptr;
}

Entity* Server::spawnMob(uint16_t type, double x, double y, double z) {
    const MobInfo* mi = mobInfo(type);
    Entity* e = spawnEntity(EK_MOB, type, x, y, z);
    if (!e) return nullptr;
    e->health = e->maxHealth = mi ? mi->health : 10;
    e->hostile = mi ? mi->hostile : false;
    e->burnsInDay = mi ? mi->burns : false;
    e->yaw = e->headYaw = s_rng.unit() * 360.0f;
    if (type == ent::Sheep) {
        static const uint8_t colors[] = {0, 0, 0, 0, 0, 0, 7, 8, 15, 12, 6};
        e->variant = colors[s_rng.range(11)];
    }
    scheduleEntityTimer(*e, ET_WANDER, 1 + (int)s_rng.range(60));
    return e;
}

// An idle mob picks somewhere new to walk to now and then (or decides to stand still).
static void planWander(Entity& e) {
    if (s_rng.range(3) != 0) {
        e.goalX = (float)(e.x + s_rng.between(-8, 8));
        e.goalZ = (float)(e.z + s_rng.between(-8, 8));
        e.hasGoal = true;
    } else {
        e.hasGoal = false;
    }
    e.lastAttacker = -1;
}

void Server::runEntityTimer(const TimerEvent& ev) {
    Entity* e = findEntity(ev.key.x);
    if (!e || e->kind == EK_PLAYER) return;
    InDim in(*this, e->dim);
    switch (ev.key.data) {
        case ET_DESPAWN:
            removeEntity(*e);
            break;
        case ET_CORPSE:
            removeEntity(*e);
            break;
        case ET_FUSE:
            if (e->health > 0 && e->fuse >= 0) {
                explode(e->x, e->y + 0.5, e->z, 3.0f, e->id);
                e->health = 0;
                removeEntity(*e);
            }
            break;
        case ET_CALM:
            if (!e->hostile) e->lastAttacker = -1;
            break;
        case ET_WANDER:
            if (e->health <= 0) break;
            // only when idle: not chasing, not fleeing, not about to explode
            if (e->target < 0 && !(!e->hostile && e->lastAttacker >= 0) && e->fuse < 0) planWander(*e);
            scheduleEntityTimer(*e, ET_WANDER, 60 + (int)s_rng.range(140));
            break;
        default:
            break;
    }
}

static void lookAt(Entity& e, double tx, double tz) {
    double dx = tx - e.x, dz = tz - e.z;
    e.yaw = (float)(atan2(-dx, dz) * 180.0 / M_PI);
    e.headYaw = e.yaw;
}

static Player* nearestTarget(Server& s, const Entity& e, double range) {
    Player* best = nullptr;
    double bd = range * range;
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& p = s.players[i];
        if (!p.inPlay() || p.dead || !p.isSurvivalLike() || p.e.dim != e.dim) continue;
        double dx = p.e.x - e.x, dy = p.e.y - e.y, dz = p.e.z - e.z;
        double d = dx * dx + dy * dy + dz * dz;
        if (d < bd) { bd = d; best = &p; }
    }
    return best;
}

static void dropMobLoot(Server& s, Entity& e) {
    auto drop = [&](uint16_t item, int lo, int hi) {
        int n = lo + s_rng.range(hi - lo + 1);
        if (n > 0) s.dropItem(e.x, e.y + 0.5, e.z, ItemStack::of(item, n));
    };
    bool burning = e.fireTicks > 0;
    switch (e.type) {
        case ent::Pig: drop(burning ? itm::CookedPorkchop : itm::Porkchop, 1, 3); break;
        case ent::Cow: drop(itm::Leather, 0, 2); drop(burning ? itm::CookedBeef : itm::Beef, 1, 3); break;
        case ent::Sheep:
            if (!(e.variant & 0x10)) drop((uint16_t)(itm::WhiteWool + (e.variant & 15)), 1, 1);
            drop(burning ? itm::CookedMutton : itm::Mutton, 1, 2);
            break;
        case ent::Chicken: drop(itm::Feather, 0, 2); drop(burning ? itm::CookedChicken : itm::Chicken, 1, 1); break;
        case ent::Zombie: drop(itm::RottenFlesh, 0, 2); break;
        case ent::Skeleton: drop(itm::Bone, 0, 2); drop(itm::Arrow, 0, 2); break;
        case ent::Creeper: drop(itm::Gunpowder, 0, 2); break;
        case ent::Spider: drop(itm::String, 0, 2); drop(itm::SpiderEye, 0, 1); break;
        default: break;
    }
}

static void shootArrow(Server& s, Entity& shooter, double tx, double ty, double tz, float speed, float damage) {
    Entity* a = s.spawnEntity(EK_ARROW, ent::Arrow, shooter.x, shooter.y + shooter.height * 0.85, shooter.z);
    if (!a) return;
    double dx = tx - a->x, dy = ty - a->y, dz = tz - a->z;
    double hd = sqrt(dx * dx + dz * dz);
    dy += hd * 0.2;  // aim a bit up to compensate gravity
    double len = sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 0.01) len = 0.01;
    a->vx = dx / len * speed;
    a->vy = dy / len * speed;
    a->vz = dz / len * speed;
    a->owner = shooter.id;
    a->damage = damage;
    a->yaw = (float)(atan2(-dx, dz) * 180.0 / M_PI);
    a->velDirty = true;
    s.playSound("entity.arrow.shoot", shooter.x, shooter.y, shooter.z, 1, 1, 5);
}

static void tickMob(Server& s, Entity& e, int idx) {
    const MobInfo* mi = mobInfo(e.type);
    float speed = mi ? mi->speed : 0.1f;
    if (e.health <= 0) return;   // the body disappears with ET_CORPSE
    if (e.attackCooldown > 0) e.attackCooldown--;
    if (e.invuln > 0) e.invuln--;
    // despawn: hostile mobs as in vanilla (at once beyond 128 blocks of every player, at
    // random beyond 32); passive ones beyond 96 blocks (entities are not saved)
    double nearest = 1e18;
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& p = s.players[i];
        if (!p.inPlay() || p.e.dim != e.dim) continue;
        double dx = p.e.x - e.x, dy = p.e.y - e.y, dz = p.e.z - e.z;
        double d = e.hostile ? dx * dx + dy * dy + dz * dz : dx * dx + dz * dz;
        if (d < nearest) nearest = d;
    }
    bool far = e.hostile ? nearest > 128.0 * 128.0 || (nearest > 32.0 * 32.0 && s_rng.range(800) == 0)
                         : nearest > 96.0 * 96.0;
    if (far || !s.world.isResident(s.curDim, (int)floor(e.x) >> 4, (int)floor(e.z) >> 4) || e.y < -64) {
        s.removeEntity(e);
        return;
    }
    // sunlight
    if (e.burnsInDay && isDay(s) && (s.ticks + idx) % 20 == 0) {
        int top = s.world.heightAt(s.curDim, (int)floor(e.x), (int)floor(e.z));
        if (e.y + 1 >= top && !inFluid(s, e, blk::Water)) {
            if (e.fireTicks <= 0) e.metaDirty = true;
            e.fireTicks = 60;
        }
    }
    if (e.fireTicks > 0) {
        if (--e.fireTicks % 20 == 0) s.damageEntity(e, 1, DC_FIRE, -1);
        if (e.fireTicks == 0) e.metaDirty = true;
        if (inFluid(s, e, blk::Water)) { e.fireTicks = 0; e.metaDirty = true; }
    }
    if (inFluid(s, e, blk::Lava) && (s.ticks % 10) == 0) { s.damageEntity(e, 4, DC_LAVA, -1); e.fireTicks = 100; }

    double mx = 0, mz = 0;
    bool moving = false;
    Player* target = nullptr;
    if (e.hostile && s.cfg.difficulty > 0) {
        target = nearestTarget(s, e, 16);
        if (e.type == ent::Spider && isDay(s) && e.lastAttacker < 0) target = nullptr;
    } else if (!e.hostile && e.lastAttacker >= 0) {   // until ET_CALM
        // panic: run away from the attacker
        Entity* att = s.findEntity(e.lastAttacker);
        if (att) {
            double dx = e.x - att->x, dz = e.z - att->z, d = sqrt(dx * dx + dz * dz) + 0.01;
            mx = dx / d; mz = dz / d;
            moving = true;
            speed *= 1.35f;  // vanilla panic: ~1.25-1.5x walking speed
        }
    }
    e.target = target ? target->e.id : -1;   // ET_WANDER leaves chasing mobs alone
    if (target) {
        double dx = target->e.x - e.x, dz = target->e.z - e.z, dy = target->e.y - e.y;
        double d = sqrt(dx * dx + dz * dz) + 0.001;
        lookAt(e, target->e.x, target->e.z);
        if (e.type == ent::Skeleton) {
            if (d > 10) { mx = dx / d; mz = dz / d; moving = true; }
            else if (d < 5) { mx = -dx / d; mz = -dz / d; moving = true; }
            if (e.attackCooldown == 0 && d < 15) {
                // vanilla AbstractSkeleton#performRangedAttack: aim at a third of the target's height
                shootArrow(s, e, target->e.x, target->e.y + 0.6, target->e.z, 1.6f, 2.0f + s.cfg.difficulty);
                e.attackCooldown = 40;
            }
        } else if (e.type == ent::Creeper) {
            if (d < 3.0 && fabs(dy) < 3) {
                if (e.fuse < 0) {
                    e.fuse = 0;
                    e.metaDirty = true;
                    s.playSound("entity.creeper.primed", e.x, e.y, e.z, 1, 0.5f, 5);
                    s.scheduleEntityTimer(e, ET_FUSE, 30);   // explodes after 1.5 s
                }
            } else if (e.fuse >= 0 && d > 7) {
                e.fuse = -1;
                e.metaDirty = true;
                s.timers.cancel(TimerKey::entity(e.id, ET_FUSE));
            }
            if (e.fuse < 0) {
                mx = dx / d; mz = dz / d; moving = true;
                bool jump = false;
                if (d > 2.0 && s.pathDirection(e, target->e.x, target->e.y, target->e.z, mx, mz, jump) && jump &&
                    e.onGround)
                    e.vy = 0.42;
            }
        } else {
            mx = dx / d; mz = dz / d;
            moving = d > 0.8;
            // around obstacles: follow a path while the target is not right in front
            bool jump = false;
            if (d > 2.0 && s.pathDirection(e, target->e.x, target->e.y, target->e.z, mx, mz, jump) && jump &&
                e.onGround)
                e.vy = 0.42;
            if (d < 1.6 && fabs(dy) < 1.5 && e.attackCooldown == 0) {
                float dmg = mi ? mi->attack : 2;
                if (s.cfg.difficulty == 1) dmg = dmg * 0.6f + 0.4f;
                if (s.cfg.difficulty == 3) dmg *= 1.5f;
                s.damagePlayer(*target, dmg, DC_ATTACK, e.id);
                Entity& te = target->e;
                te.vx = mx * 0.4;
                te.vz = mz * 0.4;
                te.vy = 0.36;
                Packet pk(pkt::s2c::EntityVelocity);
                pk.w.varint(te.id);
                writeVelocity(pk.w, te);
                target->conn.send(pk);
                s.broadcastAnimation(e, 0, nullptr);
                e.attackCooldown = 20;
            }
        }
    } else if (!moving) {
        // wander (ET_WANDER picks the goals)
        if (e.hasGoal) {
            double dx = e.goalX - e.x, dz = e.goalZ - e.z, d = sqrt(dx * dx + dz * dz);
            if (d < 0.7) e.hasGoal = false;
            else { mx = dx / d; mz = dz / d; moving = true; lookAt(e, e.goalX, e.goalZ); }
        }
    }
    // physics
    bool inWater = inFluid(s, e, blk::Water);
    if (moving) {
        e.vx = mx * speed;
        e.vz = mz * speed;
    } else {
        e.vx *= 0.5;
        e.vz *= 0.5;
    }
    e.vy -= 0.08;
    if (inWater) e.vy = e.vy < 0.04 ? e.vy + 0.1 : 0.04;
    e.vy *= 0.98;
    bool blocked = moveEntity(s, e);
    if (blocked && e.onGround && moving) e.vy = 0.42;  // jump over 1-block obstacles
    // fall damage
    if (!e.onGround && e.vy < 0) e.fallDistance -= (float)e.vy;
    else if (e.onGround) {
        if (e.fallDistance > 3 && e.type != ent::Chicken) s.damageEntity(e, ceilf(e.fallDistance - 3), DC_FALL, -1);
        e.fallDistance = 0;
    }
}

// ------------------------------------------------------------------ tick
void Server::tickEntities() {
    for (int k = 0; k < MC_MAX_ENTITIES; k++) {
        Entity& e = entities[k];
        if (e.kind == EK_NONE || e.removed) continue;
        InDim in(*this, e.dim);
        e.age++;
        switch (e.kind) {
            case EK_ITEM: {
                if (e.pickupDelay > 0) e.pickupDelay--;
                bool inWater = inFluid(*this, e, blk::Water);
                if (inFluid(*this, e, blk::Lava) || e.y < -64) {   // age: ET_DESPAWN
                    removeEntity(e);
                    break;
                }
                if (!world.isResident(curDim, (int)floor(e.x) >> 4, (int)floor(e.z) >> 4)) { removeEntity(e); break; }
                e.vy = inWater ? (e.vy < 0.06 ? e.vy + 0.02 : 0.06) : e.vy - 0.04;
                double ox = e.x, oy = e.y, oz = e.z;
                moveEntity(*this, e);
                double fr = e.onGround ? 0.588 : 0.98;
                e.vx *= fr;
                e.vz *= fr;
                e.vy *= 0.98;
                if (e.onGround && fabs(e.vx) + fabs(e.vz) < 0.002) { e.vx = e.vz = 0; }
                if (fabs(e.x - ox) + fabs(e.y - oy) + fabs(e.z - oz) > 0.5) e.velDirty = true;
                if (e.pickupDelay > 0) break;
                // pick up
                for (int i = 0; i < MC_MAX_PLAYERS; i++) {
                    Player& p = players[i];
                    if (!p.inPlay() || p.dead || p.gamemode == GM_SPECTATOR || p.e.dim != e.dim) continue;
                    if (fabs(p.e.x - e.x) > 1.3 || fabs(p.e.z - e.z) > 1.3 || e.y < p.e.y - 0.8 || e.y > p.e.y + 2.3) continue;
                    int before = e.item.count;
                    int left = giveItem(p, e.item);
                    if (left == before) continue;
                    Packet pk(pkt::s2c::Collect);
                    pk.w.varint(e.id);
                    pk.w.varint(p.e.id);
                    pk.w.varint(before - left);
                    broadcastNear(pk, (int)floor(e.x) >> 4, (int)floor(e.z) >> 4);
                    if (left == 0) { removeEntity(e); break; }
                    e.item.count = (uint8_t)left;
                    e.metaDirty = true;
                }
                break;
            }
            case EK_FALLING_BLOCK: {
                e.vy -= 0.04;
                e.vy *= 0.98;
                moveEntity(*this, e);
                if (e.onGround || e.age > 600 || e.y < -64) {
                    int bx = (int)floor(e.x), by = (int)floor(e.y + 0.5), bz = (int)floor(e.z);
                    uint16_t at = blockAt(bx, by, bz);
                    if (e.y >= 0 && (stateIsAir(at) || (blockOf(at).flags & BF_REPLACEABLE)))
                        setBlock(bx, by, bz, e.blockState);
                    else if (e.y >= 0)
                        dropItem(e.x, e.y, e.z, ItemStack::of(BLOCKS[blockIdOf(e.blockState)].item));
                    removeEntity(e);
                }
                break;
            }
            case EK_TNT: {
                e.vy -= .04;
                double vy = e.vy;
                moveEntity(*this, e);
                e.vx *= .98; e.vy *= .98; e.vz *= .98;
                if (e.onGround) { e.vx *= .7; e.vz *= .7; e.vy = -vy * .98 * .5; }
                if (--e.fuse <= 0) {
                    removeEntity(e);
                    explode(e.x, e.y + e.height / 16.0, e.z, 4, e.owner);
                }
                break;
            }
            case EK_ARROW: {
                if (e.onGround) {  // stuck in a block
                    if (e.age > 600) removeEntity(e);
                    break;
                }
                // step along the velocity checking entities and blocks
                double sp = sqrt(e.vx * e.vx + e.vy * e.vy + e.vz * e.vz);
                int steps = (int)(sp / 0.25) + 1;
                bool done = false;
                for (int st = 0; st < steps && !done; st++) {
                    double nx = e.x + e.vx / steps, ny = e.y + e.vy / steps, nz = e.z + e.vz / steps;
                    if (solidAt(*this, (int)floor(nx), (int)floor(ny), (int)floor(nz))) {
                        int bx = (int)floor(nx), by = (int)floor(ny), bz = (int)floor(nz);
                        if (blockIdOf(blockAt(bx, by, bz)) == blk::Target) {
                            double start[] = {e.x,e.y,e.z}, end[] = {nx,ny,nz};
                            int cell[] = {bx,by,bz}, face = 0; double entry = -1;
                            for (int a = 0; a < 3; ++a) {
                                double delta = end[a] - start[a]; if (fabs(delta) < 1e-12) continue;
                                double hit = ((delta > 0 ? cell[a] : cell[a] + 1) - start[a]) / delta;
                                if (hit > entry) { entry = hit; face = (a == 0 ? 4 : a == 1 ? 0 : 2) + (delta < 0); }
                            }
                            entry = std::max(0.0, std::min(1.0, entry));
                            redstone.targetHit(*this,bx,by,bz,face,e.x+(nx-e.x)*entry,e.y+(ny-e.y)*entry,e.z+(nz-e.z)*entry,true);
                        }
                        e.vx = e.vy = e.vz = 0;
                        e.onGround = true;
                        e.velDirty = true;
                        done = true;
                        break;
                    }
                    e.x = nx; e.y = ny; e.z = nz;
                    // entity hit
                    Entity* hit = nullptr;
                    for (int i = 0; i < MC_MAX_PLAYERS && !hit; i++) {
                        Player& p = players[i];
                        if (!p.inPlay() || p.dead || p.e.id == e.owner || p.gamemode == GM_SPECTATOR || p.e.dim != e.dim) continue;
                        if (fabs(p.e.x - e.x) < 0.4 && fabs(p.e.z - e.z) < 0.4 && e.y > p.e.y && e.y < p.e.y + 1.8) hit = &p.e;
                    }
                    for (int i = 0; i < MC_MAX_ENTITIES && !hit; i++) {
                        Entity& m = entities[i];
                        if (m.kind != EK_MOB || m.removed || m.health <= 0 || m.id == e.owner || m.dim != e.dim) continue;
                        if (fabs(m.x - e.x) < m.width / 2 + 0.1 && fabs(m.z - e.z) < m.width / 2 + 0.1 && e.y > m.y && e.y < m.y + m.height) hit = &m;
                    }
                    if (hit) {
                        float dmg = (float)ceil(sp * e.damage);
                        if (hit->kind == EK_PLAYER) damagePlayer(players[hit->playerSlot], dmg, DC_ARROW, e.owner);
                        else damageEntity(*hit, dmg, DC_ARROW, e.owner);
                        removeEntity(e);
                        done = true;
                    }
                }
                if (!done) {
                    e.vx *= 0.99;
                    e.vy = e.vy * 0.99 - 0.05;
                    e.vz *= 0.99;
                }
                if (e.age > 1200 || e.y < -64) removeEntity(e);
                break;
            }
            case EK_MOB: tickMob(*this, e, k); break;
            default: break;
        }
        if (!e.removed) redstone.entityInside(*this, e);
    }
}

// ------------------------------------------------------------------ combat
void Server::damageEntity(Entity& e, float amount, uint8_t cause, int32_t attackerId) {
    if (e.kind == EK_PLAYER) {
        damagePlayer(players[e.playerSlot], amount, cause, attackerId);
        return;
    }
    if (e.kind != EK_MOB || e.health <= 0 || e.removed) return;
    if (e.invuln > 0 && cause == DC_ATTACK) return;
    e.health -= amount;
    e.invuln = 10;
    if (attackerId >= 0) {
        e.lastAttacker = attackerId;
        if (!e.hostile) {
            // passive mobs flee for 3 s after the last hit
            timers.cancel(TimerKey::entity(e.id, ET_CALM));
            scheduleEntityTimer(e, ET_CALM, 60);
        }
    }
    broadcastStatus(e, 2);
    if (e.health <= 0) {
        e.health = 0;
        e.deathTicks = 0;
        timers.cancel(TimerKey::entity(e.id, ET_FUSE));
        scheduleEntityTimer(e, ET_CORPSE, 20);   // the death animation, then it disappears
        broadcastStatus(e, 3);
        dropMobLoot(*this, e);
        Player* killer = playerByEntity(attackerId);
        if (killer) giveXp(*killer, e.hostile ? 5 : 1 + s_rng.range(3));
    }
}

void Server::attack(Player& p, Entity& target) {
    if (p.dead || p.gamemode == GM_SPECTATOR) return;
    double dx = target.x - p.e.x, dz = target.z - p.e.z, dy = target.y - p.e.y;
    if (dx * dx + dy * dy + dz * dz > 36) return;
    ItemStack& held = p.heldItem();
    float dmg = held.empty() ? 1.0f : (float)ITEMS[held.id].attack;
    // attack cooldown (1.9+ combat): scale by charge
    uint32_t since = ticks - p.lastAttackTick;
    float cooldown = 12.5f;  // ticks for 1.6 attacks/s
    if (!held.empty()) {
        uint8_t k = ITEMS[held.id].kind;
        if (k == IK_SWORD) cooldown = 12.5f;
        else if (k == IK_AXE) cooldown = 20.0f;
        else if (k == IK_PICKAXE) cooldown = 16.7f;
        else if (k == IK_SHOVEL) cooldown = 20.0f;
    }
    float charge = (since + 0.5f) / cooldown;
    if (charge > 1) charge = 1;
    dmg *= 0.2f + charge * charge * 0.8f;
    p.lastAttackTick = ticks;
    bool crit = charge > 0.9f && p.e.fallDistance > 0 && !p.e.onGround;
    if (crit) dmg *= 1.5f;
    if (target.kind == EK_PLAYER) {
        Player& tp = players[target.playerSlot];
        if (!cfg.pvp || tp.dead || !tp.isSurvivalLike()) return;
        damagePlayer(tp, dmg, DC_ATTACK, p.e.id);
    } else {
        damageEntity(target, dmg, DC_ATTACK, p.e.id);
    }
    // knockback
    double yaw = p.e.yaw * M_PI / 180.0;
    double kb = (p.e.flags & EF_SPRINTING) ? 0.8 : 0.4;
    target.vx = -sin(yaw) * kb;
    target.vz = cos(yaw) * kb;
    target.vy = 0.36;
    target.velDirty = true;
    if (target.kind == EK_PLAYER) {
        Packet pk(pkt::s2c::EntityVelocity);
        pk.w.varint(target.id);
        writeVelocity(pk.w, target);
        players[target.playerSlot].conn.send(pk);
    }
    if (crit) broadcastAnimation(target, 4, nullptr);
    if (p.isSurvivalLike() && !held.empty()) {
        uint8_t k = ITEMS[held.id].kind;
        damageHeldItem(p, k == IK_SWORD ? 1 : (k >= IK_PICKAXE && k <= IK_HOE ? 2 : 0));
    }
    addExhaustion(p, 0.1f);
}

void Player::onUseEntity(Reader& r) {
    int32_t target = r.varint();
    int type = r.varint();
    if (type == 2) { r.f32(); r.f32(); r.f32(); }
    if (type == 0 || type == 2) r.varint();
    r.boolean();
    if (!r.ok()) return;
    Entity* t = srv->findEntity(target);
    if (!t || t == &e) return;
    if (type == 1) {
        srv->attack(*this, *t);
        return;
    }
    if (type != 0 || t->kind != EK_MOB) return;
    // interactions
    ItemStack& h = heldItem();
    if (t->type == ent::Sheep && h.id == itm::Shears && !(t->variant & 0x10)) {
        t->variant |= 0x10;
        t->metaDirty = true;
        srv->dropItem(t->x, t->y + 1, t->z, ItemStack::of((uint16_t)(itm::WhiteWool + (t->variant & 15)), 1 + (int)(plat::random32() % 3)));
        srv->playSound("entity.sheep.shear", t->x, t->y, t->z, 1, 1, 5);
        srv->damageHeldItem(*this, 1);
    } else if (t->type == ent::Cow && h.id == itm::Bucket) {
        srv->consumeHeld(*this);
        srv->giveItem(*this, ItemStack::of(itm::MilkBucket));
        srv->playSound("entity.cow.milk", t->x, t->y, t->z, 1, 1, 5);
    }
}

// ------------------------------------------------------------------ explosions
Entity* Server::primeTnt(int x, int y, int z, int32_t owner, bool chain) {
    Entity* e = spawnEntity(EK_TNT,ent::Tnt,x+.5,y,z+.5);
    if (!e) return nullptr;
    double angle = s_rng.unit() * M_PI * 2;
    e->vx = -sin(angle) * .02; e->vy = .2; e->vz = -cos(angle) * .02;
    e->fuse = chain ? 10 + s_rng.range(20) : 80;
    e->owner = owner;
    setBlock(x,y,z,bs::Air);
    if (!chain) playSound("entity.tnt.primed",x+.5,y+.5,z+.5,1,1,4);
    return e;
}

void Server::explode(double x, double y, double z, float power, int32_t source) {
    int r = (int)ceilf(power);
    int8_t offs[512][3];
    int n = 0;
    for (int dx = -r; dx <= r; dx++)
        for (int dy = -r; dy <= r; dy++)
            for (int dz = -r; dz <= r; dz++) {
                double d = sqrt((double)(dx * dx + dy * dy + dz * dz));
                if (d > power * (0.7 + s_rng.unit() * 0.6)) continue;
                int bx = (int)floor(x) + dx, by = (int)floor(y) + dy, bz = (int)floor(z) + dz;
                if (by < 0 || by > 255 || !world.blockInBounds(bx, bz)) continue;
                uint16_t st = blockAt(bx, by, bz);
                uint16_t bid = blockIdOf(st);
                if (stateIsAir(st) || bid == blk::Bedrock || bid == blk::Obsidian || bid == blk::Water || bid == blk::Lava) continue;
                if (n < 512) { offs[n][0] = (int8_t)dx; offs[n][1] = (int8_t)dy; offs[n][2] = (int8_t)dz; n++; }
            }
    // client side effect (particles, sound, block removal prediction)
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& p = players[i];
        if (!p.inPlay() || !p.hasChunk(curDim, (int)floor(x) >> 4, (int)floor(z) >> 4)) continue;
        Packet pk(pkt::s2c::Explosion);
        pk.w.f32((float)x);
        pk.w.f32((float)y);
        pk.w.f32((float)z);
        pk.w.f32(power);
        pk.w.i32(n);
        for (int k = 0; k < n; k++) { pk.w.i8(offs[k][0]); pk.w.i8(offs[k][1]); pk.w.i8(offs[k][2]); }
        pk.w.f32(0);
        pk.w.f32(0);
        pk.w.f32(0);
        p.conn.send(pk);
    }
    for (int k = 0; k < n; k++) {
        int bx = (int)floor(x) + offs[k][0], by = (int)floor(y) + offs[k][1], bz = (int)floor(z) + offs[k][2];
        if (blockIdOf(blockAt(bx,by,bz)) == blk::Tnt) primeTnt(bx,by,bz,source,true);
        else breakBlock(bx, by, bz, nullptr, s_rng.range(3) == 0);
    }
    // damage entities
    double range = power * 2;
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& p = players[i];
        if (!p.inPlay() || p.dead || p.e.dim != curDim) continue;
        double dx = p.e.x - x, dy = p.e.y + 0.9 - y, dz = p.e.z - z, d = sqrt(dx * dx + dy * dy + dz * dz);
        if (d >= range) continue;
        double impact = 1 - d / range;
        float dmg = (float)((impact * impact + impact) / 2 * 7 * range + 1);
        damagePlayer(p, dmg, DC_EXPLOSION, source);
        p.e.vx = dx / (d + 0.01) * impact;
        p.e.vy = dy / (d + 0.01) * impact + 0.2;
        p.e.vz = dz / (d + 0.01) * impact;
        Packet pk(pkt::s2c::EntityVelocity);
        pk.w.varint(p.e.id);
        writeVelocity(pk.w, p.e);
        p.conn.send(pk);
    }
    for (int i = 0; i < MC_MAX_ENTITIES; i++) {
        Entity& m = entities[i];
        if (m.kind != EK_MOB || m.removed || m.id == source || m.dim != curDim) continue;
        double dx = m.x - x, dy = m.y - y, dz = m.z - z, d = sqrt(dx * dx + dy * dy + dz * dz);
        if (d >= range) continue;
        double impact = 1 - d / range;
        damageEntity(m, (float)((impact * impact + impact) / 2 * 7 * range + 1), DC_EXPLOSION, source);
    }
}

}  // namespace mc
