// Status effects on a player without a client: stacking, ticking, damage, food and milk.
#include <math.h>
#include "testing.h"
#include "mc/registry.h"
#include "mc/server/server.h"

using namespace mc;

struct SinkConn : Conn {   // a client that takes everything
    int read(uint8_t*, size_t) override { return 0; }
    int write(const uint8_t*, size_t n) override { return (int)n; }
    bool connected() override { return true; }
    void close() override {}
};

static Server* effectServer() {
    static Server* s = nullptr;
    if (s) return s;
    s = new Server();
    ServerConfig cfg;
    cfg.port = 0;
    cfg.worldType = WORLD_FLAT;
    cfg.spawnMobs = false;
    cfg.seed = 7;
    if (!s->begin(cfg, nullptr)) return nullptr;
    return s;
}

// A survival player in play (no client: what it would be sent goes nowhere).
static Player& freshPlayer(Server& s) {
    Player& p = s.players[2];
    if (!p.inPlay()) {
        p.conn.attach(new SinkConn);
        p.e.id = s.newEntityId();
    }
    p.state = CS_PLAY;
    p.gamemode = GM_SURVIVAL;
    p.dead = false;
    p.e.health = 20;
    p.e.invuln = 0;
    p.food = 20;
    p.saturation = 5;
    for (int i = 0; i < 46; i++) p.inv[i].clear();
    s.clearEffects(p, false);
    return p;
}

static void tickEffects(Server& s, Player& p, int n) {
    for (int i = 0; i < n; i++) {
        p.e.invuln = 0;
        s.tickEffects(p);
    }
}

TEST(effects_stack_as_vanilla) {
    Server* s = effectServer();
    CHECK(s);
    if (!s) return;
    Player& p = freshPlayer(*s);
    CHECK(s->addEffect(p, eff::Speed, 0, 100));
    CHECK_EQ(p.effect(eff::Speed), 1);
    CHECK(!s->addEffect(p, eff::Speed, 0, 50));    // shorter: kept
    CHECK(s->addEffect(p, eff::Speed, 0, 200));    // longer: replaces
    CHECK_EQ(p.effects[eff::Speed].ticks, 200);
    CHECK(s->addEffect(p, eff::Speed, 1, 10));     // stronger: replaces, even shorter
    CHECK_EQ(p.effect(eff::Speed), 2);
    CHECK(!s->addEffect(p, eff::Speed, 0, 9999));  // weaker: kept
    tickEffects(*s, p, 10);
    CHECK_EQ(p.effect(eff::Speed), 0);             // ran out
    CHECK(s->addEffect(p, eff::Invisibility, 0, -1));   // infinite
    CHECK(p.e.flags & EF_INVISIBLE);
    tickEffects(*s, p, 1000);
    CHECK_EQ(p.effect(eff::Invisibility), 1);
    s->removeEffect(p, eff::Invisibility);
    CHECK(!(p.e.flags & EF_INVISIBLE));
}

TEST(effects_heal_and_hurt_over_time) {
    Server* s = effectServer();
    CHECK(s);
    if (!s) return;
    Player& p = freshPlayer(*s);
    p.e.health = 10;
    s->addEffect(p, eff::Regeneration, 0, 100);   // every 50 ticks
    tickEffects(*s, p, 100);
    CHECK(p.e.health == 12);
    p.e.health = 3;
    s->addEffect(p, eff::Poison, 0, 200);   // every 25 ticks, never below 1
    tickEffects(*s, p, 200);
    CHECK(p.e.health == 1);
    CHECK(!p.dead);
    s->addEffect(p, eff::Wither, 0, 100);   // can kill
    tickEffects(*s, p, 100);
    CHECK(p.dead);
    s->respawnPlayer(p);
    CHECK_EQ(p.effect(eff::Wither), 0);   // death took the effects
    // instant effects act at once
    freshPlayer(*s);
    p.e.health = 10;
    s->addEffect(p, eff::InstantHealth, 1, 1);   // healing II: 8
    CHECK(p.e.health == 18);
    s->addEffect(p, eff::InstantDamage, 0, 1);   // harming I: 6
    CHECK(p.e.health == 12);
    CHECK_EQ(p.effect(eff::InstantDamage), 0);
}

TEST(effects_against_damage) {
    Server* s = effectServer();
    CHECK(s);
    if (!s) return;
    Player& p = freshPlayer(*s);
    s->addEffect(p, eff::Absorption, 0, 600);
    CHECK(p.absorption == 4);
    s->damagePlayer(p, 6, DC_MAGIC, -1);
    CHECK(p.absorption == 0);
    CHECK(p.e.health == 18);
    p.e.invuln = 0;
    s->addEffect(p, eff::Resistance, 1, 600);   // 40% less
    s->damagePlayer(p, 10, DC_MAGIC, -1);
    CHECK(fabsf(p.e.health - 12) < 1e-4f);
    p.e.invuln = 0;
    s->addEffect(p, eff::FireResistance, 0, 600);
    s->damagePlayer(p, 4, DC_LAVA, -1);
    s->damagePlayer(p, 4, DC_FIRE, -1);
    CHECK(fabsf(p.e.health - 12) < 1e-4f);
    s->clearEffects(p);
    CHECK_EQ(p.effect(eff::Resistance), 0);
    CHECK_EQ(p.effect(eff::FireResistance), 0);
}

TEST(effects_from_food_and_milk) {
    Server* s = effectServer();
    CHECK(s);
    if (!s) return;
    Player& p = freshPlayer(*s);
    s->foodEffects(p, itm::GoldenApple);
    CHECK_EQ(p.effect(eff::Regeneration), 2);
    CHECK_EQ(p.effect(eff::Absorption), 1);
    s->foodEffects(p, itm::EnchantedGoldenApple);
    CHECK_EQ(p.effect(eff::Absorption), 4);
    CHECK(p.absorption == 16);
    CHECK_EQ(p.effect(eff::FireResistance), 1);
    s->foodEffects(p, itm::SpiderEye);
    CHECK_EQ(p.effect(eff::Poison), 1);
    s->foodEffects(p, itm::HoneyBottle);   // cures poison
    CHECK_EQ(p.effect(eff::Poison), 0);
    // a milk bucket drunk: every effect gone, an empty bucket left
    p.held = 0;
    p.inv[SLOT_HOTBAR_START] = ItemStack::of(itm::MilkBucket);
    p.usingHand = 0;
    s->finishUsingItem(p);
    CHECK_EQ(p.effect(eff::Regeneration), 0);
    CHECK_EQ(p.effect(eff::Absorption), 0);
    CHECK(p.absorption == 0);
    CHECK_EQ(p.inv[SLOT_HOTBAR_START].id, itm::Bucket);
}
