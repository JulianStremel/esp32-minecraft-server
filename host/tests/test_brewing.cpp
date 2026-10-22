// The brewing stand without a client: vanilla's mixes, a brew over time, fuel, a brew
// stopped by taking the ingredient, and the stand saved with its chunk.
#include "testing.h"
#include "mc/registry.h"
#include "mc/server/server.h"
#include "mc/storage/block_device.h"
#include "mc/storage/world_store.h"

using namespace mc;

TEST(brewing_mixes_as_vanilla) {
    auto brew = [](uint16_t item, int p, uint16_t ing) { return brewResult(potionItem(item, p), ing); };
    CHECK_EQ(potionOf(brew(itm::Potion, potion::Water, itm::NetherWart)), (int)potion::Awkward);
    CHECK_EQ(potionOf(brew(itm::Potion, potion::Water, itm::Redstone)), (int)potion::Mundane);
    CHECK_EQ(potionOf(brew(itm::Potion, potion::Water, itm::GlowstoneDust)), (int)potion::Thick);
    CHECK_EQ(potionOf(brew(itm::Potion, potion::Awkward, itm::Sugar)), (int)potion::Swiftness);
    CHECK_EQ(potionOf(brew(itm::Potion, potion::Swiftness, itm::Redstone)), (int)potion::LongSwiftness);
    CHECK_EQ(potionOf(brew(itm::Potion, potion::Swiftness, itm::GlowstoneDust)), (int)potion::StrongSwiftness);
    CHECK_EQ(potionOf(brew(itm::Potion, potion::Healing, itm::FermentedSpiderEye)), (int)potion::Harming);
    CHECK_EQ(potionOf(brew(itm::Potion, potion::Awkward, itm::BlazePowder)), (int)potion::Strength);
    // containers: gunpowder makes a splash potion, dragon's breath a lingering one
    ItemStack sp = brew(itm::Potion, potion::Poison, itm::Gunpowder);
    CHECK_EQ(sp.id, itm::SplashPotion);
    CHECK_EQ(potionOf(sp), (int)potion::Poison);
    ItemStack lg = brew(itm::SplashPotion, potion::Poison, itm::DragonBreath);
    CHECK_EQ(lg.id, itm::LingeringPotion);
    // no mix: awkward + redstone, mundane + nether wart, an empty bottle
    CHECK(brew(itm::Potion, potion::Awkward, itm::Redstone).empty());
    CHECK(brew(itm::Potion, potion::Mundane, itm::NetherWart).empty());
    CHECK(brewResult(ItemStack::of(itm::GlassBottle), itm::NetherWart).empty());
    CHECK(brewingIngredient(itm::NetherWart));
    CHECK(brewingIngredient(itm::Gunpowder));
    CHECK(!brewingIngredient(itm::Diamond));
    CHECK(brewingAccepts(4, ItemStack::of(itm::BlazePowder)));
    CHECK(!brewingAccepts(4, ItemStack::of(itm::Coal)));
    CHECK(brewingAccepts(1, ItemStack::of(itm::GlassBottle)));
}

static Server* brewServer() {
    static Server* s = nullptr;
    if (s) return s;
    s = new Server();
    ServerConfig cfg;
    cfg.port = 0;
    cfg.worldType = WORLD_FLAT;
    cfg.spawnMobs = false;
    cfg.seed = 11;
    if (!s->begin(cfg, nullptr)) return nullptr;
    s->world.load(DIM_OVERWORLD, 0, 0);
    return s;
}

static TileEntity* placeStand(Server& s, int x, int y, int z) {
    s.setBlock(x, y, z, bs::BrewingStand);
    Chunk* c = s.world.get(DIM_OVERWORLD, x >> 4, z >> 4);
    TileEntity* t = c->tileAt(x & 15, y, z & 15);
    if (!t) t = c->addTile(TILE_BREWING, x & 15, y, z & 15);
    return t;
}

TEST(brewing_stand_brews_in_twenty_seconds) {
    Server* s = brewServer();
    CHECK(s);
    if (!s) return;
    Server::InDim in(*s, DIM_OVERWORLD);
    s->meta.worldAge = 1000;   // (0 means a stand never brought up to date)
    TileEntity* t = placeStand(*s, 4, 70, 4);
    CHECK(t);
    if (!t) return;
    for (int i = 0; i < 3; i++) t->items[i] = potionItem(itm::Potion, potion::Water);
    t->items[3] = ItemStack::of(itm::NetherWart, 2);
    t->items[4] = ItemStack::of(itm::BlazePowder, 1);
    s->updateBrewing(4, 70, 4, true);
    CHECK_EQ(t->burnTime, 19);     // a blaze powder: 20 brews, one started
    CHECK(t->items[4].empty());
    CHECK_EQ(t->cookTime, 400);
    CHECK(getBool(s->blockAt(4, 70, 4), "has_bottle_1"));
    s->meta.worldAge += 399;
    s->updateBrewing(4, 70, 4, true);
    CHECK_EQ(t->cookTime, 1);
    CHECK_EQ(potionOf(t->items[0]), (int)potion::Water);
    s->meta.worldAge += 1;
    s->updateBrewing(4, 70, 4, true);
    for (int i = 0; i < 3; i++) CHECK_EQ(potionOf(t->items[i]), (int)potion::Awkward);
    CHECK_EQ(t->items[3].count, 1);   // one nether wart used
    CHECK_EQ(t->cookTime, 0);         // awkward + nether wart: nothing more to brew
    // sugar: swiftness; taken away half way, the brew stops
    t->items[3] = ItemStack::of(itm::Sugar, 1);
    s->updateBrewing(4, 70, 4, true);
    CHECK_EQ(t->cookTime, 400);
    CHECK_EQ(t->burnTime, 18);
    s->meta.worldAge += 200;
    t->items[3].clear();
    s->updateBrewing(4, 70, 4, true);
    CHECK_EQ(t->cookTime, 0);
    CHECK_EQ(potionOf(t->items[0]), (int)potion::Awkward);
    // back, a new brew (and fuel) from the start
    t->items[3] = ItemStack::of(itm::Sugar, 1);
    s->updateBrewing(4, 70, 4, true);
    s->meta.worldAge += 400;
    s->updateBrewing(4, 70, 4, true);
    CHECK_EQ(potionOf(t->items[2]), (int)potion::Swiftness);
    CHECK_EQ(t->burnTime, 17);
    CHECK(t->items[3].empty());
}

TEST(brewing_stand_saved_with_its_chunk) {
    MemDevice dev(32 * 1024 * 1024);
    WorldStore store(&dev);
    StoreParams params;
    params.radius = 1;
    params.playerSlots = 4;
    CHECK(store.open(params));
    Chunk c(0, 0);
    c.set(1, 80, 1, bs::BrewingStand);
    TileEntity* t = c.addTile(TILE_BREWING, 1, 80, 1);
    CHECK_EQ(t->slotCount(), 5);
    t->items[0] = potionItem(itm::Potion, potion::Awkward);
    t->items[3] = ItemStack::of(itm::GhastTear, 3);
    t->items[4] = ItemStack::of(itm::BlazePowder, 7);
    t->burnTime = 12;
    t->cookTime = 250;
    t->burnTotal = (int16_t)itm::GhastTear;
    CHECK(store.saveChunk(c));
    Chunk loaded(0, 0);
    CHECK_EQ(store.loadChunk(loaded), LOAD_OK);
    TileEntity* u = loaded.tileAt(1, 80, 1);
    CHECK(u);
    if (!u) return;
    CHECK_EQ(u->type, TILE_BREWING);
    CHECK_EQ(potionOf(u->items[0]), (int)potion::Awkward);
    CHECK_EQ(u->items[3].count, 3);
    CHECK_EQ(u->items[4].count, 7);
    CHECK_EQ(u->burnTime, 12);
    CHECK_EQ(u->cookTime, 250);
    CHECK_EQ(u->burnTotal, (int16_t)itm::GhastTear);
}

TEST(nether_wart_grows_on_soul_sand) {
    Server* s = brewServer();
    CHECK(s);
    if (!s) return;
    Server::InDim in(*s, DIM_OVERWORLD);
    s->setBlock(8, 70, 8, bs::SoulSand);
    s->setBlock(8, 71, 8, bs::NetherWart);
    CHECK(s->canSupport(bs::NetherWart, 8, 71, 8));
    s->setBlock(9, 70, 8, bs::Stone);
    CHECK(!s->canSupport(bs::NetherWart, 9, 71, 8));
    int ticks = 0;
    while (getProp(s->blockAt(8, 71, 8), "age") < 3 && ticks < 1000) {
        s->randomTickBlock(8, 71, 8, s->blockAt(8, 71, 8));
        ticks++;
    }
    printf("    ripe after %d random ticks (vanilla: 30 on average)\n", ticks);
    CHECK_EQ(getProp(s->blockAt(8, 71, 8), "age"), 3);
    CHECK(ticks > 3 && ticks < 300);
}
