// Enchantments without a client: the data pack's items and exclusive sets, the NBT they
// live in, and what they do (damage, protection, unbreaking, mending, silk touch, fortune).
#include <math.h>
#include "testing.h"
#include "mc/item.h"
#include "mc/registry.h"
#include "mc/server/server.h"

using namespace mc;

#define CHECK_NEAR(a, b, eps) CHECK(fabs((double)(a) - (double)(b)) < (eps))

namespace {

struct SinkConn : Conn {   // a client that takes everything
    int read(uint8_t*, size_t) override { return 0; }
    int write(const uint8_t*, size_t n) override { return (int)n; }
    bool connected() override { return true; }
    void close() override {}
};

Server* enchantServer() {
    static Server* s = nullptr;
    if (s) return s;
    s = new Server();
    ServerConfig cfg;
    cfg.port = 0;
    cfg.worldType = WORLD_FLAT;
    cfg.spawnMobs = false;
    cfg.seed = 9;
    if (!s->begin(cfg, nullptr)) return nullptr;
    s->world.load(DIM_OVERWORLD, 0, 0);
    return s;
}

Player& freshPlayer(Server& s) {
    Player& p = s.players[3];
    if (!p.inPlay()) {
        p.conn.attach(new SinkConn);
        p.e.id = s.newEntityId();
    }
    p.state = CS_PLAY;
    p.gamemode = GM_SURVIVAL;
    p.dead = false;
    p.e.health = 20;
    p.e.invuln = 0;
    p.held = 0;
    for (int i = 0; i < 46; i++) p.inv[i].clear();
    s.clearEffects(p, false);
    return p;
}

ItemStack enchanted(uint16_t item, int e, int lvl, int e2 = -1, int lvl2 = 0) {
    ItemStack it = ItemStack::of(item);
    uint8_t l[ench::COUNT] = {};
    l[e] = (uint8_t)lvl;
    if (e2 >= 0) l[e2] = (uint8_t)lvl2;
    setItemEnchants(it, l);
    return it;
}

}  // namespace

TEST(enchantments_from_the_data_pack) {
    CHECK_EQ(ench::COUNT, 42);
    CHECK_EQ(ENCHANTS[ench::Sharpness].maxLevel, 5);
    CHECK_EQ(ENCHANTS[ench::Sharpness].minCost(5), 45);
    CHECK_EQ(ENCHANTS[ench::Protection].weight, 10);
    CHECK(ENCHANTS[ench::Mending].flags & EF_TREASURE);
    CHECK(!(ENCHANTS[ench::Mending].flags & EF_IN_TABLE));
    CHECK(ENCHANTS[ench::BindingCurse].flags & EF_CURSE);
    CHECK(enchantFits(ench::Sharpness, itm::DiamondSword));
    CHECK(enchantFits(ench::Sharpness, itm::IronAxe));
    CHECK(!enchantFits(ench::Sharpness, itm::Bow));
    CHECK(enchantFits(ench::Sharpness, itm::EnchantedBook));
    CHECK(enchantPrimary(ench::Sharpness, itm::DiamondSword));
    CHECK(!enchantPrimary(ench::Sharpness, itm::IronAxe));    // an anvil's, not the table's
    CHECK(enchantFits(ench::Efficiency, itm::Shears));
    CHECK(enchantFits(ench::FeatherFalling, itm::IronBoots));
    CHECK(!enchantFits(ench::FeatherFalling, itm::IronHelmet));
    CHECK(!enchantsCompatible(ench::Sharpness, ench::Smite));
    CHECK(!enchantsCompatible(ench::Protection, ench::FireProtection));
    CHECK(!enchantsCompatible(ench::Infinity, ench::Mending));
    CHECK(!enchantsCompatible(ench::Mending, ench::Infinity));
    CHECK(!enchantsCompatible(ench::SilkTouch, ench::Fortune));
    CHECK(enchantsCompatible(ench::Sharpness, ench::Unbreaking));
    CHECK(!enchantsCompatible(ench::Unbreaking, ench::Unbreaking));
    CHECK_EQ(enchantableItem(itm::GoldenSword)->enchantability, 22);
    CHECK_EQ(enchantableItem(itm::DiamondChestplate)->enchantability, 10);
    CHECK_EQ(enchantableItem(itm::Book)->enchantability, 1);
    CHECK(!enchantableItem(itm::Stick));
    CHECK_EQ(silkTouchItem(blk::Stone), itm::Stone);
    CHECK_EQ(silkTouchItem(blk::DiamondOre), itm::DiamondOre);
    CHECK_EQ(silkTouchItem(blk::Glass), itm::Glass);
    CHECK_EQ(silkTouchItem(blk::OakLeaves), itm::OakLeaves);
    CHECK_EQ(silkTouchItem(blk::OakPlanks), 0);
}

TEST(enchantments_live_in_the_item_nbt) {
    ItemStack it = ItemStack::of(itm::DiamondPickaxe);
    it.damage = 12;
    uint8_t l[ench::COUNT] = {};
    l[ench::Efficiency] = 5;
    l[ench::Unbreaking] = 3;
    CHECK(setItemEnchants(it, l));
    CHECK_EQ(it.damage, 12);
    CHECK_EQ(itemEnchant(it, ench::Efficiency), 5);
    CHECK_EQ(itemEnchant(it, ench::Unbreaking), 3);
    CHECK_EQ(itemEnchant(it, ench::Fortune), 0);
    CHECK_EQ(itemEnchant(it, ench::Efficiency, true), 0);   // not stored (a book's)
    CHECK(setItemRepairCost(it, 3));
    CHECK(setItemName(it, "{\"text\":\"Digger\"}"));
    CHECK_EQ(itemRepairCost(it), 3);
    CHECK(hasCustomName(it));
    CHECK_EQ(itemEnchant(it, ench::Efficiency), 5);
    // the client sees them: the enchantments component
    ByteBuf buf;
    Writer w(buf);
    writeSlot(w, it);
    Reader r(buf.data(), buf.size());
    CHECK_EQ(r.varint(), 1);
    CHECK_EQ(r.varint(), (int)itm::DiamondPickaxe);
    int added = r.varint();
    CHECK_EQ(added, 3);   // damage, custom name, enchantments (the repair cost is the server's)
    // all gone again: no tag left
    memset(l, 0, sizeof(l));
    CHECK(setItemEnchants(it, l));
    CHECK(setItemRepairCost(it, 0));
    CHECK(setItemName(it, nullptr));
    CHECK_EQ((int)it.tagSize(), 0);
    CHECK_EQ(it.damage, 12);
    ItemStack book = ItemStack::of(itm::EnchantedBook);
    l[ench::Mending] = 1;
    CHECK(setItemEnchants(book, l, true));
    CHECK_EQ(itemEnchant(book, ench::Mending, true), 1);
    CHECK_EQ(itemEnchant(book, ench::Mending), 0);
}

TEST(enchantments_in_combat) {
    Entity zombie, cow;
    zombie.kind = cow.kind = EK_MOB;
    zombie.type = ent::Zombie;
    cow.type = ent::Cow;
    CHECK_NEAR(enchantAttackBonus(enchanted(itm::DiamondSword, ench::Sharpness, 5), cow), 3.0f, 1e-5);
    CHECK_NEAR(enchantAttackBonus(enchanted(itm::DiamondSword, ench::Smite, 5), zombie), 12.5f, 1e-5);
    CHECK_NEAR(enchantAttackBonus(enchanted(itm::DiamondSword, ench::Smite, 5), cow), 0.0f, 1e-5);
    Server* s = enchantServer();
    CHECK(s);
    if (!s) return;
    Player& p = freshPlayer(*s);
    // protection IV on all four pieces: 16 of 20, 64% less; feather falling IV on falls: 20
    for (int i = 0; i < 4; i++)
        p.inv[SLOT_ARMOR_START + i] = enchanted(itm::IronHelmet + i, ench::Protection, 4);
    CHECK_NEAR(s->enchantProtection(p, DC_ATTACK), 0.36f, 1e-5);
    CHECK_NEAR(s->enchantProtection(p, DC_VOID), 1.0f, 1e-5);
    p.inv[SLOT_ARMOR_START + 3] = enchanted(itm::IronBoots, ench::FeatherFalling, 4, ench::Protection, 4);
    CHECK_NEAR(s->enchantProtection(p, DC_FALL), 0.2f, 1e-5);   // 16 + 12, at most 20
    p.inv[SLOT_ARMOR_START + 1] = enchanted(itm::IronChestplate, ench::FireProtection, 4);
    CHECK_EQ(s->burnTicks(p, 100), 40);
    // the player is hurt less: 10 damage, no armour points in the way (void-like cause)
    float before = p.e.health;
    s->damagePlayer(p, 4, DC_DROWN, -1);
    CHECK_NEAR(before - p.e.health, 4 * (1 - 12.0f / 25), 1e-4);
}

TEST(unbreaking_and_mending) {
    // unbreaking III on a tool: a quarter of the damage stays; on armour 70%
    ItemStack pick = enchanted(itm::IronPickaxe, ench::Unbreaking, 3);
    int kept = unbrokenDamage(pick, 10000, false);
    CHECK(kept > 2200 && kept < 2800);
    int armor = unbrokenDamage(enchanted(itm::IronBoots, ench::Unbreaking, 3), 10000, true);
    CHECK(armor > 6700 && armor < 7300);
    CHECK_EQ(unbrokenDamage(ItemStack::of(itm::IronPickaxe), 10, false), 10);
    Server* s = enchantServer();
    CHECK(s);
    if (!s) return;
    Player& p = freshPlayer(*s);
    p.inv[SLOT_HOTBAR_START] = enchanted(itm::DiamondPickaxe, ench::Mending, 1);
    p.inv[SLOT_HOTBAR_START].damage = 15;
    int xpBefore = p.xpTotal;
    s->giveXp(p, 10);   // 15 durability for 7 points, 3 left
    CHECK_EQ(p.inv[SLOT_HOTBAR_START].damage, 0);
    CHECK_EQ(p.xpTotal - xpBefore, 3);
    p.inv[SLOT_HOTBAR_START].damage = 15;
    s->giveXp(p, 5, false);   // /xp: no mending
    CHECK_EQ(p.inv[SLOT_HOTBAR_START].damage, 15);
}

TEST(silk_touch_and_fortune) {
    Server* s = enchantServer();
    CHECK(s);
    if (!s) return;
    Server::InDim in(*s, DIM_OVERWORLD);
    Player& p = freshPlayer(*s);
    p.e.dim = DIM_OVERWORLD;
    auto dropsOf = [&](uint16_t block, const ItemStack& tool, uint16_t item) {
        s->setBlock(3, 70, 3, block);
        p.inv[SLOT_HOTBAR_START] = tool;
        for (int i = 0; i < MC_MAX_ENTITIES; i++)   // (the slots freed, as the entity tick does)
            if (s->entities[i].kind == EK_ITEM) s->entities[i].kind = EK_NONE;
        s->breakBlock(3, 70, 3, &p, true);
        int n = 0;
        for (int i = 0; i < MC_MAX_ENTITIES; i++) {
            Entity& e = s->entities[i];
            if (e.kind == EK_ITEM && !e.removed && e.item.id == item) n += e.item.count;
        }
        return n;
    };
    CHECK_EQ(dropsOf(bs::Stone, enchanted(itm::IronPickaxe, ench::SilkTouch, 1), itm::Stone), 1);
    CHECK_EQ(dropsOf(bs::Stone, ItemStack::of(itm::IronPickaxe), itm::Cobblestone), 1);
    CHECK_EQ(dropsOf(bs::Glass, enchanted(itm::IronPickaxe, ench::SilkTouch, 1), itm::Glass), 1);
    CHECK_EQ(dropsOf(bs::DiamondOre, enchanted(itm::IronPickaxe, ench::SilkTouch, 1), itm::DiamondOre), 1);
    // fortune III on diamond ore: 2.2 diamonds on average (1 + 0, 1, 2, 3 more: 2 in 5, ...)
    int total = 0, n = 200;
    for (int i = 0; i < n; i++) total += dropsOf(bs::DiamondOre, enchanted(itm::IronPickaxe, ench::Fortune, 3), itm::Diamond);
    printf("    fortune III: %.2f diamonds an ore (vanilla 2.2)\n", total / (double)n);
    CHECK(total > n * 1.9 && total < n * 2.5);
    total = 0;
    for (int i = 0; i < n; i++) total += dropsOf(bs::DiamondOre, ItemStack::of(itm::IronPickaxe), itm::Diamond);
    CHECK_EQ(total, n);
}

TEST(efficiency_digs_faster) {
    Server* s = enchantServer();
    CHECK(s);
    if (!s) return;
    Player& p = freshPlayer(*s);
    p.inv[SLOT_HOTBAR_START] = ItemStack::of(itm::IronPickaxe);
    float plain = s->digTicks(p, bs::Stone);
    p.inv[SLOT_HOTBAR_START] = enchanted(itm::IronPickaxe, ench::Efficiency, 5);
    float fast = s->digTicks(p, bs::Stone);
    // iron: 6, efficiency V: 6 + 26 = 32 (stone 1.5 hard: 8 ticks, then 2)
    CHECK_EQ(plain, 8.0f);
    CHECK_EQ(fast, 2.0f);
    CHECK_EQ(s->enchantAttributes(p) & 15, 5u);
}

namespace {
Player& atWorkstation(Server& s, uint8_t kind) {
    Player& p = freshPlayer(s);
    p.winKind = kind;
    p.winId = 5;
    p.winX = 40; p.winY = 70; p.winZ = 40;
    for (auto& c : p.craft) c.clear();
    p.anvilName[0] = 0;
    p.xpLevel = 100;
    return p;
}
}  // namespace

TEST(enchanting_table_offers) {
    Server* s = enchantServer();
    CHECK(s);
    if (!s) return;
    Server::InDim in(*s, DIM_OVERWORLD);
    s->world.load(DIM_OVERWORLD, 2, 2);
    // fifteen bookshelves around a table at (40, 70, 40), the air between kept
    for (int dx = -2; dx <= 2; dx++)
        for (int dz = -2; dz <= 2; dz++)
            for (int dy = 0; dy <= 1; dy++) {
                bool ring = dx == -2 || dx == 2 || dz == -2 || dz == 2;
                s->setBlock(40 + dx, 70 + dy, 40 + dz, ring && !(dz == 2 && dx >= -1 && dx <= 1) ? bs::Bookshelf : 0);
            }
    s->setBlock(40, 70, 40, bs::EnchantingTable);
    int shelves = s->enchantingShelves(40, 70, 40);
    CHECK_EQ(shelves, 26);   // 32 around, 6 left open for the door
    Player& p = atWorkstation(*s, WK_ENCHANTING);
    p.craft[0] = ItemStack::of(itm::DiamondPickaxe);
    p.craft[1] = ItemStack::of(itm::LapisLazuli, 3);
    int seen30 = 0, picks = 0, sharp = 0;
    for (int seed = 1; seed <= 200; seed++) {
        p.enchantSeed = seed * 7919;
        s->workstationChanged(p);
        CHECK(p.tableCost[0] >= 1 && p.tableCost[0] <= p.tableCost[1] + 1);
        if (p.tableCost[2] == 30) seen30++;
        for (int k = 0; k < 3; k++) {
            if (p.tableClue[k] < 0) continue;
            picks++;
            if (!enchantPrimary(p.tableClue[k], itm::DiamondPickaxe)) sharp++;
        }
    }
    CHECK_EQ(seen30, 200);   // fifteen or more shelves: the last offer always costs 30
    CHECK(picks > 500);
    CHECK_EQ(sharp, 0);      // only a pickaxe's enchantments are offered
    // the same seed, the same offers; enchanting takes levels and lapis, and a new seed
    p.enchantSeed = 12345;
    s->workstationChanged(p);
    int clue = p.tableClue[2], cost = p.tableCost[2];
    s->workstationChanged(p);
    CHECK_EQ(p.tableClue[2], clue);
    s->enchantButton(p, 2);
    CHECK_EQ(p.xpLevel, 97);
    CHECK(p.craft[1].empty());
    CHECK(p.enchantSeed != 12345);
    CHECK(itemEnchant(p.craft[0], clue) > 0);   // the enchantment shown is among those given
    printf("    cost %d: %s %d (and %d more)\n", cost, ENCHANTMENT_NAME[clue], itemEnchant(p.craft[0], clue),
           [&] { uint8_t l[ench::COUNT]; itemEnchants(p.craft[0], l); int n = -1; for (uint8_t v : l) n += v != 0; return n; }());
    CHECK_EQ(p.tableCost[0], 0);   // enchanted: nothing more on offer
    // a book becomes an enchanted book
    p.craft[0] = ItemStack::of(itm::Book);
    p.craft[1] = ItemStack::of(itm::LapisLazuli, 3);
    s->workstationChanged(p);
    s->enchantButton(p, 0);
    CHECK_EQ(p.craft[0].id, itm::EnchantedBook);
    uint8_t l[ench::COUNT];
    itemEnchants(p.craft[0], l, true);
    int n = 0;
    for (uint8_t v : l) n += v != 0;
    CHECK(n >= 1);
    // not enough lapis: nothing happens
    p.craft[0] = ItemStack::of(itm::IronSword);
    p.craft[1] = ItemStack::of(itm::LapisLazuli, 1);
    s->workstationChanged(p);
    s->enchantButton(p, 2);
    CHECK_EQ(itemEnchant(p.craft[0], ench::Sharpness) + itemEnchant(p.craft[0], ench::Unbreaking), 0);
    CHECK_EQ(p.craft[1].count, 1);
    p.winKind = WK_NONE;
}

TEST(anvil_repairs_combines_and_names) {
    Server* s = enchantServer();
    CHECK(s);
    if (!s) return;
    Server::InDim in(*s, DIM_OVERWORLD);
    s->setBlock(40, 70, 40, bs::Anvil);
    Player& p = atWorkstation(*s, WK_ANVIL);
    // two worn iron pickaxes: both durabilities and 12%
    p.craft[0] = ItemStack::of(itm::IronPickaxe);
    p.craft[0].damage = 200;
    p.craft[1] = ItemStack::of(itm::IronPickaxe);
    p.craft[1].damage = 100;
    s->workstationChanged(p);
    CHECK_EQ(p.craft[2].damage, 20);
    CHECK_EQ(p.anvilCost, 2);
    CHECK_EQ(itemRepairCost(p.craft[2]), 1);
    // with iron ingots: a quarter of 250 each, four of the five
    p.craft[1] = ItemStack::of(itm::IronIngot, 5);
    s->workstationChanged(p);
    CHECK_EQ(p.craft[2].damage, 0);
    CHECK_EQ(p.anvilCost, 4);
    CHECK(s->workstationMayTake(p));
    s->workstationTaken(p);
    CHECK(p.craft[0].empty());
    CHECK_EQ(p.craft[1].count, 1);
    CHECK_EQ(p.xpLevel, 96);
    // sharpness IV and a book of sharpness IV: V, for the book's halved cost
    p.craft[0] = enchanted(itm::DiamondSword, ench::Sharpness, 4);
    ItemStack book = ItemStack::of(itm::EnchantedBook);
    uint8_t l[ench::COUNT] = {};
    l[ench::Sharpness] = 4;
    setItemEnchants(book, l, true);
    p.craft[1] = book;
    s->workstationChanged(p);
    CHECK_EQ(itemEnchant(p.craft[2], ench::Sharpness), 5);
    CHECK_EQ(p.anvilCost, 5);
    // smite does not go with sharpness: no result
    l[ench::Sharpness] = 0;
    l[ench::Smite] = 3;
    setItemEnchants(book, l, true);
    p.craft[1] = book;
    s->workstationChanged(p);
    CHECK(p.craft[2].empty());
    // a name only: 1 level, the prior work not raised
    p.craft[1].clear();
    setItemRepairCost(p.craft[0], 3);
    s->anvilRename(p, "Excalibur");
    CHECK_EQ(p.anvilCost, 4);   // 3 prior work + 1
    CHECK(hasCustomName(p.craft[2]));
    CHECK_EQ(itemRepairCost(p.craft[2]), 3);
    // too expensive: 40 levels or more
    setItemRepairCost(p.craft[0], 39);
    s->anvilRename(p, "Excalibur II");
    CHECK_EQ(p.anvilCost, 39);   // renaming only: at most 39
    p.craft[1] = ItemStack::of(itm::Diamond, 1);
    p.craft[0].damage = 100;
    s->workstationChanged(p);
    CHECK(p.anvilCost >= 40);
    CHECK(p.craft[2].empty());
    p.winKind = WK_NONE;
}

TEST(grindstone_takes_enchantments_off) {
    Server* s = enchantServer();
    CHECK(s);
    if (!s) return;
    Player& p = atWorkstation(*s, WK_GRINDSTONE);
    p.xpLevel = 0;
    p.xpProgress = 0;
    p.craft[0] = enchanted(itm::DiamondSword, ench::Sharpness, 5, ench::VanishingCurse, 1);
    s->workstationChanged(p);
    CHECK_EQ(itemEnchant(p.craft[2], ench::Sharpness), 0);
    CHECK_EQ(itemEnchant(p.craft[2], ench::VanishingCurse), 1);   // curses stay
    CHECK_EQ(itemRepairCost(p.craft[2]), 1);
    int before = p.xpTotal;
    s->workstationTaken(p);
    int xp = p.xpTotal - before;
    CHECK(xp >= 23 && xp <= 45);   // sharpness V's minimum cost 45: half to all of it
    CHECK(p.craft[0].empty());
    // an enchanted book: a book again
    ItemStack book = ItemStack::of(itm::EnchantedBook);
    uint8_t l[ench::COUNT] = {};
    l[ench::Mending] = 1;
    setItemEnchants(book, l, true);
    p.craft[1] = book;
    s->workstationChanged(p);
    CHECK_EQ(p.craft[2].id, itm::Book);
    CHECK_EQ((int)p.craft[2].tagSize(), 0);
    // two worn swords: both durabilities and 5%
    p.craft[0] = ItemStack::of(itm::IronSword);
    p.craft[0].damage = 200;
    p.craft[1] = ItemStack::of(itm::IronSword);
    p.craft[1].damage = 150;
    s->workstationChanged(p);
    CHECK_EQ(p.craft[2].damage, 250 - (50 + 100 + 12));
    CHECK(!grindstoneTakes(ItemStack::of(itm::Stick)));
    CHECK(grindstoneTakes(ItemStack::of(itm::IronSword)));
    p.winKind = WK_NONE;
}
