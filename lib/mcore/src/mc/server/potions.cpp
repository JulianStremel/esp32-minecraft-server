// Potions after vanilla 1.21 (Potions, PotionItem, ThrowablePotionItem, ThrownSplashPotion,
// ThrownLingeringPotion, AreaEffectCloud): what each potion holds, drinking, glass bottles
// filled with water, splash potions (effects by distance, 4 blocks), lingering potions (a
// cloud that shrinks as it is used). Items keep the potion as 1.16.5 NBT ("Potion"), sent
// to the client as the potion_contents component (item.cpp). On mobs only instant effects
// act (healing and harming, the other way round for the undead).
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "mc/bytebuf.h"
#include "mc/nbt.h"
#include "mc/registry.h"
#include "mc/server/mob_util.h"
#include "mc/server/server.h"

namespace mc {

using namespace mobs;

namespace {

struct PotionEffect { uint8_t effect, amp; int32_t ticks; };
struct PotionDef { uint8_t n; PotionEffect fx[2]; };

// by potion id (Potions)
const PotionDef POTIONS[] = {
    {0, {}}, {0, {}}, {0, {}}, {0, {}},   // water, mundane, thick, awkward
    {1, {{eff::NightVision, 0, 3600}}}, {1, {{eff::NightVision, 0, 9600}}},
    {1, {{eff::Invisibility, 0, 3600}}}, {1, {{eff::Invisibility, 0, 9600}}},
    {1, {{eff::JumpBoost, 0, 3600}}}, {1, {{eff::JumpBoost, 0, 9600}}}, {1, {{eff::JumpBoost, 1, 1800}}},
    {1, {{eff::FireResistance, 0, 3600}}}, {1, {{eff::FireResistance, 0, 9600}}},
    {1, {{eff::Speed, 0, 3600}}}, {1, {{eff::Speed, 0, 9600}}}, {1, {{eff::Speed, 1, 1800}}},
    {1, {{eff::Slowness, 0, 1800}}}, {1, {{eff::Slowness, 0, 4800}}}, {1, {{eff::Slowness, 3, 400}}},
    {2, {{eff::Slowness, 3, 400}, {eff::Resistance, 2, 400}}},    // the turtle master
    {2, {{eff::Slowness, 3, 800}, {eff::Resistance, 2, 800}}},
    {2, {{eff::Slowness, 5, 400}, {eff::Resistance, 3, 400}}},
    {1, {{eff::WaterBreathing, 0, 3600}}}, {1, {{eff::WaterBreathing, 0, 9600}}},
    {1, {{eff::InstantHealth, 0, 1}}}, {1, {{eff::InstantHealth, 1, 1}}},
    {1, {{eff::InstantDamage, 0, 1}}}, {1, {{eff::InstantDamage, 1, 1}}},
    {1, {{eff::Poison, 0, 900}}}, {1, {{eff::Poison, 0, 1800}}}, {1, {{eff::Poison, 1, 432}}},
    {1, {{eff::Regeneration, 0, 900}}}, {1, {{eff::Regeneration, 0, 1800}}}, {1, {{eff::Regeneration, 1, 450}}},
    {1, {{eff::Strength, 0, 3600}}}, {1, {{eff::Strength, 0, 9600}}}, {1, {{eff::Strength, 1, 1800}}},
    {1, {{eff::Weakness, 0, 1800}}}, {1, {{eff::Weakness, 0, 4800}}},
    {1, {{eff::Luck, 0, 6000}}},
    {1, {{eff::SlowFalling, 0, 1800}}}, {1, {{eff::SlowFalling, 0, 4800}}},
    {1, {{eff::WindCharged, 0, 3600}}}, {1, {{eff::Weaving, 0, 3600}}}, {1, {{eff::Oozing, 0, 3600}}},
    {1, {{eff::Infested, 0, 3600}}},
};
static_assert(sizeof(POTIONS) / sizeof(POTIONS[0]) == potion::COUNT, "a definition per potion");
static_assert(potion::Infested == 45 && potion::TurtleMaster == 19, "vanilla's potion order");

// MobEffect colours (1.21), by effect id
const uint32_t EFFECT_COLOR[eff::COUNT] = {
    0x33EBFF, 0x8BAFE0, 0xD9C043, 0x4A4217, 0xFFC700, 0xF82423, 0xA9656A, 0xFDFF84, 0x551D4A, 0xCD5CAB,
    0x9146F0, 0xFF9900, 0x98DAC0, 0xF6F6F6, 0x1F1F23, 0xC2FF66, 0x587653, 0x484D48, 0x87A363, 0x736156,
    0xF87D23, 0x2552A5, 0xF82423, 0x94A061, 0xCEFFFF, 0x59C106, 0xC0A44D, 0xF3CFB9, 0x1DC2D1, 0x88A3BE,
    0x0B6138, 0x44FF44, 0x292721, 0x16A6A6, 0xDE4058, 0xBDC9FF, 0x78695A, 0x99FFA3, 0x8C9B8C,
};
constexpr uint32_t WATER_COLOR = 0x385DC6;

bool isInstant(int id) { return id == eff::InstantHealth || id == eff::InstantDamage; }

bool undead(uint16_t type) {
    switch (type) {
        case ent::Zombie: case ent::Skeleton: case ent::WitherSkeleton: case ent::ZombifiedPiglin:
        case ent::Drowned: case ent::Husk: case ent::Stray: case ent::Phantom: case ent::Wither:
        case ent::ZombieVillager: case ent::ZombieHorse: case ent::SkeletonHorse: case ent::Zoglin:
        case ent::Bogged:
            return true;
        default:
            return false;
    }
}

void levelEvent(Server& s, int type, double x, double y, double z, int data) {
    Packet pk(pkt::s2c::WorldEvent);
    pk.w.i32(type);
    pk.w.u64(packPos((int)floor(x), (int)floor(y), (int)floor(z)));
    pk.w.i32(data);
    pk.w.boolean(false);
    s.broadcastNear(pk, (int)floor(x) >> 4, (int)floor(z) >> 4, nullptr);
}

}  // namespace

// The potion of a potion item (its NBT "Potion"), -1: none.
int potionOf(const ItemStack& it) {
    if (it.tagSize() <= 3) return -1;
    Reader r(it.tagData() + 3, it.tagSize() - 3);
    while (r.ok()) {
        uint8_t t = r.u8();
        if (t == NBT_END) break;
        uint16_t l = r.u16();
        const uint8_t* k = r.take(l);
        if (!r.ok()) break;
        if (t == NBT_STRING && l == 6 && !memcmp(k, "Potion", 6)) {
            uint16_t sl = r.u16();
            const char* v = (const char*)r.take(sl);
            if (!r.ok()) return -1;
            if (sl > 10 && !memcmp(v, "minecraft:", 10)) { v += 10; sl -= 10; }
            for (int i = 0; i < potion::COUNT; i++)
                if (strlen(POTION_NAME[i]) == sl && !memcmp(POTION_NAME[i], v, sl)) return i;
            return -1;
        }
        nbtSkipPayload(r, t);
    }
    return -1;
}

ItemStack potionItem(uint16_t item, int potionId, int count) {
    ItemStack s = ItemStack::of(item, count);
    if (potionId < 0 || potionId >= potion::COUNT) return s;
    ByteBuf buf;
    Writer w(buf);
    NbtWriter n(w);
    n.beginRoot();
    char name[64];
    snprintf(name, sizeof(name), "minecraft:%s", POTION_NAME[potionId]);
    n.str("Potion", name);
    n.end();
    if (!buf.failed()) s.setTag(buf.data(), buf.size());
    return s;
}

// PotionContents#getColor: the effects' colours averaged by level; water's blue without.
uint32_t potionColor(int potionId) {
    if (potionId < 0 || potionId >= potion::COUNT || !POTIONS[potionId].n) return WATER_COLOR;
    uint32_t r = 0, g = 0, b = 0, n = 0;
    for (int i = 0; i < POTIONS[potionId].n; i++) {
        const PotionEffect& f = POTIONS[potionId].fx[i];
        uint32_t c = EFFECT_COLOR[f.effect], w = f.amp + 1u;
        r += ((c >> 16) & 255) * w;
        g += ((c >> 8) & 255) * w;
        b += (c & 255) * w;
        n += w;
    }
    return (r / n) << 16 | (g / n) << 8 | (b / n);
}

// One potion's effects on a player, scaled (splash: by distance; lingering clouds: a
// quarter of the time, half of the instant effects).
void Server::potionOnPlayer(Player& p, int potionId, double scale, double timeScale, int32_t source) {
    if (potionId < 0 || potionId >= potion::COUNT || p.dead) return;
    for (int i = 0; i < POTIONS[potionId].n; i++) {
        const PotionEffect& f = POTIONS[potionId].fx[i];
        int amp = f.amp < 8 ? f.amp : 8;
        if (isInstant(f.effect)) {   // InstantenousMobEffect#applyInstantenousEffect
            if (f.effect == eff::InstantHealth) heal(p, (float)(int)(scale * (4 << amp) + 0.5));
            else damagePlayer(p, (float)(int)(scale * (6 << amp) + 0.5), DC_MAGIC, source);
        } else {
            int ticks = (int)(scale * timeScale * f.ticks + 0.5);
            if (ticks > 20 || (scale >= 1 && ticks > 0)) addEffect(p, f.effect, f.amp, ticks);
        }
    }
}

// ... on a mob: only healing and harming (the other way round for the undead).
void Server::potionOnMob(Entity& m, int potionId, double scale, int32_t source) {
    if (potionId < 0 || potionId >= potion::COUNT || m.health <= 0) return;
    for (int i = 0; i < POTIONS[potionId].n; i++) {
        const PotionEffect& f = POTIONS[potionId].fx[i];
        if (!isInstant(f.effect)) continue;
        int amp = f.amp < 8 ? f.amp : 8;
        bool heals = (f.effect == eff::InstantHealth) != undead(m.type);
        if (heals) {
            m.health += (float)(int)(scale * (4 << amp) + 0.5);
            if (m.health > m.maxHealth) m.health = m.maxHealth;
            m.metaDirty = true;
        } else {
            damageEntity(m, (float)(int)(scale * (6 << amp) + 0.5), DC_MAGIC, source);
        }
    }
}

// PotionItem#finishUsingItem: the effects, and the empty bottle back.
void Server::drinkPotion(Player& p, ItemStack& it, int slot) {
    int potionId = potionOf(it);
    potionOnPlayer(p, potionId, 1.0, 1.0, -1);
    if (p.gamemode != GM_CREATIVE) {
        it = ItemStack::of(itm::GlassBottle);
        p.sendSlot(slot);
    }
    playSound("entity.generic.drink", p.e.x, p.e.y, p.e.z, 0.5f, 1, 7);
}

// BottleItem#use: a glass bottle used on water (the first water source the player looks
// at within its reach, not through blocks) becomes a water bottle.
bool Server::fillBottle(Player& p, int hand) {
    ItemStack& it = hand == 1 ? p.inv[SLOT_OFFHAND] : p.heldItem();
    if (it.empty() || it.id != itm::GlassBottle) return false;
    double yaw = p.e.yaw * M_PI / 180.0, pitch = p.e.pitch * M_PI / 180.0;
    double dx = -sin(yaw) * cos(pitch), dy = -sin(pitch), dz = cos(yaw) * cos(pitch);
    double ex = p.e.x, ey = p.e.y + 1.62, ez = p.e.z;
    for (double t = 0; t <= 5.0; t += 0.1) {
        int x = (int)floor(ex + dx * t), y = (int)floor(ey + dy * t), z = (int)floor(ez + dz * t);
        uint16_t st = blockAt(x, y, z);
        bool water = (blockIdOf(st) == blk::Water && getProp(st, "level") == 0) || getBool(st, "waterlogged");
        if (water) {
            playSound("item.bottle.fill", x + 0.5, y + 0.5, z + 0.5, 1, 1, 7);
            ItemStack filled = potionItem(itm::Potion, potion::Water);
            if (p.gamemode == GM_CREATIVE) {
                giveItem(p, filled);
            } else if (it.count == 1) {
                it = filled;
                p.sendSlot(hand == 1 ? SLOT_OFFHAND : SLOT_HOTBAR_START + p.held);
            } else {
                it.count--;
                p.sendSlot(hand == 1 ? SLOT_OFFHAND : SLOT_HOTBAR_START + p.held);
                if (giveItem(p, filled)) dropItem(p.e.x, p.e.y + 0.5, p.e.z, filled, false);
            }
            return true;
        }
        if (stateCollides(st)) return false;
    }
    return false;
}

// ThrowablePotionItem#use: thrown 20 degrees above where the player looks at 0.5 blocks a
// tick (with the player's own motion), slightly scattered.
bool Server::throwPotion(Player& p, int hand) {
    ItemStack& it = hand == 1 ? p.inv[SLOT_OFFHAND] : p.heldItem();
    if (it.empty() || (it.id != itm::SplashPotion && it.id != itm::LingeringPotion)) return false;
    InDim in(*this, p.e.dim);
    Entity* e = spawnEntity(EK_POTION, it.id == itm::SplashPotion ? ent::SplashPotion : ent::LingeringPotion, p.e.x,
                            p.e.y + 1.52, p.e.z);
    if (!e) return false;
    Rng& r = rng();
    double yaw = p.e.yaw * M_PI / 180.0, pitch = p.e.pitch * M_PI / 180.0, up = (p.e.pitch - 20.0) * M_PI / 180.0;
    double dx = -sin(yaw) * cos(pitch), dy = -sin(up), dz = cos(yaw) * cos(pitch);
    double len = sqrt(dx * dx + dy * dy + dz * dz);
    double sx = 0.0172275 * (r.unit() - r.unit()), sy = 0.0172275 * (r.unit() - r.unit()), sz = 0.0172275 * (r.unit() - r.unit());
    e->vx = (dx / len + sx) * 0.5 + p.e.vx;
    e->vy = (dy / len + sy) * 0.5 + (p.e.onGround ? 0 : p.e.vy);
    e->vz = (dz / len + sz) * 0.5 + p.e.vz;
    e->owner = p.e.id;
    e->dim = p.e.dim;
    e->width = e->height = 0.25f;
    e->item = it;
    e->item.count = 1;
    e->velDirty = true;
    playSound(it.id == itm::SplashPotion ? "entity.splash_potion.throw" : "entity.lingering_potion.throw", p.e.x, p.e.y,
              p.e.z, 0.5f, 0.4f, 7);
    if (p.gamemode != GM_CREATIVE) {
        if (--it.count == 0) it.clear();
        p.sendSlot(hand == 1 ? SLOT_OFFHAND : SLOT_HOTBAR_START + p.held);
    }
    return true;
}

// A thrown potion: 0.05 gravity, 0.99 drag; breaks on the first block or entity.
void Server::tickThrownPotion(Entity& e) {
    if (e.age > 1200 || e.y < dimVoidY(e.dim) || !world.isResident(curDim, (int)floor(e.x) >> 4, (int)floor(e.z) >> 4)) {
        removeEntity(e);
        return;
    }
    double sp = sqrt(e.vx * e.vx + e.vy * e.vy + e.vz * e.vz);
    int steps = (int)(sp / 0.25) + 1;
    bool hit = false;
    Entity* direct = nullptr;
    double hx = e.x, hy = e.y, hz = e.z;
    for (int st = 1; st <= steps && !hit; st++) {
        double t = (double)st / steps;
        double nx = e.x + e.vx * t, ny = e.y + e.vy * t, nz = e.z + e.vz * t;
        if (stateCollides(blockAt((int)floor(nx), (int)floor(ny), (int)floor(nz)))) {
            hit = true;
            break;
        }
        hx = nx; hy = ny; hz = nz;
        bool skipOwner = e.age < 5;   // it starts inside its thrower
        for (int i = 0; i < MC_MAX_PLAYERS && !hit; i++) {
            Player& p = players[i];
            if (!p.inPlay() || p.dead || p.gamemode == GM_SPECTATOR || p.e.dim != e.dim) continue;
            if (skipOwner && p.e.id == e.owner) continue;
            if (fabs(p.e.x - nx) < 0.425 && fabs(p.e.z - nz) < 0.425 && ny > p.e.y - 0.125 && ny < p.e.y + 1.925) {
                hit = true;
                direct = &p.e;
            }
        }
        for (int i = 0; i < MC_MAX_ENTITIES && !hit; i++) {
            Entity& m = entities[i];
            if (m.kind != EK_MOB || m.removed || m.health <= 0 || m.dim != e.dim) continue;
            if (fabs(m.x - nx) < m.width / 2 + 0.125 && fabs(m.z - nz) < m.width / 2 + 0.125 && ny > m.y - 0.125 &&
                ny < m.y + m.height + 0.125) {
                hit = true;
                direct = &m;
            }
        }
    }
    if (hit) {
        e.x = hx; e.y = hy; e.z = hz;
        potionBreaks(e, direct);
        removeEntity(e);
        return;
    }
    e.x = hx; e.y = hy; e.z = hz;
    double drag = inFluid(*this, e, blk::Water) ? 0.8 : 0.99;
    e.vx *= drag;
    e.vy = e.vy * drag - 0.05;
    e.vz *= drag;
}

// AbstractThrownPotion#onHit: water puts out fire (and hurts blazes and endermen), a
// splash potion's effects reach what is within 4 blocks, a lingering potion leaves a cloud.
void Server::potionBreaks(Entity& e, Entity* direct) {
    int potionId = potionOf(e.item);
    bool lingering = e.type == ent::LingeringPotion;
    bool instantOnly = potionId >= 0 && POTIONS[potionId].n > 0;
    for (int i = 0; potionId >= 0 && i < POTIONS[potionId].n; i++) instantOnly &= isInstant(POTIONS[potionId].fx[i].effect);
    levelEvent(*this, instantOnly ? 2007 : 2002, e.x, e.y, e.z, (int)potionColor(potionId));
    if (potionId == potion::Water || potionId < 0) {   // AbstractThrownPotion#applyWater, #dowseFire
        int bx = (int)floor(e.x), by = (int)floor(e.y), bz = (int)floor(e.z);
        static const int8_t AROUND[5][3] = {{0, 0, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};
        for (const auto& d : AROUND)
            for (int dy = 0; dy <= 1; dy++) {
                uint16_t id = blockIdOf(blockAt(bx + d[0], by + dy, bz + d[2]));
                if (id == blk::Fire || id == blk::SoulFire) setBlock(bx + d[0], by + dy, bz + d[2], bs::Air);
            }
        for (int i = 0; i < MC_MAX_PLAYERS; i++) {
            Player& p = players[i];
            if (!p.inPlay() || p.e.dim != e.dim || p.e.fireTicks <= 0) continue;
            double dx = p.e.x - e.x, dy = p.e.y - e.y, dz = p.e.z - e.z;
            if (dx * dx + dy * dy + dz * dz < 16) { p.e.fireTicks = 0; p.e.metaDirty = true; }
        }
        for (int i = 0; i < MC_MAX_ENTITIES; i++) {
            Entity& m = entities[i];
            if (m.kind != EK_MOB || m.removed || m.health <= 0 || m.dim != e.dim) continue;
            double dx = m.x - e.x, dy = m.y - e.y, dz = m.z - e.z;
            if (dx * dx + dy * dy + dz * dz >= 16) continue;
            if (m.fireTicks > 0) { m.fireTicks = 0; m.metaDirty = true; }
            if (m.type == ent::Blaze || m.type == ent::Enderman) damageEntity(m, 1, DC_MAGIC, e.owner);
        }
        return;
    }
    if (lingering) {   // ThrownLingeringPotion#onHitAsPotion: a cloud of radius 3 for 30 s
        potionCloud(e.x, e.y, e.z, potionId, e.owner);
        return;
    }
    // ThrownSplashPotion#onHitAsPotion: within 4 blocks (a box 8 x 4 x 8), weaker farther away
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& p = players[i];
        if (!p.inPlay() || p.dead || p.gamemode == GM_SPECTATOR || p.e.dim != e.dim) continue;
        double dx = p.e.x - e.x, dy = p.e.y - e.y, dz = p.e.z - e.z, d2 = dx * dx + dy * dy + dz * dz;
        if (fabs(dy) > 2 + 1.8 || d2 >= 16) continue;
        potionOnPlayer(p, potionId, direct == &p.e ? 1.0 : 1.0 - sqrt(d2) / 4.0, 1.0, e.owner);
    }
    for (int i = 0; i < MC_MAX_ENTITIES; i++) {
        Entity& m = entities[i];
        if (m.kind != EK_MOB || m.removed || m.health <= 0 || m.dim != e.dim) continue;
        double dx = m.x - e.x, dy = m.y - e.y, dz = m.z - e.z, d2 = dx * dx + dy * dy + dz * dz;
        if (d2 >= 16) continue;
        potionOnMob(m, potionId, direct == &m ? 1.0 : 1.0 - sqrt(d2) / 4.0, e.owner);
    }
}

// A lingering potion's cloud (AreaEffectCloud): waits 10 ticks, lasts 30 s while its
// radius shrinks from 3 to 0, and loses 0.5 for each one it gives the effects to (every
// second at most), at a quarter of their time and half of the instant ones.
Entity* Server::potionCloud(double x, double y, double z, int potionId, int32_t owner) {
    Entity* c = spawnEntity(EK_CLOUD, ent::AreaEffectCloud, x, y, z);
    if (!c) return nullptr;
    c->damage = 3.0f;    // the radius
    c->charge = 600;     // the duration
    c->owner = owner;
    c->width = 6.0f;
    c->height = 0.5f;
    c->item = potionItem(itm::LingeringPotion, potionId);
    c->dim = curDim;
    return c;
}

void Server::tickPotionCloud(Entity& c) {
    const int WAIT = 10;
    if (c.age >= (uint32_t)(WAIT + c.charge)) {
        removeEntity(c);
        return;
    }
    if (c.age == WAIT) c.metaDirty = true;   // no longer waiting
    if (c.age < (uint32_t)WAIT) return;
    float before = c.damage;
    c.damage -= 3.0f / c.charge;   // radiusPerTick
    if (c.damage < 0.5f) {
        removeEntity(c);
        return;
    }
    if ((c.age - WAIT) % 20 == 0) {
        int potionId = potionOf(c.item);
        float r = c.damage;
        for (int i = 0; i < MC_MAX_PLAYERS && c.damage >= 0.5f; i++) {
            Player& p = players[i];
            if (!p.inPlay() || p.dead || p.gamemode == GM_SPECTATOR || p.e.dim != c.dim) continue;
            double dx = p.e.x - c.x, dz = p.e.z - c.z;
            if (dx * dx + dz * dz > r * r || p.e.y < c.y - 1.8 || p.e.y > c.y + 0.5) continue;
            potionOnPlayer(p, potionId, 0.5, 0.5, c.owner);   // 0.5 x 0.5: a quarter of the time
            c.damage -= 0.5f;   // radiusOnUse
        }
        for (int i = 0; i < MC_MAX_ENTITIES && c.damage >= 0.5f; i++) {
            Entity& m = entities[i];
            if (m.kind != EK_MOB || m.removed || m.health <= 0 || m.dim != c.dim) continue;
            double dx = m.x - c.x, dz = m.z - c.z;
            if (dx * dx + dz * dz > r * r || m.y < c.y - m.height || m.y > c.y + 0.5) continue;
            potionOnMob(m, potionId, 0.5, c.owner);
            c.damage -= 0.5f;
        }
        if (c.damage < 0.5f) {
            removeEntity(c);
            return;
        }
    }
    if ((int)(before * 10) != (int)(c.damage * 10)) c.metaDirty = true;   // the client draws the new radius
}

}  // namespace mc
