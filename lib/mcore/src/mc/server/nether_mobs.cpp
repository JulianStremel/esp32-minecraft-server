// The Nether's mobs, after vanilla 1.16.5: zombified piglins (neutral, angry as a
// group), ghasts (float around, shoot fireballs a player can hit back) and magma cubes
// (jump at players, split when killed); and the fireballs.
#include <math.h>
#include "mc/registry.h"
#include "mc/server/mob_util.h"
#include "mc/server/server.h"

namespace mc {

using namespace mobs;

// ------------------------------------------------------------------ zombified piglins
// Vanilla ZombifiedPiglin: hitting one angers it and every zombified piglin within its
// follow range (35 blocks, 10 up or down) at the attacker, for 20 to 39 seconds.
void Server::angerPiglins(Entity& victim, int32_t attackerId) {
    for (int i = 0; i < MC_MAX_ENTITIES; i++) {
        Entity& m = entities[i];
        if (m.kind != EK_MOB || m.removed || m.type != ent::ZombifiedPiglin || m.dim != victim.dim) continue;
        if (fabs(m.x - victim.x) > 35 || fabs(m.z - victim.z) > 35 || fabs(m.y - victim.y) > 10) continue;
        if (m.angryAt != attackerId) playSound("entity.zombified_piglin.angry", m.x, m.y, m.z, 2, 1.5f, 5);
        m.angryAt = attackerId;
        m.angerTicks = (int16_t)(400 + rng().range(380));
    }
}

// ------------------------------------------------------------------ magma cubes
void Server::setMagmaCubeSize(Entity& e, uint8_t size) {
    e.size = size;
    e.health = e.maxHealth = (float)(size * size);
    e.width = e.height = 2.04f * 0.255f * size;   // vanilla: 0.255 x the base size per size step
    e.metaDirty = true;
}

// Vanilla Slime#remove: a dead cube bigger than 1 becomes 2 to 4 of half its size.
void Server::splitMagmaCube(Entity& e) {
    if (e.size <= 1) return;
    uint8_t half = (uint8_t)(e.size / 2);
    int n = 2 + rng().range(3);
    float spread = e.size / 4.0f;
    for (int i = 0; i < n; i++) {
        double ox = ((i % 2) - 0.5) * spread, oz = ((i / 2) - 0.5) * spread;
        Entity* c = spawnMob(ent::MagmaCube, e.x + ox, e.y + 0.5, e.z + oz);
        if (!c) break;
        setMagmaCubeSize(*c, half);
        c->dim = e.dim;
    }
}

// Vanilla Slime / MagmaCube: still on the ground between jumps, a jump towards the target
// (or a random direction) every few seconds, bigger cubes higher; contact damage.
void Server::tickMagmaCube(Entity& e) {
    Rng& r = rng();
    Player* target = cfg.difficulty > 0 ? nearestTarget(*this, e, 16) : nullptr;
    if (target && fabs(target->e.y - e.y) > 4) target = nullptr;   // vanilla's targeting condition
    e.target = target ? target->e.id : -1;
    if (target) {
        lookAt(e, target->e.x, target->e.z);
    } else if (e.aiTimer-- <= 0 || !e.hasGoal) {
        e.yaw = e.headYaw = (float)r.range(360);
        e.hasGoal = true;
        e.aiTimer = (int16_t)(40 + r.range(60));
    }
    bool inLava = inFluid(*this, e, blk::Lava);
    if (e.onGround || inLava) {
        if (e.charge-- <= 0) {
            // jump (MagmaCube#jumpFromGround / jumpInLiquid)
            double yaw = e.yaw * M_PI / 180.0;
            double h = 0.10 + 0.04 * e.size;
            e.vx = -sin(yaw) * h;
            e.vz = cos(yaw) * h;
            e.vy = inLava ? 0.22 + e.size * 0.05 : 0.42 + e.size * 0.1;
            e.velDirty = true;
            int delay = (r.range(20) + 10) * 4;   // magma cubes wait 4 times as long as slimes
            e.charge = (int16_t)(target ? delay / 3 : delay);
            playSound(e.size > 1 ? "entity.magma_cube.jump" : "entity.magma_cube.squish_small", e.x, e.y, e.z,
                      0.4f * e.size, 1, 5);
        } else if (e.onGround) {
            e.vx *= 0.5;
            e.vz *= 0.5;
        }
    }
    moveEntity(*this, e);
    e.vy -= 0.08;
    e.vy *= 0.98;
    e.vx *= 0.91;
    e.vz *= 0.91;
    e.fallDistance = 0;   // magma cubes take no fall damage
    // contact damage (Slime#dealDamage): size + 2
    if (target && e.attackCooldown == 0) {
        double dx = target->e.x - e.x, dy = target->e.y - e.y, dz = target->e.z - e.z;
        double reach = 0.6 * e.size + 0.4;
        if (dx * dx + dz * dz < reach * reach && dy > -1.8 && dy < e.height) {
            float dmg = (float)(e.size + 2);
            if (cfg.difficulty == 1) dmg = dmg * 0.6f + 0.4f;
            if (cfg.difficulty == 3) dmg *= 1.5f;
            damagePlayer(*target, dmg, DC_ATTACK, e.id);
            playSound("entity.magma_cube.hurt", e.x, e.y, e.z, 1, 1, 5);
            e.attackCooldown = 10;
        }
    }
}

// ------------------------------------------------------------------ ghasts
// Vanilla Ghast: floats to random places within 16 blocks (pushed 0.1 towards it every
// few ticks, 0.91 drag), targets a player within 64 blocks and 4 up or down, and, while
// it sees the target, charges for 20 ticks and shoots a fireball (40 ticks apart).
void Server::tickGhast(Entity& e) {
    Rng& r = rng();
    Player* target = nullptr;
    if (e.target >= 0) {   // keeps its target while it is within 100 blocks
        Player* p = playerByEntity(e.target);
        if (p && p->inPlay() && !p->dead && p->isSurvivalLike() && p->e.dim == e.dim) {
            double dx = p->e.x - e.x, dy = p->e.y - e.y, dz = p->e.z - e.z;
            if (dx * dx + dy * dy + dz * dz < 100.0 * 100.0) target = p;
        }
    }
    if (!target && cfg.difficulty > 0 && (ticks + e.id) % 10 == 0) {
        Player* p = nearestTarget(*this, e, 64);
        if (p && fabs(p->e.y - e.y) <= 4) target = p;
    }
    e.target = target ? target->e.id : -1;
    // float around (RandomFloatAroundGoal + GhastMoveControl)
    double gx = e.goalX - e.x, gy = e.goalY - e.y, gz = e.goalZ - e.z;
    double gd = gx * gx + gy * gy + gz * gz;
    if (!e.hasGoal || gd < 1.0 || gd > 3600.0) {
        e.goalX = (float)(e.x + (r.unit() * 2 - 1) * 16);
        e.goalY = (float)(e.y + (r.unit() * 2 - 1) * 16);
        e.goalZ = (float)(e.z + (r.unit() * 2 - 1) * 16);
        if (e.dim == DIM_NETHER && e.goalY > 118) e.goalY = 118;
        if (e.goalY < 2) e.goalY = 2;
        e.hasGoal = true;
    }
    if (e.aiTimer-- <= 0) {
        e.aiTimer = (int16_t)(r.range(5) + 2);
        double len = sqrt(gd);
        if (len > 0.01) {
            double nx = gx / len, ny = gy / len, nz = gz / len;
            // the way there must be free for its 4 x 4 x 4 body (vanilla checks all of it;
            // the next 8 blocks are enough at its speed, and 8 times cheaper on the ESP32)
            bool free = true;
            int ahead = (int)ceil(len) < 8 ? (int)ceil(len) : 8;
            for (int i = 1; i < ahead && free; i++)
                free = !boxCollides(*this, e.x + nx * i, e.y + ny * i, e.z + nz * i, e.width, e.height);
            if (free) {
                e.vx += nx * 0.1;
                e.vy += ny * 0.1;
                e.vz += nz * 0.1;
            } else {
                e.hasGoal = false;
            }
        }
    }
    moveEntity(*this, e);
    e.vx *= 0.91;
    e.vy *= 0.91;
    e.vz *= 0.91;
    // look and shoot (GhastLookGoal, GhastShootFireballGoal)
    bool wasCharging = e.charge > 10;
    if (target) {
        double dx = target->e.x - e.x, dz = target->e.z - e.z;
        double d2 = dx * dx + (target->e.y - e.y) * (target->e.y - e.y) + dz * dz;
        lookAt(e, target->e.x, target->e.z);
        if (d2 < 64.0 * 64.0 &&
            lineOfSight(*this, e.x, e.y + 2.6, e.z, target->e.x, target->e.y + 1.62, target->e.z)) {
            e.charge++;
            if (e.charge == 10) playSound("entity.ghast.warn", e.x, e.y, e.z, 10, 1, 5);
            if (e.charge == 20) {
                double yaw = e.yaw * M_PI / 180.0;
                double vx = -sin(yaw), vz = cos(yaw);
                double sx = e.x + vx * 4, sy = e.y + e.height * 0.5 + 0.5, sz = e.z + vz * 4;
                shootFireball(e, sx, sy, sz, target->e.x - sx, target->e.y + 0.9 - sy, target->e.z - sz);
                playSound("entity.ghast.shoot", e.x, e.y, e.z, 10, 1, 5);
                e.charge = -40;
            }
        } else if (e.charge > 0) {
            e.charge--;
        }
    } else {
        if (e.charge > 0) e.charge--;
        if (fabs(e.vx) + fabs(e.vz) > 0.001) {
            e.yaw = e.headYaw = (float)(-atan2(e.vx, e.vz) * 180.0 / M_PI);
        }
    }
    if ((e.charge > 10) != wasCharging) e.metaDirty = true;
}

// ------------------------------------------------------------------ fireballs
// A large fireball (vanilla LargeFireball / AbstractHurtingProjectile): accelerates by
// 0.1 per tick in its direction with 0.95 drag; explodes (power 1, with fire) where it
// hits; 6 damage to an entity it hits directly.
Entity* Server::shootFireball(Entity& shooter, double x, double y, double z, double dx, double dy, double dz) {
    Entity* f = spawnEntity(EK_FIREBALL, ent::Fireball, x, y, z);
    if (!f) return nullptr;
    double len = sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1e-6) len = 1;
    f->px = dx / len * 0.1;
    f->py = dy / len * 0.1;
    f->pz = dz / len * 0.1;
    f->owner = shooter.id;
    f->width = f->height = 1.0f;
    f->dim = shooter.dim;
    return f;
}

// AbstractHurtingProjectile#hurt: a hit sends it where the hitter looks, as the hitter's.
void Server::deflectFireball(Entity& f, Player& p) {
    double yaw = p.e.yaw * M_PI / 180.0, pitch = p.e.pitch * M_PI / 180.0;
    double lx = -sin(yaw) * cos(pitch), ly = -sin(pitch), lz = cos(yaw) * cos(pitch);
    f.vx = lx; f.vy = ly; f.vz = lz;
    f.px = lx * 0.1; f.py = ly * 0.1; f.pz = lz * 0.1;
    f.owner = p.e.id;
    f.age = 0;
    f.velDirty = true;
    f.sinceTeleport = 400;   // the client gets its new course at once
}

static bool hitsBox(const Entity& t, double x, double y, double z, double r) {
    double hw = t.width / 2 + r;
    return x > t.x - hw && x < t.x + hw && z > t.z - hw && z < t.z + hw && y > t.y - r && y < t.y + t.height + r;
}

void Server::tickFireball(Entity& f) {
    if (++f.age > 1200 || f.y < dimVoidY(f.dim) || f.y > dimMaxY(f.dim) + 64 ||
        !world.isResident(curDim, (int)floor(f.x) >> 4, (int)floor(f.z) >> 4)) {
        removeEntity(f);
        return;
    }
    // along this tick's path: the first block or entity
    double len = sqrt(f.vx * f.vx + f.vy * f.vy + f.vz * f.vz);
    int steps = (int)(len * 4) + 1;
    Entity* hitEntity = nullptr;
    bool hit = false;
    double hx = f.x, hy = f.y + 0.5, hz = f.z;
    for (int i = 1; i <= steps && !hit; i++) {
        double t = (double)i / steps;
        hx = f.x + f.vx * t; hy = f.y + 0.5 + f.vy * t; hz = f.z + f.vz * t;
        if (stateCollides(world.getBlock(curDim, (int)floor(hx), (int)floor(hy), (int)floor(hz)))) {
            hit = true;
            break;
        }
        bool skipOwner = f.age < 10;   // it starts next to its shooter
        for (int k = 0; k < MC_MAX_PLAYERS && !hitEntity; k++) {
            Player& p = players[k];
            if (!p.inPlay() || p.dead || p.gamemode == GM_SPECTATOR || p.e.dim != f.dim) continue;
            if (skipOwner && p.e.id == f.owner) continue;
            if (hitsBox(p.e, hx, hy, hz, 0.5)) hitEntity = &p.e;
        }
        for (int k = 0; k < MC_MAX_ENTITIES && !hitEntity; k++) {
            Entity& m = entities[k];
            if ((m.kind != EK_MOB && m.kind != EK_CRYSTAL) || m.removed || m.health <= 0 || m.dim != f.dim) continue;
            if ((skipOwner || f.type == ent::DragonFireball) && m.id == f.owner) continue;
            if (hitsBox(m, hx, hy, hz, 0.5)) hitEntity = &m;
        }
        if (hitEntity) hit = true;
    }
    if (hit && f.type == ent::DragonFireball) {   // a cloud of dragon's breath, no explosion
        breathCloud(hx, hy - 0.5, hz, 3.0f, 600, f.owner);
        playSound("entity.dragon_fireball.explode", hx, hy, hz, 1, 1, 5);
        removeEntity(f);
        return;
    }
    if (hit) {
        if (hitEntity) {
            // a ghast hit by its fireball sent back by a player dies (Ghast#hurt)
            bool returned = hitEntity->type == ent::Ghast && playerByEntity(f.owner);
            if (hitEntity->kind == EK_CRYSTAL) hitCrystal(*hitEntity, f.owner);
            else damageEntity(*hitEntity, returned ? 1000.0f : 6.0f, DC_FIREBALL, f.owner);
        }
        explode(hx, hy, hz, 1.0f, f.owner, true);
        removeEntity(f);
        return;
    }
    f.x += f.vx;
    f.y += f.vy;
    f.z += f.vz;
    double inertia = mobs::inFluid(*this, f, blk::Water) ? 0.8 : 0.95;
    f.vx = (f.vx + f.px) * inertia;
    f.vy = (f.vy + f.py) * inertia;
    f.vz = (f.vz + f.pz) * inertia;
}

}  // namespace mc
