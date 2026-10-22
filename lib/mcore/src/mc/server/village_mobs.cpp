// A village's friends and foes, after vanilla 1.21: zombies hunt villagers (and iron
// golems), and a villager a zombie kills on normal or hard becomes a zombie villager that
// keeps its profession and trades; weakness and a golden apple cure it (Zombie
// #killedEntity, ZombieVillager#mobInteract, #finishConversion) and the curer gets the
// cured gossip, so its prices drop. Villagers flee from zombies and illagers, remember
// who hurt them, and iron golems -- spawned where five or more villagers live and none
// guards them, or built from iron blocks and a carved pumpkin -- hunt the monsters near
// them and the players their villagers think badly of (IronGolem, DefendVillageTargetGoal).
#include <math.h>
#include <string.h>
#include "mc/registry.h"
#include "mc/server/mob_util.h"
#include "mc/server/server.h"

namespace mc {

namespace {

bool isZombie(uint16_t t) { return t == ent::Zombie || t == ent::ZombieVillager || t == ent::Husk || t == ent::Drowned; }

// what villagers flee from (VillagerHostilesSensor: zombies 8 blocks, illagers 12 or so)
bool scaresVillagers(uint16_t t) {
    return isZombie(t) || t == ent::Vindicator || t == ent::Evoker || t == ent::Pillager || t == ent::Ravager ||
           t == ent::Vex || t == ent::Illusioner || t == ent::Zoglin;
}

uint64_t whoOf(const Player& p) {
    uint64_t w = 0;
    for (int i = 0; i < 8; i++) w = w << 8 | p.uuid[i];
    return w ? w : 1;
}

double dist2(const Entity& a, const Entity& b) {
    double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

}  // namespace

// The entity a mob goes after besides players (NearestAttackableTargetGoal): zombies the
// villagers and iron golems within 16 blocks, iron golems the monsters within 10 (not
// creepers). The nearest, chosen again every 20 ticks (so one it cannot reach does not
// keep it from one that came closer), kept in between while it is alive and near.
Entity* Server::preyOf(Entity& e) {
    bool zombie = isZombie(e.type) || (isRaiderType(e.type) && (e.type != ent::Witch || e.raid >= 0));
    bool golem = e.type == ent::IronGolem;
    if (!zombie && !golem) return nullptr;
    if (zombie && cfg.difficulty == 0) return nullptr;
    double range = zombie ? 16 : 10;
    Entity* prey = e.prey >= 0 ? findEntity(e.prey) : nullptr;
    if (prey && (prey->kind != EK_MOB || prey->health <= 0 || prey->dim != e.dim || dist2(*prey, e) > range * range * 2.25))
        prey = nullptr;
    if (((uint32_t)e.id + e.age) % 20 == 0) {
        double best = range * range;
        prey = nullptr;
        for (int i = 0; i < MC_MAX_ENTITIES; i++) {
            Entity& o = entities[i];
            if (&o == &e || o.kind != EK_MOB || o.removed || o.health <= 0 || o.dim != e.dim) continue;
            bool wanted = zombie ? (o.type == ent::Villager || o.type == ent::IronGolem ||
                                    (isRaiderType(e.type) && o.type == ent::WanderingTrader))
                                 : (o.hostile && o.type != ent::Creeper && o.type != ent::EnderDragon);
            if (!wanted) continue;
            double d = dist2(o, e);
            if (d < best) {
                best = d;
                prey = &o;
            }
        }
    }
    e.prey = prey ? prey->id : -1;
    return prey;
}

// One mob hitting another (Mob#doHurtTarget): an iron golem 7.5 to 21.5 and the target
// thrown up (IronGolem#doHurtTarget), others their attack.
void Server::mobHit(Entity& e, Entity& target, float attack) {
    float dmg = attack;
    if (e.type == ent::IronGolem) dmg = 7.5f + (float)mobs::rng().range(15);
    if (damageEntity(target, dmg, DC_ATTACK, e.id) && e.type == ent::IronGolem) {
        target.vy += 0.4;
        target.velDirty = true;
    }
    if (e.type == ent::IronGolem) {
        broadcastStatus(e, 4);   // its arms swing
        playSound("entity.iron_golem.attack", e.x, e.y, e.z, 1, 1, 5);
    } else {
        broadcastAnimation(e, 0, nullptr);
    }
    e.attackCooldown = 20;
}

// ------------------------------------------------------------------ villagers' memory
void Server::villagerHurt(Entity& v, int32_t attacker) {
    wakeVillager(v);
    Player* p = playerByEntity(attacker);
    if (!p) return;
    Gossip* g = v.villager.gossipOf(whoOf(*p));   // VILLAGER_HURT: minor negative 25
    g->minorNeg = (uint8_t)(g->minorNeg + 25 > 200 ? 200 : g->minorNeg + 25);
    int rep = g->reputation();
    if (rep > -100) return;
    // DefendVillageTargetGoal: the golems near it go after that player
    for (int i = 0; i < MC_MAX_ENTITIES; i++) {
        Entity& o = entities[i];
        if (o.kind != EK_MOB || o.removed || o.type != ent::IronGolem || (o.variant & 1) || o.dim != v.dim ||
            dist2(o, v) > 16 * 16)
            continue;
        if (p->isSurvivalLike()) {
            o.angryAt = p->e.id;
            o.angerTicks = 600;
        }
    }
}

void Server::gossipTraded(Entity& v, Player& p) {
    Gossip* g = v.villager.gossipOf(whoOf(p));   // TRADE: trading 2, at most 25
    g->trading = (uint8_t)(g->trading + 2 > 25 ? 25 : g->trading + 2);
}

// GossipContainer#decay, once a day: minor positive 1, trading 2, minor negative 20,
// major negative 10 (major positive stays)
void Server::decayGossip(Entity& v) {
    uint32_t day = (uint32_t)(meta.worldAge / 24000);
    VillagerState& s = v.villager;
    if (s.gossipDay == day) return;
    int days = s.gossipDay ? (int)(day - s.gossipDay) : 0;
    s.gossipDay = day;
    for (int k = 0; k < days && k < 30; k++)
        for (Gossip& g : s.gossip) {
            if (!g.who) continue;
            g.minorPos = (uint8_t)(g.minorPos > 1 ? g.minorPos - 1 : 0);
            g.trading = (uint8_t)(g.trading > 2 ? g.trading - 2 : 0);
            g.minorNeg = (uint8_t)(g.minorNeg > 20 ? g.minorNeg - 20 : 0);
            g.majorNeg = (uint8_t)(g.majorNeg > 10 ? g.majorNeg - 10 : 0);
            if (!g.majorPos && !g.minorPos && !g.trading && !g.minorNeg && !g.majorNeg) g = Gossip();
        }
}

// Villager#updateSpecialPrices: a trade's price less (or more) the player's reputation
// times its multiplier, while they trade.
void Server::specialPrices(Entity& v, Player* p) {
    int rep = p ? v.villager.reputation(whoOf(*p)) : 0;
    int hero = p ? p->effect(eff::HeroOfTheVillage) : 0;   // its level (0: none)
    for (TradeOffer& o : v.villager.trades) {
        int special = rep ? -(int)floorf((float)rep * o.priceMult) : 0;
        if (hero) {
            int off = (int)floor((0.3 + 0.0625 * (hero - 1)) * o.buyA.count);
            special -= off > 1 ? off : 1;
        }
        o.special = (int16_t)special;
    }
}

// A villager near zombies or illagers runs from them (VillagerPanicTrigger).
void Server::villagerFlee(Entity& v) {
    if (((uint32_t)v.id + v.age) % 10 != 0) return;
    for (int i = 0; i < MC_MAX_ENTITIES; i++) {
        Entity& o = entities[i];
        if (o.kind != EK_MOB || o.removed || o.health <= 0 || o.dim != v.dim || !scaresVillagers(o.type)) continue;
        double r = isZombie(o.type) ? 8 : 12;
        if (dist2(o, v) > r * r) continue;
        v.lastAttacker = o.id;   // the panic of tickMob, until ET_CALM
        timers.cancel(TimerKey::entity(v.id, ET_CALM));
        scheduleEntityTimer(v, ET_CALM, 60);
        return;
    }
}

// ------------------------------------------------------------------ zombie villagers
// Zombie#killedEntity: on normal half the time, on hard always; it keeps what it was.
bool Server::villagerKilled(Entity& v, int32_t attacker) {
    Entity* z = findEntity(attacker);
    if (!z || z->kind != EK_MOB || !isZombie(z->type) || cfg.difficulty < 2) return false;
    if (cfg.difficulty == 2 && mobs::rng().range(2) == 0) return false;
    Entity* zv = spawnMob(ent::ZombieVillager, v.x, v.y, v.z);
    if (!zv) return false;
    zv->villager = v.villager;
    zv->villager.hasJob = false;
    zv->variant = v.variant;
    zv->yaw = zv->headYaw = v.yaw;
    zv->persistent = true;   // (ZombieVillager#removeWhenFarAway: not one that traded)
    zv->metaDirty = true;
    playSound("entity.zombie.infect", v.x, v.y, v.z, 1, 1, 5);
    removeEntity(v);
    return true;
}

// ZombieVillager#mobInteract: a golden apple for one weakened: the cure starts.
bool Server::cureZombieVillager(Player& p, Entity& z) {
    ItemStack& h = p.heldItem();
    if (h.id != itm::GoldenApple || z.weakTicks <= 0 || z.villager.convertTicks > 0) return false;
    if (p.gamemode != GM_CREATIVE) consumeHeld(p);
    z.villager.convertTicks = (int16_t)(3600 + mobs::rng().range(2401));
    z.villager.convertBy = whoOf(p);
    z.weakTicks = 0;
    z.persistent = true;
    z.metaDirty = true;
    broadcastStatus(z, 16);   // the cure's sound
    return true;
}

// Each tick of a zombie villager: weakness wears off, the cure goes on (ZombieVillager
// #getConversionProgress: iron bars and beds near it are not counted here) and ends with
// the villager back, who thinks well of its curer (CURED: major positive 20, minor 25).
void Server::tickZombieVillager(Entity& z) {
    if (z.weakTicks > 0) z.weakTicks--;
    if (z.villager.convertTicks <= 0 || --z.villager.convertTicks > 0) return;
    Entity* v = spawnMob(ent::Villager, z.x, z.y, z.z);
    if (!v) {
        z.villager.convertTicks = 20;   // (no room: soon again)
        return;
    }
    v->villager = z.villager;
    v->villager.convertTicks = 0;
    v->variant = z.variant;
    v->yaw = v->headYaw = z.yaw;
    if (z.villager.convertBy) {
        Gossip* g = v->villager.gossipOf(z.villager.convertBy);
        g->majorPos = 20;
        g->minorPos = (uint8_t)(g->minorPos + 25 > 25 ? 25 : g->minorPos + 25);
    }
    v->villager.convertBy = 0;
    v->metaDirty = true;
    Packet pk(pkt::s2c::WorldEvent);   // the zombie villager converted
    pk.w.i32(1027);
    pk.w.u64(packPos((int)floor(z.x), (int)floor(z.y), (int)floor(z.z)));
    pk.w.i32(0);
    pk.w.boolean(false);
    broadcastNear(pk, (int)floor(z.x) >> 4, (int)floor(z.z) >> 4, nullptr);
    removeEntity(z);
}

// ------------------------------------------------------------------ iron golems
static bool golemFits(Server& s, int x, int y, int z) {
    if (!stateCollides(s.blockAt(x, y - 1, z))) return false;
    for (int dy = 0; dy < 3; dy++) {
        uint16_t st = s.blockAt(x, y + dy, z);
        if (stateCollides(st) || stateIsFluid(st)) return false;
    }
    return true;
}

// Villager#spawnGolemIfNeeded, simplified: a villager with four more villagers within 10
// blocks and no golem within 16 has one come (tried at 10 spots around it), at most every
// 30 s for each villager.
void Server::villageGolem(Entity& v) {
    if (((uint32_t)v.id * 13u + v.age) % 600 != 0) return;
    int villagers = 0;
    for (int i = 0; i < MC_MAX_ENTITIES; i++) {
        const Entity& o = entities[i];
        if (o.kind != EK_MOB || o.removed || o.health <= 0 || o.dim != v.dim) continue;
        double d = dist2(o, v);
        if (o.type == ent::IronGolem && d < 16 * 16) return;
        if (o.type == ent::Villager && d < 10 * 10) villagers++;
    }
    if (villagers < 5) return;
    Rng& r = mobs::rng();
    for (int t = 0; t < 10; t++) {
        int x = (int)floor(v.x) + r.between(-8, 8), z = (int)floor(v.z) + r.between(-8, 8);
        for (int y = (int)floor(v.y) + 6; y >= (int)floor(v.y) - 6; y--) {
            if (!world.isResident(v.dim, x >> 4, z >> 4) || !golemFits(*this, x, y, z)) continue;
            Entity* g = spawnMob(ent::IronGolem, x + 0.5, y, z + 0.5);
            if (g) playSound("entity.iron_golem.repair", x + 0.5, y, z + 0.5, 1, 1, 5);
            return;
        }
    }
}

// CarvedPumpkinBlock#trySpawnGolem: a carved pumpkin (or jack o'lantern) on a T of four
// iron blocks (arms along x or z, air beside the foot) makes a golem; the blocks go.
bool Server::buildGolem(int x, int y, int z) {
    uint16_t head = blockIdOf(blockAt(x, y, z));
    if (head != blk::CarvedPumpkin && head != blk::JackOLantern) return false;
    auto iron = [&](int a, int b, int c) { return blockIdOf(blockAt(a, b, c)) == blk::IronBlock; };
    auto clear = [&](int a, int b, int c) { uint16_t st = blockAt(a, b, c); return stateIsAir(st) || (blockOf(st).flags & BF_REPLACEABLE); };
    if (!iron(x, y - 1, z) || !iron(x, y - 2, z)) return false;
    for (int axis = 0; axis < 2; axis++) {
        int ax = axis == 0, az = axis == 1;
        if (!iron(x - ax, y - 1, z - az) || !iron(x + ax, y - 1, z + az)) continue;
        if (!clear(x - ax, y - 2, z - az) || !clear(x + ax, y - 2, z + az)) continue;
        for (int b : {0, 1, 2}) setBlock(x, y - b, z, 0);
        setBlock(x - ax, y - 1, z - az, 0);
        setBlock(x + ax, y - 1, z + az, 0);
        Entity* g = spawnMob(ent::IronGolem, x + 0.5, y - 2, z + 0.5);
        if (g) {
            g->variant = 1;   // made by a player: it never turns on players for its village
            g->persistent = true;
            g->metaDirty = true;
        }
        return true;
    }
    return false;
}

// IronGolem#mobInteract: an iron ingot heals 25.
bool Server::repairGolem(Player& p, Entity& g) {
    ItemStack& h = p.heldItem();
    if (h.id != itm::IronIngot || g.health >= g.maxHealth) return false;
    g.health = g.health + 25 > g.maxHealth ? g.maxHealth : g.health + 25;
    g.metaDirty = true;
    if (p.gamemode != GM_CREATIVE) consumeHeld(p);
    playSound("entity.iron_golem.repair", g.x, g.y, g.z, 1, 0.8f + mobs::rng().unit() * 0.4f, 5);
    return true;
}

}  // namespace mc
