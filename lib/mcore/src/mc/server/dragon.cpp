// The End's dragon fight, after vanilla 1.16.5 (EndDragonFight, EnderDragon and its
// phases, EndCrystal, DragonFireball) in a simpler form: the fight starts when a player
// comes near the island, the dragon circles the obsidian spikes, strafes players with
// fireballs, charges them, perches on the exit portal and breathes; end crystals heal it.
// When it dies the exit portal opens and the first kill leaves the dragon egg.
// What the fight needs over restarts (state, the dragon's health, the crystals left,
// the portal's height) is in WorldState; the entities themselves are not saved.
#include <math.h>
#include <string.h>
#include "mc/registry.h"
#include "mc/server/mob_util.h"
#include "mc/server/server.h"
#include "mc/world/generator.h"

namespace mc {

using namespace mobs;

// vanilla EnderDragonPhase ids (the client reads them from metadata 15)
enum : uint8_t {
    PH_HOLDING = 0, PH_STRAFE = 1, PH_LANDING_APPROACH = 2, PH_LANDING = 3, PH_TAKEOFF = 4, PH_SITTING_FLAMING = 5,
    PH_SITTING_SCANNING = 6, PH_SITTING_ATTACKING = 7, PH_CHARGING = 8, PH_DYING = 9, PH_HOVERING = 10
};

static bool sitting(uint8_t ph) { return ph == PH_SITTING_FLAMING || ph == PH_SITTING_SCANNING || ph == PH_SITTING_ATTACKING; }
static double wrapDeg(double d) {
    d = fmod(d, 360.0);
    if (d >= 180) d -= 360;
    if (d < -180) d += 360;
    return d;
}
static double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

static constexpr double FIGHT_RANGE = 192;   // players within this of (0, 0) take part

// ------------------------------------------------------------------ the fight
int Server::crystalsAlive() const {
    int n = 0;
    for (int i = 0; i < 10; i++) n += (wstate.dragon.crystals >> i) & 1;
    return n;
}

Entity* Server::dragon() {
    if (dragonId_ < 0) return nullptr;
    Entity* e = findEntity(dragonId_);
    if (!e || e->type != ent::EnderDragon) {
        dragonId_ = -1;
        return nullptr;
    }
    return e;
}

// Vanilla EndPodiumFeature at (0, y, 0): a bedrock bowl around 5 x 5 portal blocks (air
// until the dragon dies), a bedrock pillar of 4 with torches.
void Server::placeExitPortal(bool active) {
    InDim in(*this, DIM_END);
    int py = wstate.dragon.portalY;
    for (int dx = -4; dx <= 4; dx++)
        for (int dz = -4; dz <= 4; dz++)
            for (int dy = -1; dy <= 32; dy++) {
                double d = sqrt((double)(dx * dx + dz * dz + dy * dy));
                bool inner = sqrt((double)(dx * dx + dz * dz + (dy < 0 ? dy * dy : 0))) <= 2.5 && d <= 2.5 + (dy < 0 ? 0 : 99);
                inner = (dx * dx + dy * dy + dz * dz) <= 6.25;
                bool outer = (dx * dx + dy * dy + dz * dz) <= 12.25;
                if (!inner && !outer) continue;
                int x = dx, y = py + dy, z = dz;
                uint16_t st;
                if (dy < 0) st = inner ? bs::Bedrock : bs::EndStone;
                else if (dy > 0) st = 0;
                else st = inner ? (active ? bs::EndPortal : 0) : bs::Bedrock;
                if (blockAt(x, y, z) != st) world.setBlock(DIM_END, x, y, z, st);
            }
    for (int i = 0; i < 4; i++) world.setBlock(DIM_END, 0, py + i, 0, bs::Bedrock);
    static const char* const FACING[4] = {"north", "south", "west", "east"};
    static const int8_t D[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
    for (int f = 0; f < 4; f++)
        world.setBlock(DIM_END, D[f][0], py + 2, D[f][1], setPropStr(BLOCKS[blk::WallTorch].defState, "facing", FACING[f]));
}

// Once a second: start (or resume after a restart) the fight when a player is near the
// island; keep the boss bar shown to the players taking part.
void Server::tickDragonFight() {
    if (ticks % 20 != 0) return;
    DragonFight& f = wstate.dragon;
    bool near = false;
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& p = players[i];
        if (p.inPlay() && p.e.dim == DIM_END && p.e.x * p.e.x + p.e.z * p.e.z < FIGHT_RANGE * FIGHT_RANGE) near = true;
    }
    Entity* d = dragon();
    if (near && !d && f.state != DragonFight::KILLED && world.isResident(DIM_END, 0, 0)) startDragonFight();
    d = dragon();
    // the boss bar
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& p = players[i];
        bool want = d && p.inPlay() && p.e.dim == DIM_END && d->health > 0 &&
                    (p.e.x - d->x) * (p.e.x - d->x) + (p.e.z - d->z) * (p.e.z - d->z) < FIGHT_RANGE * FIGHT_RANGE;
        if (want != p.bossBar) sendBossBar(p, want ? 0 : 1);
    }
}

void Server::startDragonFight() {
    InDim in(*this, DIM_END);
    DragonFight& f = wstate.dragon;
    if (f.state == DragonFight::NOT_STARTED) {
        // on the island as generated: what was built there (an old portal) does not count
        int y = generatorOf(DIM_END).endSurfaceY(0, 0);
        if (f.portalY && f.portalY != y)   // a portal placed at another height (before this fix): gone
            for (int dx = -4; dx <= 4; dx++)
                for (int dz = -4; dz <= 4; dz++)
                    for (int dy = -1; dy <= 4; dy++)
                        world.setBlock(DIM_END, dx, f.portalY + dy, dz, f.portalY + dy < y ? bs::EndStone : 0);
        f.portalY = (int16_t)(y > 0 ? y : 64);
        f.crystals = 0x3FF;
        f.dragonHealth = 200;
        placeExitPortal(false);
        wstate.dirty = true;
    }
    f.state = DragonFight::DRAGON_ALIVE;
    // the crystals still standing (entities are not saved: they come back on a restart)
    EndSpike spikes[10];
    endSpikes(meta.seed, spikes);
    for (int i = 0; i < 10; i++) {
        if (!((f.crystals >> i) & 1)) continue;
        bool there = false;
        for (const Entity& c : entities)
            if (c.kind == EK_CRYSTAL && !c.removed && c.dim == DIM_END && (int)c.variant == i) there = true;
        if (there) continue;
        Entity* c = spawnEntity(EK_CRYSTAL, ent::EndCrystal, spikes[i].x + 0.5, spikes[i].height + 1, spikes[i].z + 0.5);
        if (!c) break;
        c->variant = (uint8_t)i;
        c->width = c->height = 2.0f;
        c->health = 1;
        world.setBlock(DIM_END, spikes[i].x, spikes[i].height, spikes[i].z, bs::Bedrock);
    }
    Entity* d = spawnMob(ent::EnderDragon, 0.5, 128, 0.5);
    if (!d) return;
    d->health = f.dragonHealth;
    d->maxHealth = 200;
    d->phase = PH_HOLDING;
    d->hasGoal = false;
    reserveEntityIds(8);   // its 8 body parts' ids (the client numbers them from the dragon's)
    dragonId_ = d->id;
    MC_LOGI("dragon fight: the dragon is back (health %.0f, %d crystals)", d->health, crystalsAlive());
}

void Server::sendBossBar(Player& p, int action) {
    static const uint8_t BAR_UUID[16] = {0xD7, 0x4D, 0x1A, 0x6E, 0x50, 0x45, 0x4E, 0x44,
                                         0x52, 0x41, 0x47, 0x4F, 0x4E, 0x42, 0x41, 0x52};
    Entity* d = dragon();
    Packet pk(pkt::s2c::BossBar);
    pk.w.uuid(BAR_UUID);
    pk.w.varint(action);
    if (action == 0) {
        pk.w.string("{\"translate\":\"entity.minecraft.ender_dragon\"}");
        pk.w.f32(d ? d->health / 200.0f : 1.0f);
        pk.w.varint(0);   // pink
        pk.w.varint(0);   // no notches
        pk.w.u8(0x02 | 0x04);   // boss music, world fog
        p.bossBar = true;
    } else if (action == 2) {
        pk.w.f32(d ? (d->health > 0 ? d->health : 0) / 200.0f : 0.0f);
    } else {
        p.bossBar = false;
    }
    p.conn.send(pk);
}

static void updateBossBars(Server& s) {
    for (int i = 0; i < MC_MAX_PLAYERS; i++)
        if (s.players[i].inPlay() && s.players[i].bossBar) s.sendBossBar(s.players[i], 2);
}

// ------------------------------------------------------------------ end crystals
void Server::hitCrystal(Entity& c, int32_t by) {
    if (c.removed) return;
    removeEntity(c);
    wstate.dragon.crystals &= (uint16_t)~(1u << c.variant);
    wstate.dirty = true;
    Entity* d = dragon();
    // the crystal healing the dragon: 10 damage to it (EnderDragon#onCrystalDestroyed)
    if (d && d->health > 0 && d->target == c.id) {
        dragonPart_ = 0;
        damageEntity(*d, 10, DC_EXPLOSION, -1);
        dragonPart_ = -1;
    }
    if (d && d->phase == PH_HOLDING && playerByEntity(by)) {
        d->phase = PH_STRAFE;
        d->angryAt = by;
        d->phaseTicks = 0;
        d->metaDirty = true;
    }
    explode(c.x, c.y, c.z, 6.0f, -1);
}

void Server::tickCrystal(Entity& c) {
    // the fire on its bedrock (EndCrystal#tick)
    if ((ticks + c.id) % 20 == 0) {
        int x = (int)floor(c.x), y = (int)floor(c.y), z = (int)floor(c.z);
        if (stateIsAir(blockAt(x, y, z))) world.setBlock(curDim, x, y, z, bs::Fire);
    }
}

// ------------------------------------------------------------------ dragon breath
// Vanilla DragonFireball: no explosion, a cloud of dragon's breath (radius 3, 30 s) where
// it lands. The cloud (AreaEffectCloud with instant damage II): 6 damage every second.
Entity* Server::breathCloud(double x, double y, double z, float radius, int duration, int32_t owner) {
    Entity* c = spawnEntity(EK_CLOUD, ent::AreaEffectCloud, x, y, z);
    if (!c) return nullptr;
    c->damage = radius;
    c->charge = (int16_t)duration;
    c->owner = owner;
    c->width = radius * 2;
    c->height = 0.5f;
    return c;
}

void Server::tickCloud(Entity& c) {
    if (++c.age > (uint32_t)c.charge) {
        removeEntity(c);
        return;
    }
    if (c.age % 20 != 0) return;
    float r = c.damage;
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& p = players[i];
        if (!p.inPlay() || p.dead || !p.isSurvivalLike() || p.e.dim != c.dim) continue;
        double dx = p.e.x - c.x, dz = p.e.z - c.z;
        if (dx * dx + dz * dz <= r * r && p.e.y >= c.y - 1 && p.e.y <= c.y + 3) damagePlayer(p, 6, DC_MAGIC, c.owner);
    }
}

void Server::writeCloudMetadata(Writer& w, const Entity& c) {
    w.u8(7); w.varint(2); w.f32(c.damage);   // radius
    w.u8(8); w.varint(1); w.varint(0x9B32B3);   // colour (purple)
    w.u8(9); w.varint(7); w.boolean(false);   // not waiting
    w.u8(10); w.varint(15); w.varint(8);   // particle: dragon_breath
}

// ------------------------------------------------------------------ the dragon
// Vanilla's 24 path nodes: 12 on a ring of radius 60, 8 of radius 40, 4 of radius 20;
// above the terrain by 5 (15 for the middle ring), at least at y 10.
static void dragonNode(Server& s, int i, double& x, double& y, double& z) {
    int k = 5;
    double a;
    int r;
    if (i < 12) { a = 2.0 * (-M_PI + M_PI / 6 * i); r = 60; }
    else if (i < 20) { a = 2.0 * (-M_PI + M_PI / 4 * (i - 12)); r = 40; k += 10; }
    else { a = 2.0 * (-M_PI + M_PI / 2 * (i - 20)); r = 20; }
    x = floor(r * cos(a));
    z = floor(r * sin(a));
    int h = s.world.isResident(DIM_END, (int)x >> 4, (int)z >> 4) ? s.world.heightAt(DIM_END, (int)x, (int)z) : 60;
    y = h + k > 10 ? h + k : 10;
}

static int closestNode(Server& s, double px, double py, double pz, bool rings) {
    int best = 0;
    double bd = 1e30;
    for (int i = 0; i < (rings ? 12 : 24); i++) {
        double x, y, z;
        dragonNode(s, i, x, y, z);
        double d = (x - px) * (x - px) + (y - py) * (y - py) + (z - pz) * (z - pz);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

static void setPhase(Entity& d, uint8_t ph) {
    if (d.phase == ph) return;
    d.phase = ph;
    d.phaseTicks = 0;
    d.hasGoal = false;
    d.charge = 0;   // strafe: fireball charge; perched: damage taken
    d.metaDirty = true;
}

// The next node of the holding pattern (vanilla DragonHoldingPatternPhase#findNewTarget):
// around the outer ring while crystals stand, the middle ring without them; turning
// around now and then.
static void nextHoldingTarget(Server& s, Entity& d) {
    Rng& r = rng();
    bool crystals = s.crystalsAlive() > 0;
    int n = closestNode(s, d.x, d.y, d.z, false);
    if (r.range(8) == 0) {
        d.clockwise = !d.clockwise;
        n += 6;
    }
    n += d.clockwise ? 1 : -1;
    if (!crystals) n = 12 + (((n - 12) % 8) + 8) % 8;
    else n = ((n % 12) + 12) % 12;
    double x, y, z;
    dragonNode(s, n, x, y, z);
    d.goalX = (float)x;
    d.goalY = (float)(y + r.unit() * 20);
    d.goalZ = (float)z;
    d.hasGoal = true;
}

static Player* nearestEndPlayer(Server& s, double x, double y, double z, double range, double dyMax = 1e9) {
    Player* best = nullptr;
    double bd = range * range;
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& p = s.players[i];
        if (!p.inPlay() || p.dead || !p.isSurvivalLike() || p.e.dim != DIM_END || fabs(p.e.y - y) > dyMax) continue;
        double d = (p.e.x - x) * (p.e.x - x) + (p.e.y - y) * (p.e.y - y) + (p.e.z - z) * (p.e.z - z);
        if (d < bd) { bd = d; best = &p; }
    }
    return best;
}

// where the dragon's head is (vanilla tickPart for the head, without the vertical part)
static void headPos(const Entity& d, double& x, double& y, double& z) {
    double yaw = d.yaw * M_PI / 180.0;
    x = d.x + sin(yaw) * 6.5;
    y = d.y + (sitting(d.phase) ? -1.0 : 0.5) + 1;
    z = d.z - cos(yaw) * 6.5;
}

void Server::tickDragon(Entity& d) {
    Rng& r = rng();
    DragonFight& f = wstate.dragon;
    int py = f.portalY;
    d.phaseTicks++;
    // dead: rise, spin, then the portal (vanilla EnderDragon#tickDeath)
    if (d.health <= 0) {
        d.deathTicks++;
        d.y += 0.1;
        d.yaw += 20;
        d.vx = d.vy = d.vz = 0;
        if (d.deathTicks == 1) updateBossBars(*this);
        if (d.deathTicks >= 200) finishDragonFight(d);
        return;
    }
    // crystals: the nearest within 32 heals it by 1 every 10 ticks (EnderDragon#checkCrystals)
    if (r.range(10) == 0 || d.target >= 0) {
        Entity* best = nullptr;
        double bd = 32.0 * 32.0;
        for (Entity& c : entities) {
            if (c.kind != EK_CRYSTAL || c.removed || c.dim != d.dim) continue;
            double dd = (c.x - d.x) * (c.x - d.x) + (c.y - d.y) * (c.y - d.y) + (c.z - d.z) * (c.z - d.z);
            if (dd < bd) { bd = dd; best = &c; }
        }
        d.target = best ? best->id : -1;
    }
    if (d.target >= 0 && ticks % 10 == 0 && d.health < d.maxHealth) {
        d.health += 1;
        f.dragonHealth = d.health;
        d.metaDirty = true;
        if (ticks % 40 == 0) updateBossBars(*this);
    }
    // the phase decides where it flies (null: it stays)
    bool fly = true;
    double speed = 0.6;   // AbstractDragonPhaseInstance#getFlySpeed
    switch (d.phase) {
        case PH_HOLDING: {
            double gd = (d.goalX - d.x) * (d.goalX - d.x) + (d.goalY - d.y) * (d.goalY - d.y) + (d.goalZ - d.z) * (d.goalZ - d.z);
            if (!d.hasGoal || gd < 100 || gd > 22500) {
                // at a node: perch (more often with fewer crystals), strafe a player, or fly on
                int n = crystalsAlive();
                if (d.hasGoal && r.range(n + 3) == 0) {
                    setPhase(d, PH_LANDING_APPROACH);
                    break;
                }
                Player* p = nearestEndPlayer(*this, 0, py, 0, 64);
                if (d.hasGoal && p) {
                    double dist = (p->e.x * p->e.x + (p->e.y - py) * (p->e.y - py) + p->e.z * p->e.z) / 512.0;
                    if (r.range((int)fabs(dist) + 2) == 0 || r.range(n + 2) == 0) {
                        setPhase(d, PH_STRAFE);
                        d.angryAt = p->e.id;
                        break;
                    }
                }
                nextHoldingTarget(*this, d);
            }
            break;
        }
        case PH_STRAFE: {
            Player* p = playerByEntity(d.angryAt);
            if (!p || p->dead || p->e.dim != DIM_END || !p->isSurvivalLike() || d.phaseTicks > 400) {
                setPhase(d, PH_HOLDING);
                break;
            }
            double dx = p->e.x - d.x, dz = p->e.z - d.z, hd = sqrt(dx * dx + dz * dz);
            double up = fmin(0.4 + hd / 80.0 - 1.0, 10.0);
            d.goalX = (float)p->e.x; d.goalY = (float)(p->e.y + up); d.goalZ = (float)p->e.z;
            d.hasGoal = true;
            double dist2 = dx * dx + dz * dz + (p->e.y - d.y) * (p->e.y - d.y);
            if (dist2 < 64.0 * 64.0 && lineOfSight(*this, d.x, d.y + 2, d.z, p->e.x, p->e.y + 1.6, p->e.z)) {
                d.charge++;
                // facing it within 10 degrees: a fireball from the head
                double yaw = d.yaw * M_PI / 180.0;
                double fx = sin(yaw), fz = -cos(yaw);
                double dot = (fx * dx + fz * dz) / (hd + 1e-9);
                double ang = acos(clampd(dot, -1, 1)) * 180 / M_PI + 0.5;
                if (d.charge >= 5 && ang < 10) {
                    double hx, hy, hz;
                    headPos(d, hx, hy, hz);
                    Entity* fb = shootFireball(d, hx - fx, hy + 0.5, hz - fz, p->e.x - hx, p->e.y + 0.9 - hy, p->e.z - hz);
                    if (fb) fb->type = ent::DragonFireball;
                    playSound("entity.ender_dragon.shoot", d.x, d.y, d.z, 5, 1, 5);
                    d.charge = 0;
                    setPhase(d, PH_HOLDING);
                }
            } else if (d.charge > 0) {
                d.charge--;
            }
            break;
        }
        case PH_CHARGING:
            speed = 3.0;
            if (!d.hasGoal || d.phaseTicks > 200) { setPhase(d, PH_HOLDING); break; }
            {
                double gd = (d.goalX - d.x) * (d.goalX - d.x) + (d.goalY - d.y) * (d.goalY - d.y) + (d.goalZ - d.z) * (d.goalZ - d.z);
                if (gd < 100) setPhase(d, PH_HOLDING);
            }
            break;
        case PH_LANDING_APPROACH:
            d.goalX = 0.5f; d.goalY = (float)(py + 20); d.goalZ = 0.5f;
            d.hasGoal = true;
            if ((d.x * d.x + d.z * d.z) < 100 && fabs(d.y - (py + 20)) < 10) setPhase(d, PH_LANDING);
            if (d.phaseTicks > 600) setPhase(d, PH_LANDING);
            break;
        case PH_LANDING:
            speed = 1.5;
            d.goalX = 0.5f; d.goalY = (float)(py + 4); d.goalZ = 0.5f;
            d.hasGoal = true;
            if ((d.x - 0.5) * (d.x - 0.5) + (d.y - py - 4) * (d.y - py - 4) + (d.z - 0.5) * (d.z - 0.5) < 1.0 ||
                d.phaseTicks > 300) {
                d.x = 0.5; d.y = py + 4; d.z = 0.5;
                setPhase(d, PH_SITTING_SCANNING);
                d.size = 0;   // breath cycles this perch
            }
            break;
        case PH_SITTING_SCANNING: {
            fly = false;
            Player* p = nearestEndPlayer(*this, d.x, d.y, d.z, 20, 10);
            if (p) {
                if (d.phaseTicks > 25) { setPhase(d, PH_SITTING_ATTACKING); playSound("entity.ender_dragon.growl", d.x, d.y, d.z, 2.5f, 1, 5); }
                else {   // turn towards it
                    double hx, hy, hz;
                    headPos(d, hx, hy, hz);
                    double want = wrapDeg(180.0 - atan2(p->e.x - hx, p->e.z - hz) * 180 / M_PI - d.yaw);
                    d.yaw += (float)clampd(want, -10, 10);
                }
            } else if (d.phaseTicks >= 100) {
                Player* q = nearestEndPlayer(*this, d.x, d.y, d.z, 150);
                setPhase(d, q ? PH_CHARGING : PH_TAKEOFF);
                if (q) { d.goalX = (float)q->e.x; d.goalY = (float)q->e.y; d.goalZ = (float)q->e.z; d.hasGoal = true; }
            }
            break;
        }
        case PH_SITTING_ATTACKING:
            fly = false;
            if (d.phaseTicks >= 40) setPhase(d, PH_SITTING_FLAMING);
            break;
        case PH_SITTING_FLAMING:
            fly = false;
            if (d.phaseTicks == 10) {   // the breath, in front of the head, on the ground
                double hx, hy, hz;
                headPos(d, hx, hy, hz);
                double yaw = d.yaw * M_PI / 180.0;
                double cx = hx + sin(yaw) * 2.5, cz = hz - cos(yaw) * 2.5;
                int y = (int)floor(hy);
                while (y > 0 && !stateCollides(blockAt((int)floor(cx), y - 1, (int)floor(cz)))) y--;
                breathCloud(cx, y, cz, 5.0f, 200, d.id);
            }
            if (d.phaseTicks >= 200) {
                d.size++;
                setPhase(d, d.size >= 4 ? PH_TAKEOFF : PH_SITTING_SCANNING);
            }
            break;
        case PH_TAKEOFF:
            if (!d.hasGoal) {
                double yaw = d.yaw * M_PI / 180.0;
                int n = closestNode(*this, -sin(yaw) * 40, 105, cos(yaw) * 40, crystalsAlive() > 0);
                double x, y, z;
                dragonNode(*this, n, x, y, z);
                d.goalX = (float)x; d.goalY = (float)(y + r.unit() * 20); d.goalZ = (float)z;
                d.hasGoal = true;
            }
            if (d.x * d.x + (d.y - py) * (d.y - py) + d.z * d.z > 100) setPhase(d, PH_HOLDING);
            break;
        case PH_DYING:
            d.goalX = 0.5f; d.goalY = (float)(py + 4); d.goalZ = 0.5f;
            d.hasGoal = true;
            {
                double dd = d.x * d.x + (d.y - py - 4) * (d.y - py - 4) + d.z * d.z;
                if (dd < 100 || d.phaseTicks > 200) {
                    d.health = 0;
                    d.deathTicks = 0;
                    d.metaDirty = true;
                    f.state = DragonFight::KILLED;   // saved now: a restart does not bring it back
                    wstate.dirty = true;
                }
            }
            break;
        default:
            setPhase(d, PH_HOLDING);
            break;
    }
    if (d.phase != PH_HOLDING && d.phase != PH_STRAFE && d.phase != PH_CHARGING && d.phase != PH_LANDING_APPROACH &&
        d.phase != PH_LANDING && d.phase != PH_TAKEOFF && d.phase != PH_DYING)
        fly = false;
    if (!fly || !d.hasGoal) {
        d.vx = d.vy = d.vz = 0;
    } else {
        // vanilla EnderDragon#aiStep: steer towards the target, accelerate along the body
        double dx = d.goalX - d.x, dy = d.goalY - d.y, dz = d.goalZ - d.z;
        double dist = sqrt(dx * dx + dy * dy + dz * dz), hd = sqrt(dx * dx + dz * dz);
        double climb = hd > 0 ? clampd(dy / hd, -speed, speed) : 0;
        d.vy += climb * 0.01;
        d.yaw = (float)wrapDeg(d.yaw);
        double turn = clampd(wrapDeg(180.0 - atan2(dx, dz) * 180.0 / M_PI - d.yaw), -50, 50);
        double yaw = d.yaw * M_PI / 180.0;
        double fx = sin(yaw), fy = d.vy, fz = -cos(yaw);
        double fl = sqrt(fx * fx + fy * fy + fz * fz);
        fx /= fl; fy /= fl; fz /= fl;
        double tx = dx / (dist + 1e-9), ty = dy / (dist + 1e-9), tz = dz / (dist + 1e-9);
        double facing = fmax((fx * tx + fy * ty + fz * tz + 0.5) / 1.5, 0.0);
        double hv = sqrt(d.vx * d.vx + d.vz * d.vz) + 1;
        double turnSpeed = 0.7 / fmin(hv, 40.0) / hv;
        d.pitch = 0;
        d.yawVel = (float)(d.yawVel * 0.8 + turn * turnSpeed);
        d.yaw += d.yawVel * 0.1f;
        double near = 2.0 / (dist + 1.0);
        double acc = 0.06 * (facing * near + (1.0 - near));
        yaw = d.yaw * M_PI / 180.0;
        d.vx += sin(yaw) * acc;
        d.vz += -cos(yaw) * acc;
        d.x += d.vx;
        d.y += d.vy;
        d.z += d.vz;
        double vl = sqrt(d.vx * d.vx + d.vy * d.vy + d.vz * d.vz) + 1e-9;
        double along = 0.8 + 0.15 * ((d.vx / vl) * fx + (d.vy / vl) * fy + (d.vz / vl) * fz + 1.0) / 2.0;
        d.vx *= along;
        d.vy *= 0.91;
        d.vz *= along;
    }
    d.headYaw = d.yaw;
    if (d.y < 1) d.y = 1;
    // its head and neck hurt, its wings throw players aside (EnderDragon#hurt / #knockBack)
    if (d.invuln == 0 && d.phase != PH_DYING) {
        double hx, hy, hz;
        headPos(d, hx, hy, hz);
        double yaw = d.yaw * M_PI / 180.0;
        for (int i = 0; i < MC_MAX_PLAYERS; i++) {
            Player& p = players[i];
            if (!p.inPlay() || p.dead || !p.isSurvivalLike() || p.e.dim != d.dim) continue;
            double ex = p.e.x - hx, ey = p.e.y + 0.9 - hy, ez = p.e.z - hz;
            if (ex * ex + ey * ey + ez * ez < 2.5 * 2.5) {
                damagePlayer(p, 10, DC_ATTACK, d.id);
                continue;
            }
            // the wings: 4.5 blocks to either side, 2 up
            for (int s = -1; s <= 1; s += 2) {
                double wx = d.x + cos(yaw) * 4.5 * s, wz = d.z + sin(yaw) * 4.5 * s;
                double kx = p.e.x - wx, kz = p.e.z - wz, ky = p.e.y - (d.y + 2 - 2);
                if (fabs(kx) < 6 && fabs(kz) < 6 && ky > -2 && ky < 4) {
                    double bx = p.e.x - d.x, bz = p.e.z - d.z, bd = sqrt(bx * bx + bz * bz) + 0.1;
                    p.e.vx = bx / bd * 4 * 0.25;
                    p.e.vy = 0.2 + 0.4;
                    p.e.vz = bz / bd * 4 * 0.25;
                    Packet pk(pkt::s2c::EntityVelocity);
                    pk.w.varint(p.e.id);
                    writeVelocity(pk.w, p.e.vx, p.e.vy, p.e.vz);
                    p.conn.send(pk);
                    if (!sitting(d.phase)) damagePlayer(p, 5, DC_ATTACK, d.id);
                    break;
                }
            }
        }
    }
    if (d.invuln > 0) d.invuln--;
    f.dragonHealth = d.health > 0 ? d.health : 0;
}

// Its damage, by the part that was hit (vanilla EnderDragon#hurt(EnderDragonPart, ...)):
// the head takes it all, the rest a quarter plus 1. Arrows bounce off while it perches.
float Server::dragonDamage(Entity& d, float amount, uint8_t cause) {
    if (d.phase == PH_DYING || d.health <= 0) return 0;
    if (sitting(d.phase) && cause == DC_ARROW) return 0;
    if (dragonPart_ != 0) amount = amount / 4.0f + fminf(amount, 1.0f);
    return amount;
}

void Server::dragonHurt(Entity& d, float before) {
    if (d.health <= 0 && d.phase != PH_DYING) {   // vanilla: alive (1) until it reaches the portal
        d.health = 1;
        setPhase(d, PH_DYING);
        MC_LOGI("dragon fight: the dragon is dying");
    }
    if (sitting(d.phase)) {   // a quarter of its health lost while perching: it takes off
        d.charge = (int16_t)(d.charge + (before - d.health));
        if (d.charge > 50) setPhase(d, PH_TAKEOFF);
    }
    wstate.dragon.dragonHealth = d.health;
    d.metaDirty = true;
    updateBossBars(*this);
}

// The dragon's body parts' ids follow its own (vanilla); on the client they start at the
// dragon's own id, so a hit on part k arrives as dragon + k: the neck's click hits the
// head here, the head's own click is the dragon itself and does nothing (vanilla).
Entity* Server::dragonByPart(int32_t id, int& part) {
    Entity* d = dragon();
    if (!d || id <= d->id || id > d->id + 8) return nullptr;
    part = id - d->id - 1;
    return d;
}

void Server::finishDragonFight(Entity& d) {
    DragonFight& f = wstate.dragon;
    // XP: 12000 for the first kill, 500 after (EnderDragon#tickDeath), to the players nearby
    int xp = f.previouslyKilled ? 500 : 12000;
    int n = 0;
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& p = players[i];
        if (p.inPlay() && !p.dead && p.e.dim == DIM_END &&
            (p.e.x - d.x) * (p.e.x - d.x) + (p.e.z - d.z) * (p.e.z - d.z) < FIGHT_RANGE * FIGHT_RANGE)
            n++;
    }
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& p = players[i];
        if (p.inPlay() && !p.dead && p.e.dim == DIM_END &&
            (p.e.x - d.x) * (p.e.x - d.x) + (p.e.z - d.z) * (p.e.z - d.z) < FIGHT_RANGE * FIGHT_RANGE)
            giveXp(p, xp / (n ? n : 1));
    }
    playSound("entity.ender_dragon.death", d.x, d.y, d.z, 5, 1, 5);
    removeEntity(d);
    dragonId_ = -1;
    for (int i = 0; i < MC_MAX_PLAYERS; i++)
        if (players[i].inPlay() && players[i].bossBar) sendBossBar(players[i], 1);
    placeExitPortal(true);
    if (!f.previouslyKilled) {
        InDim in(*this, DIM_END);
        world.setBlock(DIM_END, 0, f.portalY + 4, 0, BLOCKS[blk::DragonEgg].defState);
        f.eggPlaced = true;
    }
    f.state = DragonFight::KILLED;
    f.previouslyKilled = true;
    f.dragonHealth = 200;
    wstate.dirty = true;
    packWorldState();
    if (storage) saveMetaLater();
    MC_LOGI("dragon fight: the dragon was killed; the exit portal is open");
}

// /dragon respawn: a new fight (crystals back, portal closed), as vanilla's respawn with
// four crystals on the exit portal; /dragon reset: as if never fought (the egg and the
// first kill's XP again).
void Server::resetDragonFight(bool asNew) {
    InDim in(*this, DIM_END);
    DragonFight& f = wstate.dragon;
    for (Entity& e : entities)
        if (e.dim == DIM_END && !e.removed && (e.type == ent::EnderDragon || e.kind == EK_CRYSTAL || e.kind == EK_CLOUD))
            removeEntity(e);
    dragonId_ = -1;
    for (int i = 0; i < MC_MAX_PLAYERS; i++)
        if (players[i].inPlay() && players[i].bossBar) sendBossBar(players[i], 1);
    if (f.portalY) {
        world.load(DIM_END, 0, 0);
        placeExitPortal(false);
        if (asNew && blockIdOf(blockAt(0, f.portalY + 4, 0)) == blk::DragonEgg) world.setBlock(DIM_END, 0, f.portalY + 4, 0, 0);
    }
    f.state = DragonFight::NOT_STARTED;
    f.crystals = 0x3FF;
    f.dragonHealth = 200;
    if (asNew) {
        f.previouslyKilled = false;
        f.eggPlaced = false;
    }
    wstate.dirty = true;
    packWorldState();
    if (storage) saveMetaLater();
}

}  // namespace mc
