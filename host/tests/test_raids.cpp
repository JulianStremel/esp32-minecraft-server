// Patrols and raids without a client: the ominous bottle and Bad Omen, Raid Omen in a
// village and the raid it starts, its waves, victory (Hero of the Village and its prices)
// and loss, a patrol and its captain's bottle, witches' potions, evokers' fangs.
#include "testing.h"
#include "mc/item.h"
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

Server* server() {
    static Server* s = nullptr;
    if (s) return s;
    s = new Server();
    ServerConfig cfg;
    cfg.port = 0;
    cfg.worldType = WORLD_FLAT;
    cfg.spawnMobs = false;
    cfg.seed = 66;
    if (!s->begin(cfg, nullptr)) return nullptr;
    for (int cx = -4; cx <= 4; cx++)
        for (int cz = -4; cz <= 4; cz++) s->world.load(DIM_OVERWORLD, cx, cz);
    s->cfg.difficulty = 2;
    return s;
}

Player& player(Server& s) {
    Player& p = s.players[7];
    if (!p.inPlay()) {
        p.conn.attach(new SinkConn);
        p.e.id = s.newEntityId();
    }
    p.state = CS_PLAY;
    p.gamemode = GM_SURVIVAL;
    p.dead = false;
    p.e.health = 20;
    p.e.dim = DIM_OVERWORLD;
    p.e.x = 0.5; p.e.y = s.world.heightAt(DIM_OVERWORLD, 0, 0); p.e.z = 0.5;
    for (int i = 0; i < 46; i++) p.inv[i].clear();
    s.clearEffects(p, false);
    p.winKind = WK_NONE;
    return p;
}

void clearMobs(Server& s) {
    for (int i = 0; i < MC_MAX_ENTITIES; i++)
        if (s.entities[i].kind != EK_NONE) s.entities[i].kind = EK_NONE;
    for (auto& r : s.raids) r = Server::Raid();
}

int count(Server& s, uint16_t type, int raid = -2) {
    int n = 0;
    for (int i = 0; i < MC_MAX_ENTITIES; i++) {
        const Entity& e = s.entities[i];
        n += e.kind == EK_MOB && !e.removed && e.health > 0 && e.type == type && (raid == -2 || e.raid == raid);
    }
    return n;
}

void killRaiders(Server& s) {
    for (int i = 0; i < MC_MAX_ENTITIES; i++) {
        Entity& e = s.entities[i];
        if (e.kind == EK_MOB && e.raid >= 0) e.kind = EK_NONE;
    }
}

}  // namespace

TEST(ominous_bottle_and_bad_omen) {
    Server* s = server();
    CHECK(s);
    if (!s) return;
    ItemStack b = ominousBottle(3);
    CHECK_EQ(ominousAmplifier(b), 3);
    ByteBuf buf;   // the client sees it as the ominous_bottle_amplifier component and sends it back
    Writer w(buf);
    writeSlot(w, b);
    Reader r(buf.data(), buf.size());
    CHECK_EQ(r.varint(), 1);
    CHECK_EQ(r.varint(), (int)itm::OminousBottle);
    CHECK_EQ(r.varint(), 1);
    r.varint();
    CHECK_EQ(r.varint(), (int)comp::OminousBottleAmplifier);
    CHECK_EQ(r.varint(), 3);
    Player& p = player(*s);
    p.inv[SLOT_HOTBAR_START] = b;
    s->drinkOminousBottle(p, p.inv[SLOT_HOTBAR_START], SLOT_HOTBAR_START);
    CHECK_EQ(p.effect(eff::BadOmen), 4);
    CHECK(p.inv[SLOT_HOTBAR_START].empty());
}

TEST(a_raid_from_bad_omen_to_victory) {
    Server* s = server();
    CHECK(s);
    if (!s) return;
    Server::InDim in(*s, DIM_OVERWORLD);
    clearMobs(*s);
    Player& p = player(*s);
    int y = (int)p.e.y;
    s->setBlock(3, y, 3, bs::Bell);   // a village
    for (int i = 0; i < 3; i++) s->spawnMob(ent::Villager, 2.5 + i, y, 5.5);
    s->addEffect(p, eff::BadOmen, 0, 120000);
    s->ticks = (s->ticks / 20 + 1) * 20;
    s->tickOmens(p);
    CHECK_EQ(p.effect(eff::BadOmen), 0);
    CHECK_EQ(p.effect(eff::RaidOmen), 1);
    // 30 s later the raid begins
    for (int i = 0; i < 600; i++) s->tickEffects(p);
    CHECK(s->raids[0].active);
    CHECK_EQ(s->raids[0].waves, 5);   // normal
    // the first wave after 15 s: pillagers (4, and 0 or 1 more on normal), one the captain
    for (int i = 0; i < 300; i++) s->tickRaids();
    CHECK_EQ(s->raids[0].wave, 1);
    int pillagers = count(*s, ent::Pillager, 0);
    CHECK(pillagers >= 4 && pillagers <= 5);
    int captains = 0;
    for (int i = 0; i < MC_MAX_ENTITIES; i++) captains += s->entities[i].kind == EK_MOB && s->entities[i].captain;
    CHECK_EQ(captains, 1);
    // all waves beaten: Hero of the Village
    for (int wave = 1; wave <= 5; wave++) {
        CHECK_EQ(s->raids[0].wave, wave);
        if (wave == 3) CHECK_EQ(count(*s, ent::Ravager, 0), 1);
        killRaiders(*s);
        for (int i = 0; i < 301; i++) s->tickRaids();
    }
    CHECK_EQ(s->raids[0].status, Server::Raid::VICTORY);
    CHECK_EQ(p.effect(eff::HeroOfTheVillage), 1);
    // cheaper: 30% of the price off
    Entity* v = nullptr;
    for (int i = 0; i < MC_MAX_ENTITIES; i++)
        if (s->entities[i].kind == EK_MOB && s->entities[i].type == ent::Villager) v = &s->entities[i];
    CHECK(v);
    if (v) {
        TradeOffer o;
        o.buyA = ItemStack::of(itm::Emerald, 20);
        o.sell = ItemStack::of(itm::Bookshelf);
        v->villager.trades.push_back(o);
        s->specialPrices(*v, &p);
        CHECK_EQ(tradeCostA(v->villager.trades.back()), 14);
        s->specialPrices(*v, nullptr);
    }
    for (int i = 0; i < 700; i++) s->tickRaids();
    CHECK(!s->raids[0].active);
    s->setBlock(3, y, 3, 0);
    s->clearEffects(p, false);
    clearMobs(*s);
}

TEST(a_raid_lost_with_no_villagers_left) {
    Server* s = server();
    CHECK(s);
    if (!s) return;
    Server::InDim in(*s, DIM_OVERWORLD);
    clearMobs(*s);
    Player& p = player(*s);
    p.raidX = 0; p.raidY = (int)p.e.y; p.raidZ = 0;
    s->spawnMob(ent::Villager, 2.5, p.e.y, 2.5);
    s->raidOmenEnded(p, 0);
    CHECK(s->raids[0].active);
    for (int i = 0; i < 300; i++) s->tickRaids();
    CHECK_EQ(s->raids[0].wave, 1);
    for (int i = 0; i < MC_MAX_ENTITIES; i++)
        if (s->entities[i].type == ent::Villager) s->entities[i].kind = EK_NONE;
    for (int i = 0; i < 40; i++) s->tickRaids();
    CHECK_EQ(s->raids[0].status, Server::Raid::LOSS);
    clearMobs(*s);
}

TEST(patrol_and_its_captains_bottle) {
    Server* s = server();
    CHECK(s);
    if (!s) return;
    Server::InDim in(*s, DIM_OVERWORLD);
    clearMobs(*s);
    Player& p = player(*s);
    CHECK(s->spawnPatrol(&p, true));
    int n = count(*s, ent::Pillager);
    CHECK(n >= 1 && n <= 3);   // normal: difficulty + 1
    Entity* captain = nullptr;
    for (int i = 0; i < MC_MAX_ENTITIES; i++) {
        Entity& e = s->entities[i];
        if (e.kind == EK_MOB && e.type == ent::Pillager) {
            CHECK(e.leader >= 0);
            if (e.captain) captain = &e;
        }
    }
    CHECK(captain);
    if (!captain) return;
    double gx, gy, gz, arrive;
    CHECK(s->raiderGoal(*captain, gx, gy, gz, arrive));   // somewhere far
    CHECK(fabs(gx - captain->x) + fabs(gz - captain->z) > 100);
    s->damageEntity(*captain, 100, DC_ATTACK, p.e.id);
    bool bottle = false;
    for (int i = 0; i < MC_MAX_ENTITIES; i++)
        bottle |= s->entities[i].kind == EK_ITEM && !s->entities[i].removed && s->entities[i].item.id == itm::OminousBottle;
    CHECK(bottle);
    clearMobs(*s);
}

TEST(witches_throw_and_evokers_bite) {
    Server* s = server();
    CHECK(s);
    if (!s) return;
    Server::InDim in(*s, DIM_OVERWORLD);
    clearMobs(*s);
    int y = s->world.heightAt(DIM_OVERWORLD, 10, 10);
    Entity* w = s->spawnMob(ent::Witch, 10.5, y, 10.5);
    Entity* v = s->spawnMob(ent::Villager, 16.5, y, 10.5);
    CHECK(w && v);
    if (!w || !v) return;
    s->witchPotion(*w, *v);
    bool potion = false;
    for (int i = 0; i < MC_MAX_ENTITIES; i++)
        potion |= s->entities[i].kind == EK_POTION && !s->entities[i].removed && s->entities[i].owner == w->id;
    CHECK(potion);
    Entity* ev = s->spawnMob(ent::Evoker, 10.5, y, 12.5);
    CHECK(ev);
    if (!ev) return;
    float before = v->health;
    v->x = 13.0; v->z = 12.5;   // in the line
    s->evokerFangs(*ev, *v);
    for (int t = 0; t < 40; t++)
        for (int i = 0; i < MC_MAX_ENTITIES; i++)
            if (s->entities[i].kind == EK_FANGS && !s->entities[i].removed) s->tickFangs(s->entities[i]);
    CHECK(v->health < before);
    clearMobs(*s);
}
