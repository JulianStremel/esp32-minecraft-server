// The wandering trader without a client: its six trades, coming near a player towards
// the bell, trading without levels, leaving when its time is up, the daily chance, and
// saved with its chunk.
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
    cfg.seed = 55;
    if (!s->begin(cfg, nullptr)) return nullptr;
    for (int cx = -4; cx <= 4; cx++)
        for (int cz = -4; cz <= 4; cz++) s->world.load(DIM_OVERWORLD, cx, cz);
    return s;
}

Player& player(Server& s) {
    Player& p = s.players[6];
    if (!p.inPlay()) {
        p.conn.attach(new SinkConn);
        p.e.id = s.newEntityId();
    }
    p.state = CS_PLAY;
    p.gamemode = GM_SURVIVAL;
    p.dead = false;
    p.e.dim = DIM_OVERWORLD;
    p.e.x = 0.5; p.e.y = s.world.heightAt(DIM_OVERWORLD, 0, 0); p.e.z = 0.5;
    for (int i = 0; i < 46; i++) p.inv[i].clear();
    p.winKind = WK_NONE;
    return p;
}

Entity* trader(Server& s) {
    for (int i = 0; i < MC_MAX_ENTITIES; i++)
        if (s.entities[i].kind == EK_MOB && !s.entities[i].removed && s.entities[i].type == ent::WanderingTrader) return &s.entities[i];
    return nullptr;
}

void clearMobs(Server& s) {
    for (int i = 0; i < MC_MAX_ENTITIES; i++)
        if (s.entities[i].kind != EK_NONE) s.entities[i].kind = EK_NONE;
}

}  // namespace

TEST(wandering_trader_comes_to_the_bell_and_trades) {
    Server* s = server();
    CHECK(s);
    if (!s) return;
    Server::InDim in(*s, DIM_OVERWORLD);
    clearMobs(*s);
    Player& p = player(*s);
    int bellY = s->world.heightAt(DIM_OVERWORLD, 20, 10);
    s->setBlock(20, bellY, 10, bs::Bell);
    CHECK(s->spawnTrader(&p));
    Entity* t = trader(*s);
    CHECK(t);
    if (!t) return;
    CHECK_EQ((int)t->villager.trades.size(), 6);
    CHECK_EQ(t->villager.despawnDelay, 48000);
    CHECK(t->villager.hasWander);
    CHECK_EQ(t->villager.wanderX, 20);
    CHECK(fabs(t->x - 20.5) <= 11 && fabs(t->z - 10.5) <= 11);   // within 10 of the bell
    // trading: no levels, experience for the player only
    TradeOffer& o = t->villager.trades[0];
    p.inv[SLOT_MAIN_START] = ItemStack::of(itm::Emerald, 64);
    s->interactVillager(p, *t);
    CHECK_EQ(p.winKind, WK_MERCHANT);
    s->selectTrade(p, 0);
    CHECK_EQ(p.craft[2].id, o.sell.id);
    int xp = p.xpTotal;
    s->merchantTaken(p);
    CHECK_EQ(o.uses, 1);
    CHECK_EQ(t->villager.xp, 0);
    CHECK(p.xpTotal > xp);
    // it does not leave while someone trades with it, and leaves after
    t->villager.despawnDelay = 1;
    s->tickTrader(*t);
    CHECK(!t->removed);
    s->closeWindow(p, true);
    s->tickTrader(*t);
    CHECK(t->removed);
    s->setBlock(20, bellY, 10, 0);
    clearMobs(*s);
}

TEST(wandering_trader_daily_chance) {
    Server* s = server();
    CHECK(s);
    if (!s) return;
    Server::InDim in(*s, DIM_OVERWORLD);
    clearMobs(*s);
    player(*s);
    s->cfg.spawnMobs = true;
    s->traderDelay = 24000;
    s->traderChance = 25;
    // a day passes in 1200-tick steps; then a chance: 25%, 50%, 75%, 75% ... until one comes
    int days = 0;
    for (; days < 40 && !trader(*s); days++)
        for (int k = 0; k < 20; k++) {
            s->ticks = (s->ticks / 1200 + 1) * 1200;
            s->tickTraderSpawner();
        }
    printf("    a wandering trader after %d days\n", days);
    CHECK(trader(*s));
    CHECK_EQ(s->traderChance, 25);   // back to 25% once one came
    s->cfg.spawnMobs = false;
    clearMobs(*s);
}

TEST(wandering_trader_saved_with_its_chunk) {
    MemDevice dev(32 * 1024 * 1024);
    WorldStore store(&dev);
    StoreParams params;
    params.radius = 1;
    params.playerSlots = 4;
    CHECK(store.open(params));
    Chunk c(0, 0);
    SavedEntity e;
    e.kind = SAVED_KIND_MOB;
    e.type = ent::WanderingTrader;
    e.x = 3.5; e.y = 70; e.z = 3.5;
    e.health = 20;
    addWanderingOffers(e.villager);
    e.villager.despawnDelay = 12345;
    e.villager.hasWander = true;
    e.villager.wanderX = 9; e.villager.wanderY = 64; e.villager.wanderZ = -9;
    CHECK(c.addEntity(e));
    CHECK(store.saveChunk(c));
    Chunk loaded(0, 0);
    CHECK_EQ(store.loadChunk(loaded), LOAD_OK);
    CHECK_EQ(loaded.entCount, 1);
    if (loaded.entCount != 1) return;
    const VillagerState& v = loaded.ents[0].villager;
    CHECK_EQ((int)v.trades.size(), 6);
    CHECK_EQ(v.despawnDelay, 12345);
    CHECK(v.hasWander);
    CHECK_EQ(v.wanderZ, -9);
}
