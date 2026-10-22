// Potions without a client: items and their component, drinking, splash and lingering
// potions, /give with a potion.
#include <math.h>
#include <string.h>
#include "testing.h"
#include "mc/bytebuf.h"
#include "mc/registry.h"
#include "mc/server/server.h"

using namespace mc;

namespace {
struct SinkConn : Conn {
    int read(uint8_t*, size_t) override { return 0; }
    int write(const uint8_t*, size_t n) override { return (int)n; }
    bool connected() override { return true; }
    void close() override {}
};
}

static Server* potionServer() {
    static Server* s = nullptr;
    if (s) return s;
    s = new Server();
    ServerConfig cfg;
    cfg.port = 0;
    cfg.worldType = WORLD_FLAT;
    cfg.spawnMobs = false;
    cfg.seed = 9;
    if (!s->begin(cfg, nullptr)) return nullptr;
    for (int cx = -2; cx <= 2; cx++)
        for (int cz = -2; cz <= 2; cz++) s->world.load(DIM_OVERWORLD, cx, cz);
    return s;
}

static Player& potionPlayer(Server& s) {
    Player& p = s.players[3];
    if (!p.inPlay()) {
        p.conn.attach(new SinkConn);
        p.e.id = s.newEntityId();
        snprintf(p.name, sizeof(p.name), "Brewer");
    }
    p.state = CS_PLAY;
    p.gamemode = GM_SURVIVAL;
    p.dead = false;
    p.e.dim = DIM_OVERWORLD;
    p.e.x = 0.5; p.e.z = 0.5;
    p.e.y = s.world.heightAt(DIM_OVERWORLD, 0, 0);
    p.e.health = 20;
    p.e.invuln = 0;
    for (int i = 0; i < 46; i++) p.inv[i].clear();
    s.clearEffects(p, false);
    return p;
}

TEST(potion_items_and_their_component) {
    ItemStack h = potionItem(itm::Potion, potion::StrongHealing);
    CHECK_EQ(potionOf(h), (int)potion::StrongHealing);
    CHECK_EQ(potionOf(ItemStack::of(itm::Potion)), -1);
    // from the client (the creative inventory: each component length-prefixed)
    ByteBuf comp;
    Writer cw(comp);
    cw.boolean(true);
    cw.varint(potion::StrongHealing);
    cw.boolean(false);
    cw.varint(0);
    cw.boolean(false);
    ByteBuf buf;
    Writer w(buf);
    w.varint(1);
    w.varint(itm::Potion);
    w.varint(1);   // one component added
    w.varint(0);
    w.varint(comp::PotionContents);
    w.varint((int32_t)comp.size());
    w.bytes(comp.data(), comp.size());
    Reader r(buf.data(), buf.size());
    ItemStack back;
    CHECK(readSlot(r, back));
    CHECK_EQ(back.id, itm::Potion);
    CHECK_EQ(potionOf(back), (int)potion::StrongHealing);
    CHECK(back.sameItem(h));
    // colours: water's blue, swiftness's light blue, the turtle master's mix
    CHECK_EQ(potionColor(potion::Water), 0x385DC6u);
    CHECK_EQ(potionColor(potion::Swiftness), 0x33EBFFu);
    CHECK(potionColor(potion::TurtleMaster) != potionColor(potion::Slowness));
}

TEST(potion_drunk) {
    Server* s = potionServer();
    CHECK(s);
    if (!s) return;
    Player& p = potionPlayer(*s);
    p.e.health = 6;
    p.held = 0;
    p.inv[SLOT_HOTBAR_START] = potionItem(itm::Potion, potion::StrongHealing);
    p.usingHand = 0;
    s->finishUsingItem(p);
    CHECK(p.e.health == 14);   // healing II: 8
    CHECK_EQ(p.inv[SLOT_HOTBAR_START].id, itm::GlassBottle);
    p.inv[SLOT_HOTBAR_START] = potionItem(itm::Potion, potion::LongSwiftness);
    s->finishUsingItem(p);
    CHECK_EQ(p.effect(eff::Speed), 1);
    CHECK_EQ(p.effects[eff::Speed].ticks, 9600);
    p.inv[SLOT_HOTBAR_START] = potionItem(itm::Potion, potion::TurtleMaster);
    s->finishUsingItem(p);
    CHECK_EQ(p.effect(eff::Slowness), 4);
    CHECK_EQ(p.effect(eff::Resistance), 3);
}

TEST(potion_splash_by_distance) {
    Server* s = potionServer();
    CHECK(s);
    if (!s) return;
    Player& p = potionPlayer(*s);
    Server::InDim in(*s, DIM_OVERWORLD);
    double y = p.e.y;
    Entity* pig = s->spawnMob(ent::Pig, 3.5, y, 0.5);
    Entity* zombie = s->spawnMob(ent::Zombie, -2.5, y, 0.5);
    CHECK(pig && zombie);
    if (!pig || !zombie) return;
    zombie->health = 10;
    // harming II thrown at the player's feet: the player takes it all, the pig 3 blocks away
    // a quarter, the zombie 3 blocks away is healed
    Entity* thrown = s->spawnEntity(EK_POTION, ent::SplashPotion, 0.5, y, 0.5);
    CHECK(thrown);
    if (!thrown) return;
    thrown->item = potionItem(itm::SplashPotion, potion::StrongHarming);
    s->potionBreaks(*thrown, &p.e);
    s->removeEntity(*thrown);
    CHECK(p.e.health == 8);       // 12
    CHECK(pig->health == 7);      // 12 x 0.25
    CHECK(zombie->health == 12);  // healed by 8 x 0.25 (undead)
    // regeneration 2 blocks away: half the time
    p.e.health = 20;
    thrown = s->spawnEntity(EK_POTION, ent::SplashPotion, 2.5, y, 0.5);
    thrown->item = potionItem(itm::SplashPotion, potion::Regeneration);
    s->potionBreaks(*thrown, nullptr);
    s->removeEntity(*thrown);
    CHECK_EQ(p.effect(eff::Regeneration), 1);
    CHECK_EQ(p.effects[eff::Regeneration].ticks, 450);
    s->removeEntity(*pig);
    s->removeEntity(*zombie);
}

TEST(potion_lingering_cloud) {
    Server* s = potionServer();
    CHECK(s);
    if (!s) return;
    Player& p = potionPlayer(*s);
    Server::InDim in(*s, DIM_OVERWORLD);
    Entity* c = s->potionCloud(1.5, p.e.y, 0.5, potion::Poison, -1);
    CHECK(c);
    if (!c) return;
    for (int t = 0; t < 9; t++) { c->age++; s->tickCloud(*c); }
    CHECK_EQ(p.effect(eff::Poison), 0);   // still waiting
    c->age++;
    s->tickCloud(*c);                      // the first use: a quarter of the time
    CHECK_EQ(p.effect(eff::Poison), 1);
    CHECK_EQ(p.effects[eff::Poison].ticks, 225);
    CHECK(c->damage < 2.6f && c->damage > 2.4f);   // 3, less 0.5 for the use
    // it runs out: smaller each tick, gone within 30 s
    int t = 0;
    while (!c->removed && t < 700) { c->age++; s->tickCloud(*c); t++; }
    CHECK(c->removed);
}

TEST(potion_given_by_command) {
    Server* s = potionServer();
    CHECK(s);
    if (!s) return;
    Player& p = potionPlayer(*s);
    s->runCommand(nullptr, "give Brewer splash_potion[potion_contents={potion:\"minecraft:long_fire_resistance\"}] 2");
    int found = 0;
    for (int i = 0; i < 46; i++)
        if (p.inv[i].id == itm::SplashPotion && potionOf(p.inv[i]) == potion::LongFireResistance) found += p.inv[i].count;
    CHECK_EQ(found, 2);
}
