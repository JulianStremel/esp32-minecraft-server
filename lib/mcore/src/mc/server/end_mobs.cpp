// The End's mobs, after vanilla 1.21: endermen (neutral until stared at or hit; they
// teleport away from projectiles, water and daylight, and after a target that runs),
// shulkers (shut in their shells until a player comes near, then they open and shoot
// homing bullets that make the player float up), endermites (from thrown ender pearls),
// and the pearls themselves.
#include <math.h>
#include "mc/registry.h"
#include "mc/server/mob_util.h"
#include "mc/server/server.h"

namespace mc {

using namespace mobs;

static constexpr int LEVITATION = 24;   // the mob effect's registry id

// ------------------------------------------------------------------ endermen
// EnderMan#teleport(x, y, z): down to the ground there, a free box without fluid.
bool Server::endermanTeleportTo(Entity& e, double x, double y, double z) {
    int bx = (int)floor(x), by = (int)floor(y), bz = (int)floor(z);
    if (!world.isResident(curDim, bx >> 4, bz >> 4)) return false;
    if (by > dimMaxY(curDim) - 3) by = dimMaxY(curDim) - 3;
    while (by > dimMinY(curDim) && !stateCollides(blockAt(bx, by - 1, bz))) by--;
    if (by <= dimMinY(curDim)) return false;
    if (stateIsFluid(blockAt(bx, by - 1, bz))) return false;
    for (int dy = 0; dy < 3; dy++) {
        uint16_t st = blockAt(bx, by + dy, bz);
        if (stateCollides(st) || stateIsFluid(st)) return false;
    }
    playSound("entity.enderman.teleport", e.x, e.y, e.z, 1, 1, 5);
    e.x = bx + 0.5;
    e.y = by;
    e.z = bz + 0.5;
    e.vx = e.vy = e.vz = 0;
    e.fallDistance = 0;
    e.hasGoal = false;
    e.pathLen = 0;
    e.sinceTeleport = 400;   // a teleport packet, not a move
    playSound("entity.enderman.teleport", e.x, e.y, e.z, 1, 1, 5);
    return true;
}

// EnderMan#teleport(): somewhere within 32 blocks
bool Server::endermanTeleport(Entity& e) {
    Rng& r = rng();
    double x = e.x + (r.unit() - 0.5) * 64, y = e.y + (r.range(64) - 32), z = e.z + (r.unit() - 0.5) * 64;
    return endermanTeleportTo(e, x, y, z);
}

// EnderMan#teleportTowards: 16 blocks along the way to the target, give or take
static bool teleportTowards(Server& s, Entity& e, const Entity& t) {
    double dx = e.x - t.x, dy = e.y + 1.45 - (t.y + 1.62), dz = e.z - t.z;
    double len = sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1e-6) return false;
    dx /= len; dy /= len; dz /= len;
    Rng& r = rng();
    return s.endermanTeleportTo(e, e.x + (r.unit() - 0.5) * 8 - dx * 16, e.y + (r.range(16) - 8) - dy * 16,
                                e.z + (r.unit() - 0.5) * 8 - dz * 16);
}

// EnderMan#isLookingAtMe: the player's view within a narrow cone (narrower farther away)
// of the enderman's eyes, nothing in between, no carved pumpkin on the player's head.
static bool staredAt(Server& s, const Entity& e, const Player& p) {
    if (p.inv[SLOT_ARMOR_START].id == itm::CarvedPumpkin) return false;
    double yaw = p.e.yaw * M_PI / 180.0, pitch = p.e.pitch * M_PI / 180.0;
    double lx = -sin(yaw) * cos(pitch), ly = -sin(pitch), lz = cos(yaw) * cos(pitch);
    double ex = e.x - p.e.x, ey = e.y + 2.55 - (p.e.y + 1.62), ez = e.z - p.e.z;
    double d = sqrt(ex * ex + ey * ey + ez * ez);
    if (d < 1e-6 || d > 64) return false;
    double dot = (lx * ex + ly * ey + lz * ez) / d;
    return dot > 1.0 - 0.025 / d && lineOfSight(s, p.e.x, p.e.y + 1.62, p.e.z, e.x, e.y + 2.55, e.z);
}

void Server::angerEnderman(Entity& e, int32_t playerId) {
    if (e.angryAt != playerId) {
        playSound("entity.enderman.stare", e.x, e.y, e.z, 2.5f, 1, 5);
        e.metaDirty = true;
    }
    e.angryAt = playerId;
    e.angerTicks = (int16_t)(400 + rng().range(380));
}

// Before the common chase (tickMob): who stares at it, the teleports, water and daylight.
// The chase itself is the neutral mobs' (an angry enderman runs at its target).
void Server::tickEnderman(Entity& e) {
    Rng& r = rng();
    bool wasAngry = e.angryAt >= 0;
    // EndermanLookForPlayerGoal: a player looking it in the eyes makes it angry
    if (e.angryAt < 0 && cfg.difficulty > 0 && (ticks + e.id) % 5 == 0) {
        for (int i = 0; i < MC_MAX_PLAYERS; i++) {
            Player& p = players[i];
            if (!p.inPlay() || p.dead || !p.isSurvivalLike() || p.e.dim != e.dim) continue;
            if (staredAt(*this, e, p)) {
                angerEnderman(e, p.e.id);
                break;
            }
        }
    }
    Player* t = e.angryAt >= 0 ? playerByEntity(e.angryAt) : nullptr;
    if (t && (!t->inPlay() || t->dead || t->e.dim != e.dim)) t = nullptr;
    if (t) {
        double dx = t->e.x - e.x, dy = t->e.y - e.y, dz = t->e.z - e.z, d2 = dx * dx + dy * dy + dz * dz;
        if (staredAt(*this, e, *t)) {   // stared at up close: away (vanilla teleports within 4 blocks)
            if (d2 < 16.0) endermanTeleport(e);
            e.charge = 0;
        } else if (d2 > 256.0 && ++e.charge >= 30) {   // the target ran: after it
            if (teleportTowards(*this, e, t->e)) e.charge = 0;
        }
    }
    // water and rain hurt it (1 a hit, LivingEntity#baseTick), and it teleports away
    int bx = (int)floor(e.x), bz = (int)floor(e.z);
    bool wet = inFluid(*this, e, blk::Water) ||
               (meta.raining && e.dim == DIM_OVERWORLD && world.heightAt(curDim, bx, bz) <= (int)floor(e.y) + 1);
    if (wet && e.invuln == 0) damageEntity(e, 1, DC_DROWN, -1);   // (damageEntity teleports it)
    // daylight in the open (EnderMan#customServerAiStep): it loses its target and goes
    if (e.dim == DIM_OVERWORLD && isDay(*this) && (ticks + e.id) % 20 == 0 && r.range(20) == 0 &&
        world.heightAt(curDim, bx, bz) <= (int)floor(e.y) + 1) {
        e.angryAt = -1;
        e.angerTicks = 0;
        endermanTeleport(e);
    }
    if ((e.angryAt >= 0) != wasAngry) e.metaDirty = true;   // its open mouth (Creepy)
}

// ------------------------------------------------------------------ shulkers
// Vanilla Shulker: fixed to its block; shut (armour 20, arrows bounce off) until a player
// within 16 blocks makes it open and shoot a bullet every 1 to 5.5 s; it peeks out now
// and then when idle. Hurt below half its health, it teleports now and then.
void Server::tickShulker(Entity& e) {
    Rng& r = rng();
    Player* target = cfg.difficulty > 0 ? nearestTarget(*this, e, 16) : nullptr;
    if (target && !lineOfSight(*this, e.x, e.y + 0.5, e.z, target->e.x, target->e.y + 1.0, target->e.z)) target = nullptr;
    e.target = target ? target->e.id : -1;
    uint8_t peek = e.size;   // its shell's opening (0 to 100)
    if (target) {
        lookAt(e, target->e.x, target->e.z);
        peek = 20;   // ShulkerAttackGoal#start
        if (--e.attackCooldown <= 0) {
            e.attackCooldown = (int16_t)(20 + r.range(10) * 20 / 2);
            shootShulkerBullet(e, *target);
        }
    } else {
        e.attackCooldown = 20;
        // ShulkerPeekGoal: open a little for 1 to 4 s now and then
        if (e.size == 0 && r.range(40) == 0) {
            peek = 30;
            e.aiTimer = (int16_t)(20 * (1 + r.range(3)));
        } else if (e.size > 0 && e.aiTimer-- <= 0) {
            peek = 0;
        }
    }
    if (peek != e.size) {
        if (peek == 0) playSound("entity.shulker.close", e.x, e.y, e.z, 1, 1, 5);
        else if (e.size == 0) playSound("entity.shulker.open", e.x, e.y, e.z, 1, 1, 5);
        e.size = peek;
        e.metaDirty = true;
    }
}

// Shulker#teleportSomewhere: a free spot within 8 blocks on a solid block
void Server::shulkerTeleport(Entity& e) {
    Rng& r = rng();
    for (int i = 0; i < 5; i++) {
        int x = (int)floor(e.x) + r.between(-8, 8), y = (int)floor(e.y) + r.between(-8, 8), z = (int)floor(e.z) + r.between(-8, 8);
        if (!world.isResident(curDim, x >> 4, z >> 4) || y <= dimMinY(curDim) || y >= dimMaxY(curDim)) continue;
        uint16_t at = blockAt(x, y, z), below = blockAt(x, y - 1, z);
        if (stateCollides(at) || stateIsFluid(at) || !stateCollides(below)) continue;
        playSound("entity.shulker.teleport", e.x, e.y, e.z, 1, 1, 5);
        e.x = x + 0.5;
        e.y = y;
        e.z = z + 0.5;
        e.sinceTeleport = 400;
        playSound("entity.shulker.teleport", e.x, e.y, e.z, 1, 1, 5);
        return;
    }
}

// What reaches a shulker of a hit (Shulker#hurt and its armour): nothing of an arrow
// while it is shut; against other hits its shell is armour 20.
float mobs::shulkerDamage(const Entity& e, float amount, uint8_t cause) {
    if (e.size > 0) return amount;   // open
    if (cause == DC_ARROW) return 0;
    float cut = 20.0f - amount / 2.0f;   // CombatRules#getDamageAfterAbsorb, armour 20, no toughness
    if (cut < 4.0f) cut = 4.0f;
    return amount * (1.0f - cut / 25.0f);
}

void Server::shootShulkerBullet(Entity& shooter, Player& target) {
    Entity* b = spawnEntity(EK_BULLET, ent::ShulkerBullet, shooter.x, shooter.y + 0.5, shooter.z);
    if (!b) return;
    b->owner = shooter.id;
    b->target = target.e.id;
    b->dim = shooter.dim;
    b->width = b->height = 0.3125f;
    playSound("entity.shulker.shoot", shooter.x, shooter.y, shooter.z, 2, 1, 5);
}

// ShulkerBullet: homes in on its target (steering by 0.2 of the difference a tick at
// 0.15 blocks a tick, as vanilla's axis steps average out), 4 damage and 10 s of
// levitation; it breaks on blocks, after 15 s, or when hit.
void Server::tickShulkerBullet(Entity& b) {
    Player* t = b.target >= 0 ? playerByEntity(b.target) : nullptr;
    if (t && (!t->inPlay() || t->dead || t->e.dim != b.dim)) t = nullptr;
    if (b.age > 300 || !t || !world.isResident(curDim, (int)floor(b.x) >> 4, (int)floor(b.z) >> 4)) {
        removeEntity(b);
        return;
    }
    double dx = t->e.x - b.x, dy = t->e.y + t->e.height * 0.5 - b.y, dz = t->e.z - b.z;
    double d = sqrt(dx * dx + dy * dy + dz * dz);
    if (d > 1e-6) {
        b.vx += (dx / d * 0.15 - b.vx) * 0.2;
        b.vy += (dy / d * 0.15 - b.vy) * 0.2;
        b.vz += (dz / d * 0.15 - b.vz) * 0.2;
    }
    double nx = b.x + b.vx, ny = b.y + b.vy, nz = b.z + b.vz;
    if (stateCollides(blockAt((int)floor(nx), (int)floor(ny), (int)floor(nz)))) {
        playSound("entity.shulker_bullet.hit", b.x, b.y, b.z, 1, 1, 5);
        removeEntity(b);
        return;
    }
    b.x = nx; b.y = ny; b.z = nz;
    double hw = t->e.width / 2 + 0.16;
    if (fabs(t->e.x - b.x) < hw && fabs(t->e.z - b.z) < hw && b.y > t->e.y - 0.16 && b.y < t->e.y + t->e.height + 0.16) {
        if (t->isSurvivalLike()) {
            damagePlayer(*t, 4, DC_ATTACK, b.owner);
            if (!t->dead) levitate(*t, 200);
        }
        playSound("entity.shulker_bullet.hit", b.x, b.y, b.z, 1, 1, 5);
        removeEntity(b);
    }
}

// The levitation effect: the client floats the player up while it lasts (and falls
// after, with the fall damage that brings).
void Server::levitate(Player& p, int ticks) {
    p.levitationTicks = (int16_t)ticks;
    Packet pk(pkt::s2c::EntityEffect);
    pk.w.varint(p.e.id);
    pk.w.varint(LEVITATION);
    pk.w.varint(0);       // amplifier: level I
    pk.w.varint(ticks);
    pk.w.u8(0x06);        // particles and the icon
    p.conn.send(pk);
}

void Server::tickEffects(Player& p) {
    if (p.pearlCooldown > 0) p.pearlCooldown--;
    if (p.levitationTicks <= 0) return;
    p.e.fallDistance = 0;   // vanilla: rising, no fall builds up
    if (--p.levitationTicks > 0 && !p.dead) return;
    p.levitationTicks = 0;
    Packet pk(pkt::s2c::RemoveEntityEffect);
    pk.w.varint(p.e.id);
    pk.w.varint(LEVITATION);
    p.conn.send(pk);
}

// ------------------------------------------------------------------ ender pearls
// EnderpearlItem#use: thrown at 1.5 blocks a tick where the player looks (1 s cooldown);
// ThrownEnderpearl#onHit: the thrower lands there with 5 fall damage, and 1 time in 20
// an endermite appears.
bool Server::throwEnderPearl(Player& p) {
    if (p.heldItem().id != itm::EnderPearl || p.pearlCooldown > 0) return false;
    InDim in(*this, p.e.dim);
    Entity* e = spawnEntity(EK_PEARL, ent::EnderPearl, p.e.x, p.e.y + 1.52, p.e.z);
    if (!e) return false;
    double yaw = p.e.yaw * M_PI / 180.0, pitch = p.e.pitch * M_PI / 180.0;
    e->vx = -sin(yaw) * cos(pitch) * 1.5 + p.e.vx;
    e->vy = -sin(pitch) * 1.5 + (p.e.onGround ? 0 : p.e.vy);
    e->vz = cos(yaw) * cos(pitch) * 1.5 + p.e.vz;
    e->owner = p.e.id;
    e->dim = p.e.dim;
    e->width = e->height = 0.25f;
    e->velDirty = true;
    playSound("entity.ender_pearl.throw", p.e.x, p.e.y, p.e.z, 0.5f, 0.4f, 7);
    p.pearlCooldown = 20;
    {
        Packet pk(pkt::s2c::SetCooldown);
        pk.w.string("minecraft:ender_pearl");
        pk.w.varint(20);
        p.conn.send(pk);
    }
    if (p.isSurvivalLike()) consumeHeld(p);
    return true;
}

void Server::tickEnderPearl(Entity& e) {
    if (e.age > 1200 || e.y < dimVoidY(e.dim) ||
        !world.isResident(curDim, (int)floor(e.x) >> 4, (int)floor(e.z) >> 4)) {
        removeEntity(e);
        return;
    }
    Player* owner = playerByEntity(e.owner);
    double sp = sqrt(e.vx * e.vx + e.vy * e.vy + e.vz * e.vz);
    int steps = (int)(sp / 0.25) + 1;
    bool hit = false;
    double hx = e.x, hy = e.y, hz = e.z;
    for (int st = 1; st <= steps && !hit; st++) {
        double t = (double)st / steps;
        double nx = e.x + e.vx * t, ny = e.y + e.vy * t, nz = e.z + e.vz * t;
        if (stateCollides(blockAt((int)floor(nx), (int)floor(ny), (int)floor(nz)))) {
            hit = true;
            break;   // lands where it was before the block
        }
        hx = nx; hy = ny; hz = nz;
        for (int i = 0; i < MC_MAX_PLAYERS && !hit; i++) {
            Player& p = players[i];
            if (!p.inPlay() || p.dead || p.e.id == e.owner || p.gamemode == GM_SPECTATOR || p.e.dim != e.dim) continue;
            if (fabs(p.e.x - nx) < 0.4 && fabs(p.e.z - nz) < 0.4 && ny > p.e.y && ny < p.e.y + 1.8) hit = true;
        }
        for (int i = 0; i < MC_MAX_ENTITIES && !hit; i++) {
            Entity& m = entities[i];
            if (m.kind != EK_MOB || m.removed || m.health <= 0 || m.dim != e.dim) continue;
            if (fabs(m.x - nx) < m.width / 2 + 0.1 && fabs(m.z - nz) < m.width / 2 + 0.1 && ny > m.y && ny < m.y + m.height) {
                damageEntity(m, 0, DC_ATTACK, e.owner);   // a hit without damage (it still hurts and turns)
                hit = true;
            }
        }
    }
    if (hit) {
        if (owner && owner->inPlay() && !owner->dead && owner->e.dim == e.dim) {
            if (owner->isSurvivalLike() && rng().range(20) == 0 && cfg.spawnMobs) {
                Entity* m = spawnMob(ent::Endermite, owner->e.x, owner->e.y, owner->e.z);
                if (m) m->dim = e.dim;
            }
            owner->teleport(hx, hy, hz, owner->e.yaw, owner->e.pitch);
            playSound("entity.player.teleport", hx, hy, hz, 1, 1, 7);
            if (owner->isSurvivalLike()) damagePlayer(*owner, 5, DC_FALL, -1);
        }
        removeEntity(e);
        return;
    }
    e.x = hx; e.y = hy; e.z = hz;
    double drag = inFluid(*this, e, blk::Water) ? 0.8 : 0.99;
    e.vx *= drag;
    e.vy = e.vy * drag - 0.03;
    e.vz *= drag;
}

}  // namespace mc
