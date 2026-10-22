// A village's friends and foes without a client: zombies turning villagers, curing a
// zombie villager and the cheaper prices after, villagers' memory of a player who hurts
// them and the golems it sets on that player, golems built and coming to villages, and a
// zombie villager with its cure and gossip saved with its chunk.
#include "testing.h"
#include "mc/item.h"
#include "mc/registry.h"
#include "mc/server/server.h"
#include "mc/storage/block_device.h"
#include "mc/storage/world_store.h"

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
    cfg.seed = 33;
    if (!s->begin(cfg, nullptr)) return nullptr;
    for (int cx = 0; cx < 3; cx++)
        for (int cz = 0; cz < 3; cz++) s->world.load(DIM_OVERWORLD, cx, cz);
    return s;
}

Player& player(Server& s) {
    Player& p = s.players[5];
    if (!p.inPlay()) {
        p.conn.attach(new SinkConn);
        p.e.id = s.newEntityId();
        for (int i = 0; i < 16; i++) p.uuid[i] = (uint8_t)(0x40 + i);
    }
    p.state = CS_PLAY;
    p.gamemode = GM_SURVIVAL;
    p.dead = false;
    p.e.dim = DIM_OVERWORLD;
    p.e.x = 8; p.e.y = 70; p.e.z = 4;
    p.held = 0;
    for (int i = 0; i < 46; i++) p.inv[i].clear();
    return p;
}

Entity* librarian(Server& s, double x, double z) {
    Entity* v = s.spawnMob(ent::Villager, x, 70, z);
    if (!v) return nullptr;
    v->villager.profession = 9;
    v->villager.xp = 5;
    v->villager.trades.clear();
    TradeOffer o;
    o.buyA = ItemStack::of(itm::Emerald, 20);
    o.sell = ItemStack::of(itm::Bookshelf);
    o.priceMult = 0.05f;
    v->villager.trades.push_back(o);
    return v;
}

void clearMobs(Server& s) {
    for (int i = 0; i < MC_MAX_ENTITIES; i++)
        if (s.entities[i].kind == EK_MOB) s.entities[i].kind = EK_NONE;
}

int count(Server& s, uint16_t type) {
    int n = 0;
    for (int i = 0; i < MC_MAX_ENTITIES; i++)
        n += s.entities[i].kind == EK_MOB && !s.entities[i].removed && s.entities[i].type == type;
    return n;
}

Entity* first(Server& s, uint16_t type) {
    for (int i = 0; i < MC_MAX_ENTITIES; i++)
        if (s.entities[i].kind == EK_MOB && !s.entities[i].removed && s.entities[i].type == type) return &s.entities[i];
    return nullptr;
}

}  // namespace

TEST(zombies_turn_villagers_on_hard) {
    Server* s = server();
    CHECK(s);
    if (!s) return;
    Server::InDim in(*s, DIM_OVERWORLD);
    clearMobs(*s);
    s->cfg.difficulty = 3;
    Entity* v = librarian(*s, 8.5, 8.5);
    Entity* z = s->spawnMob(ent::Zombie, 10.5, 70, 8.5);
    CHECK(v && z);
    if (!v || !z) return;
    z->age = (uint32_t)(20 - (uint32_t)z->id % 20);   // (prey is looked for every 20 ticks)
    Entity* prey = s->preyOf(*z);
    CHECK(prey && prey->id == v->id);
    s->damageEntity(*v, 100, DC_ATTACK, z->id);
    CHECK(v->removed);
    Entity* zv = first(*s, ent::ZombieVillager);
    CHECK(zv);
    if (zv) {
        CHECK_EQ(zv->villager.profession, 9);
        CHECK_EQ((int)zv->villager.trades.size(), 1);
        CHECK(zv->persistent);
    }
    // on easy a villager only dies
    s->cfg.difficulty = 1;
    Entity* w = librarian(*s, 6.5, 6.5);
    CHECK(w);
    if (w) {
        s->damageEntity(*w, 100, DC_ATTACK, z->id);
        CHECK(w->health <= 0);
        CHECK(!w->removed);   // (its body lies there until ET_CORPSE)
    }
    s->cfg.difficulty = 2;
    clearMobs(*s);
}

TEST(curing_a_zombie_villager) {
    Server* s = server();
    CHECK(s);
    if (!s) return;
    Server::InDim in(*s, DIM_OVERWORLD);
    clearMobs(*s);
    Entity* zv = s->spawnMob(ent::ZombieVillager, 8.5, 70, 8.5);
    CHECK(zv);
    if (!zv) return;
    zv->villager.profession = 9;
    TradeOffer o;
    o.buyA = ItemStack::of(itm::Emerald, 20);
    o.sell = ItemStack::of(itm::Bookshelf);
    zv->villager.trades.push_back(o);
    Player& p = player(*s);
    p.inv[SLOT_HOTBAR_START] = ItemStack::of(itm::GoldenApple, 2);
    CHECK(!s->cureZombieVillager(p, *zv));   // not weakened
    s->potionOnMob(*zv, potion::Weakness, 1.0, -1);
    CHECK(zv->weakTicks > 0);
    CHECK(s->cureZombieVillager(p, *zv));
    CHECK_EQ(p.inv[SLOT_HOTBAR_START].count, 1);
    CHECK(zv->villager.convertTicks >= 3600 && zv->villager.convertTicks <= 6000);
    int ticks = zv->villager.convertTicks;
    for (int i = 0; i < ticks; i++) s->tickZombieVillager(*zv);
    CHECK(zv->removed);
    Entity* v = first(*s, ent::Villager);
    CHECK(v);
    if (!v) return;
    CHECK_EQ(v->villager.profession, 9);
    // the curer: reputation 20 * 5 + 25 = 125, so 6 emeralds off a trade of 0.05
    s->specialPrices(*v, &p);
    CHECK_EQ(v->villager.trades[0].special, -6);
    CHECK_EQ(tradeCostA(v->villager.trades[0]), 14);
    s->specialPrices(*v, nullptr);
    CHECK_EQ(tradeCostA(v->villager.trades[0]), 20);
    clearMobs(*s);
}

TEST(hurt_villagers_set_golems_on_a_player) {
    Server* s = server();
    CHECK(s);
    if (!s) return;
    Server::InDim in(*s, DIM_OVERWORLD);
    clearMobs(*s);
    Entity* v = librarian(*s, 8.5, 8.5);
    Entity* g = s->spawnMob(ent::IronGolem, 12.5, 70, 8.5);
    CHECK(v && g);
    if (!v || !g) return;
    CHECK_EQ(g->maxHealth, 100);
    Player& p = player(*s);
    for (int i = 0; i < 3; i++) {
        v->invuln = 0;
        s->damageEntity(*v, 1, DC_ATTACK, p.e.id);
    }
    CHECK_EQ(g->angryAt, -1);   // -75: not yet
    v->invuln = 0;
    s->damageEntity(*v, 1, DC_ATTACK, p.e.id);
    CHECK_EQ(g->angryAt, p.e.id);   // -100
    // the prices went up for that player: 100 reputation lost, 5 emeralds more
    s->specialPrices(*v, &p);
    CHECK_EQ(tradeCostA(v->villager.trades[0]), 25);
    s->specialPrices(*v, nullptr);
    // a golem goes for monsters near it, not for creepers
    s->spawnMob(ent::Creeper, 14.5, 70, 8.5);
    g->prey = -1;
    g->age = (uint32_t)(20 - (uint32_t)g->id % 20);
    CHECK(s->preyOf(*g) == nullptr);
    Entity* z = s->spawnMob(ent::Zombie, 15.5, 70, 9.5);
    g->age += 20;
    Entity* prey = s->preyOf(*g);
    CHECK(prey && z && prey->id == z->id);
    // golems take no knockback
    g->vx = g->vz = 0;
    s->knockback(*g, 1.0, 1, 0);
    CHECK(g->vx == 0 && g->vz == 0);
    clearMobs(*s);
}

TEST(iron_golems_built_and_coming_to_villages) {
    Server* s = server();
    CHECK(s);
    if (!s) return;
    Server::InDim in(*s, DIM_OVERWORLD);
    clearMobs(*s);
    // a T of iron blocks along x, the pumpkin on top
    s->setBlock(20, 70, 20, bs::IronBlock);
    s->setBlock(20, 71, 20, bs::IronBlock);
    s->setBlock(19, 71, 20, bs::IronBlock);
    s->setBlock(21, 71, 20, bs::IronBlock);
    s->setBlock(20, 72, 20, bs::CarvedPumpkin);
    CHECK(s->buildGolem(20, 72, 20));
    CHECK(stateIsAir(s->blockAt(20, 71, 20)) && stateIsAir(s->blockAt(19, 71, 20)) && stateIsAir(s->blockAt(20, 72, 20)));
    CHECK_EQ(count(*s, ent::IronGolem), 1);
    Entity* made = first(*s, ent::IronGolem);
    CHECK(made && (made->variant & 1));
    // a pumpkin on a wrong shape: nothing
    s->setBlock(24, 70, 20, bs::IronBlock);
    s->setBlock(24, 71, 20, bs::IronBlock);
    s->setBlock(24, 72, 20, bs::CarvedPumpkin);
    CHECK(!s->buildGolem(24, 72, 20));
    clearMobs(*s);
    // five villagers close together and no golem: one comes (onto a stone floor)
    for (int x = 20; x < 46; x++)
        for (int z = 20; z < 46; z++) s->setBlock(x, 69, z, bs::Stone);
    Entity* v0 = nullptr;
    for (int i = 0; i < 5; i++) {
        Entity* v = s->spawnMob(ent::Villager, 30.5 + i, 70, 30.5);
        if (!v0) v0 = v;
    }
    CHECK(v0);
    if (!v0) return;
    v0->age = (uint32_t)((600 - ((uint32_t)v0->id * 13u) % 600) % 600);
    s->villageGolem(*v0);
    CHECK_EQ(count(*s, ent::IronGolem), 1);
    v0->age += 600;
    s->villageGolem(*v0);   // one is enough
    CHECK_EQ(count(*s, ent::IronGolem), 1);
    clearMobs(*s);
}

TEST(zombie_villager_and_gossip_saved_with_the_chunk) {
    MemDevice dev(32 * 1024 * 1024);
    WorldStore store(&dev);
    StoreParams params;
    params.radius = 1;
    params.playerSlots = 4;
    CHECK(store.open(params));
    Chunk c(0, 0);
    SavedEntity e;
    e.kind = SAVED_KIND_MOB;
    e.type = ent::ZombieVillager;
    e.x = 3.5; e.y = 70; e.z = 3.5;
    e.health = 20;
    e.villager.profession = 5;
    e.villager.convertTicks = 1234;
    e.villager.convertBy = 0x1122334455667788ull;
    e.villager.gossip[0].who = 42;
    e.villager.gossip[0].majorPos = 20;
    e.villager.gossip[0].minorNeg = 50;
    e.villager.gossipDay = 9;
    CHECK(c.addEntity(e));
    CHECK(store.saveChunk(c));
    Chunk loaded(0, 0);
    CHECK_EQ(store.loadChunk(loaded), LOAD_OK);
    CHECK_EQ(loaded.entCount, 1);
    if (loaded.entCount != 1) return;
    const VillagerState& v = loaded.ents[0].villager;
    CHECK_EQ(v.profession, 5);
    CHECK_EQ(v.convertTicks, 1234);
    CHECK(v.convertBy == 0x1122334455667788ull);
    CHECK_EQ(v.reputation(42), 100 - 50);
    CHECK_EQ(v.gossipDay, 9u);
}

TEST(a_stronger_hit_lands_within_the_immunity) {
    Server* s = server();
    CHECK(s);
    if (!s) return;
    Server::InDim in(*s, DIM_OVERWORLD);
    clearMobs(*s);
    Entity* z = s->spawnMob(ent::Zombie, 8.5, 70, 8.5);
    CHECK(z);
    if (!z) return;
    CHECK(s->damageEntity(*z, 1, DC_FIRE, -1));        // burning: 1, then 10 ticks of immunity
    CHECK(!s->damageEntity(*z, 1, DC_ATTACK, -1));     // no more than that: nothing
    CHECK_EQ((int)z->health, 19);
    s->damageEntity(*z, 10, DC_ATTACK, -1);            // more: the difference
    CHECK_EQ((int)z->health, 10);
    CHECK(!s->damageEntity(*z, 5, DC_ATTACK, -1));     // less than the last: nothing
    CHECK_EQ((int)z->health, 10);
    clearMobs(*s);
}
