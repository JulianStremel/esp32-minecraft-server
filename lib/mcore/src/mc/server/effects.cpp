// Status effects on players, after vanilla 1.21 (MobEffect and its subclasses): what
// each does per tick, the client's icons and attributes, and what food brings.
// Effects the client shows on its own (night vision, blindness, nausea, darkness, ...)
// are only sent; the rest act here, in survival.cpp (resistance, fire resistance,
// absorption, water breathing), blocks.cpp (haste, mining fatigue), entities.cpp
// (strength, weakness) and play.cpp (jump boost, slow falling).
#include <math.h>
#include "mc/registry.h"
#include "mc/server/mob_util.h"
#include "mc/server/server.h"

namespace mc {

static bool instant(int id) { return id == eff::InstantHealth || id == eff::InstantDamage; }

static void sendEffect(Player& p, int id) {
    const Player::ActiveEffect& a = p.effects[id];
    Packet pk(pkt::s2c::EntityEffect);
    pk.w.varint(p.e.id);
    pk.w.varint(id);
    pk.w.varint(a.amp);
    pk.w.varint(a.ticks);      // -1: infinite
    pk.w.u8(a.particles ? 0x06 : 0x04);   // particles, the icon
    p.conn.send(pk);
}

// Effects that change attributes or the entity's flags: what the client must be told.
static bool changesAttributes(int id) {
    return id == eff::Speed || id == eff::Slowness || id == eff::Absorption;
}
static void updateFlags(Player& p) {
    uint8_t f = p.e.flags & (uint8_t)~(EF_INVISIBLE | EF_GLOWING);
    if (p.effect(eff::Invisibility)) f |= EF_INVISIBLE;
    if (p.effect(eff::Glowing)) f |= EF_GLOWING;
    if (f != p.e.flags) {
        p.e.flags = f;
        p.e.metaDirty = true;
    }
}

// InstantenousMobEffect: healing heals 4 << level-1, harming hurts 6 << level-1 (magic).
static void applyInstant(Server& s, Player& p, int id, int amp) {
    if (id == eff::InstantHealth) s.heal(p, (float)(4 << (amp < 8 ? amp : 8)));
    else s.damagePlayer(p, (float)(6 << (amp < 8 ? amp : 8)), DC_MAGIC, -1);
}

bool Server::addEffect(Player& p, int id, int amp, int ticks, bool particles) {
    if (id < 0 || id >= eff::COUNT || p.dead || ticks == 0) return false;
    if (amp < 0) amp = 0;
    if (amp > 255) amp = 255;
    if (instant(id)) {
        applyInstant(*this, p, id, amp);
        return true;
    }
    Player::ActiveEffect& a = p.effects[id];
    // MobEffectInstance#update: a higher level replaces; the same level only lasts longer
    if (a.ticks != 0) {
        bool longer = a.ticks >= 0 && (ticks < 0 || ticks > a.ticks);
        if (amp < a.amp || (amp == a.amp && !longer)) return false;
    }
    int oldAmp = a.ticks ? a.amp : -1;
    a.ticks = ticks;
    a.amp = (uint8_t)amp;
    a.particles = particles;
    sendEffect(p, id);
    if (id == eff::Absorption) {
        // MobEffects.ABSORPTION#onEffectStarted: at least 4 per level
        float want = 4.0f * (amp + 1);
        if (p.absorption < want) p.absorption = want;
        p.e.metaDirty = true;
    }
    if (changesAttributes(id) && oldAmp != amp) sendAttributes(p);
    updateFlags(p);
    return true;
}

void Server::removeEffect(Player& p, int id) {
    if (id < 0 || id >= eff::COUNT || !p.effects[id].ticks) return;
    p.effects[id] = Player::ActiveEffect();
    Packet pk(pkt::s2c::RemoveEntityEffect);
    pk.w.varint(p.e.id);
    pk.w.varint(id);
    p.conn.send(pk);
    if (id == eff::Absorption && p.absorption > 0) {   // its max goes: the rest of it with it
        p.absorption = 0;
        p.e.metaDirty = true;
    }
    if (changesAttributes(id)) sendAttributes(p);
    updateFlags(p);
}

void Server::clearEffects(Player& p, bool send) {
    for (int id = 0; id < eff::COUNT; id++) {
        if (!p.effects[id].ticks) continue;
        if (send) removeEffect(p, id);
        else p.effects[id] = Player::ActiveEffect();
    }
    if (!send) {
        p.absorption = 0;
        updateFlags(p);
    }
}

void Server::sendEffects(Player& p) {
    for (int id = 0; id < eff::COUNT; id++)
        if (p.effects[id].ticks) sendEffect(p, id);
    sendAttributes(p);
    updateFlags(p);
}

// Update Attributes for the player's own client: movement speed with speed, slowness
// and sprinting (the client replaces all of an attribute's modifiers with these), and
// max absorption.
void Server::sendAttributes(Player& p) {
    struct Mod { const char* id; double amount; uint8_t op; };
    Mod speed[3];
    int n = 0;
    if (p.e.flags & EF_SPRINTING) speed[n++] = {"minecraft:sprinting", 0.3, 2};
    if (int l = p.effect(eff::Speed)) speed[n++] = {"minecraft:effect.speed", 0.2 * l, 2};
    if (int l = p.effect(eff::Slowness)) speed[n++] = {"minecraft:effect.slowness", -0.15 * l, 2};
    Packet pk(pkt::s2c::EntityUpdateAttributes);
    pk.w.varint(p.e.id);
    pk.w.varint(2);
    pk.w.varint(attr::MovementSpeed);
    pk.w.f64(0.1);
    pk.w.varint(n);
    for (int i = 0; i < n; i++) {
        pk.w.string(speed[i].id);
        pk.w.f64(speed[i].amount);
        pk.w.u8(speed[i].op);
    }
    pk.w.varint(attr::MaxAbsorption);
    pk.w.f64(0);
    int abs = p.effect(eff::Absorption);
    pk.w.varint(abs ? 1 : 0);
    if (abs) {
        pk.w.string("minecraft:effect.absorption");
        pk.w.f64(4.0 * abs);
        pk.w.u8(0);
    }
    p.conn.send(pk);
}

// Every tick: what the effects do (MobEffect#applyEffectTick when shouldApplyEffectTick),
// then their time runs down.
void Server::tickEffects(Player& p) {
    if (p.pearlCooldown > 0) p.pearlCooldown--;
    if (p.dead) return;
    for (int id = 0; id < eff::COUNT; id++) {
        Player::ActiveEffect& a = p.effects[id];
        if (!a.ticks) continue;
        int amp = a.amp < 5 ? a.amp : 5;
        // the interval at which a ticking effect acts: halved per level (at least every tick)
        auto every = [&](int base) { int k = base >> amp; return k < 1 || a.ticks < 0 || a.ticks % k == 0; };
        switch (id) {
            case eff::Regeneration:
                if (every(50)) heal(p, 1);
                break;
            case eff::Poison:   // never kills
                if (every(25) && p.e.health > 1) damagePlayer(p, 1, DC_MAGIC, -1);
                break;
            case eff::Wither:
                if (every(40)) damagePlayer(p, 1, DC_WITHER, -1);
                break;
            case eff::Hunger:
                addExhaustion(p, 0.005f * (a.amp + 1));
                break;
            case eff::Saturation:   // FoodData#eat(level, 1.0)
                if (p.isSurvivalLike()) {
                    p.food = p.food + a.amp + 1 > 20 ? 20 : p.food + a.amp + 1;
                    p.saturation += 2.0f * (a.amp + 1);
                    if (p.saturation > p.food) p.saturation = (float)p.food;
                    p.healthDirty = true;
                }
                break;
            case eff::Levitation:
            case eff::SlowFalling:   // no fall builds up (rising, or falling slowly)
                p.e.fallDistance = 0;
                break;
            default:
                break;
        }
        if (p.dead) return;
        if (a.ticks > 0 && --a.ticks == 0) {
            a.ticks = 1;   // still there for removeEffect
            removeEffect(p, id);
        }
    }
}

// FoodProperties' consume effects (1.21), with their chances.
void Server::foodEffects(Player& p, uint16_t item) {
    Rng& r = mobs::rng();
    switch (item) {
        case itm::GoldenApple:
            addEffect(p, eff::Regeneration, 1, 100);
            addEffect(p, eff::Absorption, 0, 2400);
            break;
        case itm::EnchantedGoldenApple:
            addEffect(p, eff::Regeneration, 1, 400);
            addEffect(p, eff::Resistance, 0, 6000);
            addEffect(p, eff::FireResistance, 0, 6000);
            addEffect(p, eff::Absorption, 3, 2400);
            break;
        case itm::RottenFlesh:
            if (r.unit() < 0.8f) addEffect(p, eff::Hunger, 0, 600);
            break;
        case itm::Chicken:
            if (r.unit() < 0.3f) addEffect(p, eff::Hunger, 0, 600);
            break;
        case itm::SpiderEye:
            addEffect(p, eff::Poison, 0, 100);
            break;
        case itm::PoisonousPotato:
            if (r.unit() < 0.6f) addEffect(p, eff::Poison, 0, 100);
            break;
        case itm::Pufferfish:
            addEffect(p, eff::Poison, 1, 1200);
            addEffect(p, eff::Hunger, 2, 300);
            addEffect(p, eff::Nausea, 0, 300);
            break;
        case itm::HoneyBottle:
            removeEffect(p, eff::Poison);
            break;
        default:
            break;
    }
}

}  // namespace mc
