// Villagers without a client: a profession from a workstation, the trades of a level,
// trading (payment, uses, experience), levelling up, restocking and demand, losing the
// profession, and a villager saved with its chunk.
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

Server* villageServer() {
    static Server* s = nullptr;
    if (s) return s;
    s = new Server();
    ServerConfig cfg;
    cfg.port = 0;
    cfg.worldType = WORLD_FLAT;
    cfg.spawnMobs = false;
    cfg.seed = 21;
    if (!s->begin(cfg, nullptr)) return nullptr;
    s->world.load(DIM_OVERWORLD, 0, 0);
    return s;
}

Player& trader(Server& s) {
    Player& p = s.players[4];
    if (!p.inPlay()) {
        p.conn.attach(new SinkConn);
        p.e.id = s.newEntityId();
    }
    p.state = CS_PLAY;
    p.gamemode = GM_SURVIVAL;
    p.dead = false;
    p.e.dim = DIM_OVERWORLD;
    p.e.x = 8; p.e.y = 70; p.e.z = 6;
    for (int i = 0; i < 46; i++) p.inv[i].clear();
    p.winKind = WK_NONE;
    p.xpLevel = 0;
    p.xpTotal = 0;
    p.xpProgress = 0;
    return p;
}

// ticks the villager until its brain has looked for a job site (every 100 ticks)
void work(Server& s, Entity& v, int ticks) {
    for (int i = 0; i < ticks; i++) {
        v.age++;
        s.tickVillager(v);
    }
}

}  // namespace

TEST(villager_takes_a_profession_from_a_workstation) {
    Server* s = villageServer();
    CHECK(s);
    if (!s) return;
    Server::InDim in(*s, DIM_OVERWORLD);
    Entity* v = s->spawnMob(ent::Villager, 8.5, 70, 8.5);
    CHECK(v);
    if (!v) return;
    CHECK_EQ(v->villager.profession, 0);
    work(*s, *v, 120);
    CHECK_EQ(v->villager.profession, 0);   // nothing near: unemployed
    s->setBlock(11, 70, 8, bs::Lectern);
    work(*s, *v, 120);
    CHECK_EQ(v->villager.profession, 9);   // librarian
    CHECK(v->villager.hasJob);
    CHECK_EQ(v->villager.jobX, 11);
    CHECK_EQ((int)v->villager.trades.size(), 2);
    for (const TradeOffer& o : v->villager.trades) {   // two of paper, an enchanted book, bookshelves
        bool known = (o.buyA.id == itm::Paper && o.sell.id == itm::Emerald) ||
                     (o.sell.id == itm::EnchantedBook && o.buyB.id == itm::Book) ||
                     (o.sell.id == itm::Bookshelf && o.buyA.id == itm::Emerald && o.buyA.count == 9);
        CHECK(known);
    }
    // a second villager does not take the same lectern
    Entity* w = s->spawnMob(ent::Villager, 9.5, 70, 9.5);
    CHECK(w);
    if (w) {
        work(*s, *w, 120);
        CHECK_EQ(w->villager.profession, 0);
        s->removeEntity(*w);
    }
    // the lectern broken before any trade: the profession goes
    s->setBlock(11, 70, 8, 0);
    work(*s, *v, 60);
    CHECK_EQ(v->villager.profession, 0);
    CHECK(!v->villager.hasJob);
    CHECK(v->villager.trades.empty());
    s->removeEntity(*v);
}

TEST(trading_with_a_villager) {
    Server* s = villageServer();
    CHECK(s);
    if (!s) return;
    Server::InDim in(*s, DIM_OVERWORLD);
    Entity* v = s->spawnMob(ent::Villager, 8.5, 70, 8.5);
    CHECK(v);
    if (!v) return;
    // a farmer with a known first trade: 20 wheat for an emerald
    v->villager.profession = 5;
    v->villager.trades.clear();
    TradeOffer wheat;
    wheat.buyA = ItemStack::of(itm::Wheat, 20);
    wheat.sell = ItemStack::of(itm::Emerald);
    wheat.maxUses = 16;
    wheat.xp = 2;
    v->villager.trades.push_back(wheat);
    TradeOffer bread;
    bread.buyA = ItemStack::of(itm::Emerald, 1);
    bread.sell = ItemStack::of(itm::Bread, 6);
    bread.maxUses = 16;
    bread.xp = 1;
    v->villager.trades.push_back(bread);
    Player& p = trader(*s);
    p.inv[SLOT_MAIN_START] = ItemStack::of(itm::Wheat, 64);
    s->interactVillager(p, *v);
    CHECK_EQ(p.winKind, WK_MERCHANT);
    CHECK_EQ(v->tradingWith, p.slot);
    s->selectTrade(p, 0);   // the wheat moved into the payment slot
    CHECK_EQ(p.craft[0].id, itm::Wheat);
    CHECK_EQ(p.craft[0].count, 64);
    CHECK_EQ(p.craft[2].id, itm::Emerald);
    CHECK(s->merchantMayTake(p));
    s->merchantTaken(p);
    CHECK_EQ(p.craft[0].count, 44);
    CHECK_EQ(v->villager.trades[0].uses, 1);
    CHECK_EQ(v->villager.xp, 2);
    CHECK(p.xpTotal >= 3 && p.xpTotal <= 6);
    CHECK_EQ(p.craft[2].id, itm::Emerald);   // paid for once more
    // four more (more wheat): 10 experience, a level up waiting for the trading to end
    for (int i = 0; i < 4; i++) {
        p.craft[0].count = 64;
        s->merchantChanged(p);
        s->merchantTaken(p);
    }
    CHECK_EQ(v->villager.xp, 10);
    CHECK(v->levelUpPending);
    CHECK_EQ(v->villager.level, 1);
    work(*s, *v, 60);
    CHECK_EQ(v->villager.level, 1);   // still trading
    s->closeWindow(p, true);
    CHECK_EQ(v->tradingWith, -1);
    work(*s, *v, 41);
    CHECK_EQ(v->villager.level, 2);
    CHECK_EQ((int)v->villager.trades.size(), 4);
    // restocking: the uses back, the demand up for what sold out
    v->villager.trades[0].uses = 16;
    s->restockVillager(*v);
    CHECK_EQ(v->villager.trades[0].uses, 0);
    CHECK_EQ(v->villager.trades[0].demand, 16);
    CHECK_EQ(tradeCostA(v->villager.trades[0]), 20 + 16);   // 20 + floor(20 * 16 * 0.05)
    CHECK_EQ(v->villager.trades[1].demand, -16);
    CHECK_EQ(tradeCostA(v->villager.trades[1]), 1);
    s->removeEntity(*v);
}

TEST(villager_type_from_its_biome) {
    Server* s = villageServer();
    CHECK(s);
    if (!s) return;
    Server::InDim in(*s, DIM_OVERWORLD);
    Chunk* c = s->world.get(DIM_OVERWORLD, 0, 0);
    uint8_t old = c->biome(4, 4);
    c->setBiomeAll(biome::Desert);
    c->clearBiomes3d();
    Entity* v = s->spawnMob(ent::Villager, 4.5, 70, 4.5);
    CHECK(v);
    if (v) {
        CHECK_EQ(s->villagerType(*v), 0);   // desert
        s->removeEntity(*v);
    }
    c->setBiomeAll(old);
}

TEST(villager_saved_with_its_chunk) {
    MemDevice dev(32 * 1024 * 1024);
    WorldStore store(&dev);
    StoreParams params;
    params.radius = 1;
    params.playerSlots = 4;
    CHECK(store.open(params));
    Chunk c(0, 0);
    SavedEntity e;
    e.kind = SAVED_KIND_MOB;
    e.type = ent::Villager;
    e.x = 3.5; e.y = 70; e.z = 3.5;
    e.health = 20;
    e.variant = 7;
    e.villager.profession = 9;
    e.villager.level = 3;
    e.villager.xp = 77;
    e.villager.hasJob = true;
    e.villager.jobX = 5; e.villager.jobY = 70; e.villager.jobZ = -2;
    TradeOffer o;
    o.buyA = ItemStack::of(itm::Emerald, 30);
    o.buyB = ItemStack::of(itm::Book);
    o.sell = ItemStack::of(itm::EnchantedBook);
    uint8_t l[ench::COUNT] = {};
    l[ench::Mending] = 1;
    setItemEnchants(o.sell, l, true);
    o.uses = 3;
    o.maxUses = 12;
    o.demand = -5;
    o.priceMult = 0.2f;
    e.villager.trades.push_back(o);
    CHECK(c.addEntity(e));
    CHECK(store.saveChunk(c));
    Chunk loaded(0, 0);
    CHECK_EQ(store.loadChunk(loaded), LOAD_OK);
    CHECK_EQ(loaded.entCount, 1);
    if (loaded.entCount != 1) return;
    const SavedEntity& r = loaded.ents[0];
    CHECK_EQ(r.variant, 7);
    CHECK_EQ(r.villager.profession, 9);
    CHECK_EQ(r.villager.level, 3);
    CHECK_EQ(r.villager.xp, 77);
    CHECK(r.villager.hasJob);
    CHECK_EQ(r.villager.jobZ, -2);
    CHECK_EQ((int)r.villager.trades.size(), 1);
    if (r.villager.trades.size() != 1) return;
    CHECK_EQ(r.villager.trades[0].buyA.count, 30);
    CHECK_EQ(r.villager.trades[0].buyB.id, itm::Book);
    CHECK_EQ(itemEnchant(r.villager.trades[0].sell, ench::Mending, true), 1);
    CHECK_EQ(r.villager.trades[0].uses, 3);
    CHECK_EQ(r.villager.trades[0].demand, -5);
    CHECK(r.villager.trades[0].priceMult == 0.2f);
}

TEST(every_profession_has_trades_at_every_level) {
    for (int prof = 1; prof <= 14; prof++) {
        if (prof == 11) continue;   // the nitwit
        for (int level = 1; level <= 5; level++) {
            VillagerState v;
            v.profession = (uint8_t)prof;
            v.level = (uint8_t)level;
            addVillagerOffers(v, 2);
            CHECK(v.trades.size() >= 1);
            for (const TradeOffer& o : v.trades) {
                CHECK(!o.buyA.empty() && !o.sell.empty());
                CHECK(o.buyA.count >= 1 && o.buyA.count <= 64);
            }
        }
    }
}
