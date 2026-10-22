// Villagers and trading, after vanilla 1.21 (Villager, VillagerTrades, MerchantMenu,
// MerchantOffer): an unemployed villager takes the profession of a free workstation
// near it; each level brings two trades from its profession's list; trading earns it
// experience up to master; it restocks at its workstation twice a day, and what was
// traded a lot costs more after (demand). Its biome gives its clothes (the type).
// Not (yet): beds, schedules and gossip, breeding, zombie villagers, iron golems,
// explorer maps (cartographers), dyed leather and suspicious stew effects.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc/registry.h"
#include "mc/server/mob_util.h"
#include "mc/server/server.h"

namespace mc {

namespace {

// VillagerProfession and VillagerType: the registries' order (their ids)
enum : uint8_t {
    P_NONE, P_ARMORER, P_BUTCHER, P_CARTOGRAPHER, P_CLERIC, P_FARMER, P_FISHERMAN, P_FLETCHER, P_LEATHERWORKER,
    P_LIBRARIAN, P_MASON, P_NITWIT, P_SHEPHERD, P_TOOLSMITH, P_WEAPONSMITH, P_COUNT
};
const char* const PROF_NAME[P_COUNT] = {"none", "armorer", "butcher", "cartographer", "cleric", "farmer", "fisherman",
                                        "fletcher", "leatherworker", "librarian", "mason", "nitwit", "shepherd",
                                        "toolsmith", "weaponsmith"};
enum : uint8_t { VT_DESERT, VT_JUNGLE, VT_PLAINS, VT_SAVANNA, VT_SNOW, VT_SWAMP, VT_TAIGA };

// the workstation (job site) of a profession (PoiTypes)
int professionOf(uint16_t blockId) {
    switch (blockId) {
        case blk::BlastFurnace: return P_ARMORER;
        case blk::Smoker: return P_BUTCHER;
        case blk::CartographyTable: return P_CARTOGRAPHER;
        case blk::BrewingStand: return P_CLERIC;
        case blk::Composter: return P_FARMER;
        case blk::Barrel: return P_FISHERMAN;
        case blk::FletchingTable: return P_FLETCHER;
        case blk::Cauldron: case blk::WaterCauldron: case blk::LavaCauldron: case blk::PowderSnowCauldron:
            return P_LEATHERWORKER;
        case blk::Lectern: return P_LIBRARIAN;
        case blk::Stonecutter: return P_MASON;
        case blk::Loom: return P_SHEPHERD;
        case blk::SmithingTable: return P_TOOLSMITH;
        case blk::Grindstone: return P_WEAPONSMITH;
        default: return P_NONE;
    }
}

// VillagerType#byBiome
uint8_t typeOfBiome(uint8_t b) {
    switch (b) {
        case biome::Badlands: case biome::ErodedBadlands: case biome::WoodedBadlands: case biome::Desert: return VT_DESERT;
        case biome::Jungle: case biome::BambooJungle: case biome::SparseJungle: return VT_JUNGLE;
        case biome::Savanna: case biome::SavannaPlateau: case biome::WindsweptSavanna: return VT_SAVANNA;
        case biome::DeepFrozenOcean: case biome::FrozenOcean: case biome::FrozenRiver: case biome::IceSpikes:
        case biome::SnowyBeach: case biome::SnowyTaiga: case biome::SnowyPlains: case biome::Grove:
        case biome::SnowySlopes: case biome::FrozenPeaks: case biome::JaggedPeaks:
            return VT_SNOW;
        case biome::Swamp: case biome::MangroveSwamp: return VT_SWAMP;
        case biome::OldGrowthSpruceTaiga: case biome::OldGrowthPineTaiga: case biome::Taiga: case biome::WindsweptForest:
        case biome::WindsweptGravellyHills: case biome::WindsweptHills:
            return VT_TAIGA;
        default: return VT_PLAINS;
    }
}

// ------------------------------------------------------------------ VillagerTrades
// The kinds of VillagerTrades.ItemListing: buy (EmeraldForItems: a of item for an
// emerald), sell (ItemsForEmeralds: a emeralds for b of item), swap
// (ItemsAndEmeraldsToItems: a of item and c emeralds for b of item2), an enchanted item
// (base price a), an enchanted book, the boat of the villager's type for an emerald,
// tipped arrows, suspicious stew. colors: the item and the next 15 (the 16 colours), each
// its own listing. pm: the price multiplier in hundredths.
enum Kind : uint8_t { BUY, SELL, SWAP, ENCH_ITEM, ENCH_BOOK, TYPE_BOAT, TIPPED, STEW };
struct Listing {
    uint8_t prof, level, kind, colors;
    uint16_t item;
    uint8_t a, b, c, uses, xp, pm;
    uint16_t item2;
};
constexpr Listing Buy(uint8_t p, uint8_t l, uint16_t item, uint8_t cost, uint8_t uses, uint8_t xp) {
    return {p, l, BUY, 0, item, cost, 0, 0, uses, xp, 5, 0};
}
constexpr Listing Sell(uint8_t p, uint8_t l, uint16_t item, uint8_t emeralds, uint8_t count, uint8_t uses, uint8_t xp,
                       uint8_t pm = 5, uint8_t colors = 0) {
    return {p, l, SELL, colors, item, emeralds, count, 0, uses, xp, pm, 0};
}
constexpr Listing Swap(uint8_t p, uint8_t l, uint16_t from, uint8_t fromCount, uint8_t emeralds, uint16_t to, uint8_t toCount,
                       uint8_t uses, uint8_t xp) {
    return {p, l, SWAP, 0, from, fromCount, toCount, emeralds, uses, xp, 5, to};
}
constexpr Listing Ench(uint8_t p, uint8_t l, uint16_t item, uint8_t base, uint8_t uses, uint8_t xp, uint8_t pm = 5) {
    return {p, l, ENCH_ITEM, 0, item, base, 0, 0, uses, xp, pm, 0};
}
constexpr Listing Book(uint8_t p, uint8_t l, uint8_t xp) { return {p, l, ENCH_BOOK, 0, 0, 0, 0, 0, 12, xp, 20, 0}; }
constexpr Listing Stew(uint8_t xp) { return {P_FARMER, 4, STEW, 0, itm::SuspiciousStew, 1, 1, 0, 12, xp, 5, 0}; }

const Listing LISTINGS[] = {
    // farmer
    Buy(P_FARMER, 1, itm::Wheat, 20, 16, 2), Buy(P_FARMER, 1, itm::Potato, 26, 16, 2), Buy(P_FARMER, 1, itm::Carrot, 22, 16, 2),
    Buy(P_FARMER, 1, itm::Beetroot, 15, 16, 2), Sell(P_FARMER, 1, itm::Bread, 1, 6, 16, 1),
    Buy(P_FARMER, 2, itm::Pumpkin, 6, 12, 10), Sell(P_FARMER, 2, itm::PumpkinPie, 1, 4, 12, 5), Sell(P_FARMER, 2, itm::Apple, 1, 4, 16, 5),
    Sell(P_FARMER, 3, itm::Cookie, 3, 18, 12, 10), Buy(P_FARMER, 3, itm::Melon, 4, 12, 20),
    Sell(P_FARMER, 4, itm::Cake, 1, 1, 12, 15), Stew(15), Stew(15), Stew(15), Stew(15), Stew(15), Stew(15),
    Sell(P_FARMER, 5, itm::GoldenCarrot, 3, 3, 12, 30), Sell(P_FARMER, 5, itm::GlisteringMelonSlice, 4, 3, 12, 30),
    // fisherman
    Buy(P_FISHERMAN, 1, itm::String, 20, 16, 2), Buy(P_FISHERMAN, 1, itm::Coal, 10, 16, 2),
    Swap(P_FISHERMAN, 1, itm::Cod, 6, 1, itm::CookedCod, 6, 16, 1), Sell(P_FISHERMAN, 1, itm::CodBucket, 3, 1, 16, 1),
    Buy(P_FISHERMAN, 2, itm::Cod, 15, 16, 10), Swap(P_FISHERMAN, 2, itm::Salmon, 6, 1, itm::CookedSalmon, 6, 16, 5),
    Sell(P_FISHERMAN, 2, itm::Campfire, 2, 1, 12, 5),
    Buy(P_FISHERMAN, 3, itm::Salmon, 13, 16, 20), Ench(P_FISHERMAN, 3, itm::FishingRod, 3, 3, 10, 20),
    Buy(P_FISHERMAN, 4, itm::TropicalFish, 6, 12, 30),
    Buy(P_FISHERMAN, 5, itm::Pufferfish, 4, 12, 30), {P_FISHERMAN, 5, TYPE_BOAT, 0, 0, 1, 0, 0, 12, 30, 5, 0},
    // shepherd
    Buy(P_SHEPHERD, 1, itm::WhiteWool, 18, 16, 2), Buy(P_SHEPHERD, 1, itm::BrownWool, 18, 16, 2),
    Buy(P_SHEPHERD, 1, itm::BlackWool, 18, 16, 2), Buy(P_SHEPHERD, 1, itm::GrayWool, 18, 16, 2), Sell(P_SHEPHERD, 1, itm::Shears, 2, 1, 12, 1),
    Buy(P_SHEPHERD, 2, itm::WhiteDye, 12, 16, 10), Buy(P_SHEPHERD, 2, itm::GrayDye, 12, 16, 10), Buy(P_SHEPHERD, 2, itm::BlackDye, 12, 16, 10),
    Buy(P_SHEPHERD, 2, itm::LightBlueDye, 12, 16, 10), Buy(P_SHEPHERD, 2, itm::LimeDye, 12, 16, 10),
    Sell(P_SHEPHERD, 2, itm::WhiteWool, 1, 1, 16, 5, 5, 1), Sell(P_SHEPHERD, 2, itm::WhiteCarpet, 1, 4, 16, 5, 5, 1),
    Buy(P_SHEPHERD, 3, itm::YellowDye, 12, 16, 20), Buy(P_SHEPHERD, 3, itm::LightGrayDye, 12, 16, 20),
    Buy(P_SHEPHERD, 3, itm::OrangeDye, 12, 16, 20), Buy(P_SHEPHERD, 3, itm::RedDye, 12, 16, 20), Buy(P_SHEPHERD, 3, itm::PinkDye, 12, 16, 20),
    Sell(P_SHEPHERD, 3, itm::WhiteBed, 3, 1, 12, 10, 5, 1),
    Buy(P_SHEPHERD, 4, itm::BrownDye, 12, 16, 30), Buy(P_SHEPHERD, 4, itm::PurpleDye, 12, 16, 30), Buy(P_SHEPHERD, 4, itm::BlueDye, 12, 16, 30),
    Buy(P_SHEPHERD, 4, itm::GreenDye, 12, 16, 30), Buy(P_SHEPHERD, 4, itm::MagentaDye, 12, 16, 30), Buy(P_SHEPHERD, 4, itm::CyanDye, 12, 16, 30),
    Sell(P_SHEPHERD, 4, itm::WhiteBanner, 3, 1, 12, 15, 5, 1),
    Sell(P_SHEPHERD, 5, itm::Painting, 2, 3, 12, 30),
    // fletcher
    Buy(P_FLETCHER, 1, itm::Stick, 32, 16, 2), Sell(P_FLETCHER, 1, itm::Arrow, 1, 16, 12, 1),
    Swap(P_FLETCHER, 1, itm::Gravel, 10, 1, itm::Flint, 10, 12, 1),
    Buy(P_FLETCHER, 2, itm::Flint, 26, 12, 10), Sell(P_FLETCHER, 2, itm::Bow, 2, 1, 12, 5),
    Buy(P_FLETCHER, 3, itm::String, 14, 16, 20), Sell(P_FLETCHER, 3, itm::Crossbow, 3, 1, 12, 10),
    Buy(P_FLETCHER, 4, itm::Feather, 24, 16, 30), Ench(P_FLETCHER, 4, itm::Bow, 2, 3, 15),
    Buy(P_FLETCHER, 5, itm::TripwireHook, 8, 12, 30), Ench(P_FLETCHER, 5, itm::Crossbow, 3, 3, 15),
    {P_FLETCHER, 5, TIPPED, 0, itm::Arrow, 5, 5, 2, 12, 30, 5, itm::TippedArrow},
    // librarian
    Buy(P_LIBRARIAN, 1, itm::Paper, 24, 16, 2), Book(P_LIBRARIAN, 1, 1), Sell(P_LIBRARIAN, 1, itm::Bookshelf, 9, 1, 12, 1),
    Buy(P_LIBRARIAN, 2, itm::Book, 4, 12, 10), Book(P_LIBRARIAN, 2, 5), Sell(P_LIBRARIAN, 2, itm::Lantern, 1, 1, 12, 5),
    Buy(P_LIBRARIAN, 3, itm::InkSac, 5, 12, 20), Book(P_LIBRARIAN, 3, 10), Sell(P_LIBRARIAN, 3, itm::Glass, 1, 4, 12, 10),
    Buy(P_LIBRARIAN, 4, itm::WritableBook, 2, 12, 30), Book(P_LIBRARIAN, 4, 15), Sell(P_LIBRARIAN, 4, itm::Clock, 5, 1, 12, 15),
    Sell(P_LIBRARIAN, 4, itm::Compass, 4, 1, 12, 15),
    Sell(P_LIBRARIAN, 5, itm::NameTag, 20, 1, 12, 30),
    // cartographer (the explorer maps left out: no maps yet)
    Buy(P_CARTOGRAPHER, 1, itm::Paper, 24, 16, 2), Sell(P_CARTOGRAPHER, 1, itm::Map, 7, 1, 12, 1),
    Buy(P_CARTOGRAPHER, 2, itm::GlassPane, 11, 16, 10),
    Buy(P_CARTOGRAPHER, 3, itm::Compass, 1, 12, 20),
    Sell(P_CARTOGRAPHER, 4, itm::ItemFrame, 7, 1, 12, 15), Sell(P_CARTOGRAPHER, 4, itm::WhiteBanner, 3, 1, 12, 15, 5, 1),
    Sell(P_CARTOGRAPHER, 5, itm::GlobeBannerPattern, 8, 1, 12, 30),
    // cleric
    Buy(P_CLERIC, 1, itm::RottenFlesh, 32, 16, 2), Sell(P_CLERIC, 1, itm::Redstone, 1, 2, 12, 1),
    Buy(P_CLERIC, 2, itm::GoldIngot, 3, 12, 10), Sell(P_CLERIC, 2, itm::LapisLazuli, 1, 1, 12, 5),
    Buy(P_CLERIC, 3, itm::RabbitFoot, 2, 12, 20), Sell(P_CLERIC, 3, itm::Glowstone, 4, 1, 12, 10),
    Buy(P_CLERIC, 4, itm::TurtleScute, 4, 12, 30), Buy(P_CLERIC, 4, itm::GlassBottle, 9, 12, 30),
    Sell(P_CLERIC, 4, itm::EnderPearl, 5, 1, 12, 15),
    Buy(P_CLERIC, 5, itm::NetherWart, 22, 12, 30), Sell(P_CLERIC, 5, itm::ExperienceBottle, 3, 1, 12, 30),
    // armorer
    Buy(P_ARMORER, 1, itm::Coal, 15, 16, 2), Sell(P_ARMORER, 1, itm::IronLeggings, 7, 1, 12, 1, 20),
    Sell(P_ARMORER, 1, itm::IronBoots, 4, 1, 12, 1, 20), Sell(P_ARMORER, 1, itm::IronHelmet, 5, 1, 12, 1, 20),
    Sell(P_ARMORER, 1, itm::IronChestplate, 9, 1, 12, 1, 20),
    Buy(P_ARMORER, 2, itm::IronIngot, 4, 12, 10), Sell(P_ARMORER, 2, itm::Bell, 36, 1, 12, 5, 20),
    Sell(P_ARMORER, 2, itm::ChainmailBoots, 1, 1, 12, 5, 20), Sell(P_ARMORER, 2, itm::ChainmailLeggings, 3, 1, 12, 5, 20),
    Buy(P_ARMORER, 3, itm::LavaBucket, 1, 12, 20), Buy(P_ARMORER, 3, itm::Diamond, 1, 12, 20),
    Sell(P_ARMORER, 3, itm::ChainmailHelmet, 1, 1, 12, 10, 20), Sell(P_ARMORER, 3, itm::ChainmailChestplate, 4, 1, 12, 10, 20),
    Sell(P_ARMORER, 3, itm::Shield, 5, 1, 12, 10, 20),
    Ench(P_ARMORER, 4, itm::DiamondLeggings, 14, 3, 15, 20), Ench(P_ARMORER, 4, itm::DiamondBoots, 8, 3, 15, 20),
    Ench(P_ARMORER, 5, itm::DiamondHelmet, 8, 3, 30, 20), Ench(P_ARMORER, 5, itm::DiamondChestplate, 16, 3, 30, 20),
    // weaponsmith
    Buy(P_WEAPONSMITH, 1, itm::Coal, 15, 16, 2), Sell(P_WEAPONSMITH, 1, itm::IronAxe, 3, 1, 12, 1, 20),
    Ench(P_WEAPONSMITH, 1, itm::IronSword, 2, 3, 1),
    Buy(P_WEAPONSMITH, 2, itm::IronIngot, 4, 12, 10), Sell(P_WEAPONSMITH, 2, itm::Bell, 36, 1, 12, 5, 20),
    Buy(P_WEAPONSMITH, 3, itm::Flint, 24, 12, 20),
    Buy(P_WEAPONSMITH, 4, itm::Diamond, 1, 12, 30), Ench(P_WEAPONSMITH, 4, itm::DiamondAxe, 12, 3, 15, 20),
    Ench(P_WEAPONSMITH, 5, itm::DiamondSword, 8, 3, 30, 20),
    // toolsmith
    Buy(P_TOOLSMITH, 1, itm::Coal, 15, 16, 2), Sell(P_TOOLSMITH, 1, itm::StoneAxe, 1, 1, 12, 1, 20),
    Sell(P_TOOLSMITH, 1, itm::StoneShovel, 1, 1, 12, 1, 20), Sell(P_TOOLSMITH, 1, itm::StonePickaxe, 1, 1, 12, 1, 20),
    Sell(P_TOOLSMITH, 1, itm::StoneHoe, 1, 1, 12, 1, 20),
    Buy(P_TOOLSMITH, 2, itm::IronIngot, 4, 12, 10), Sell(P_TOOLSMITH, 2, itm::Bell, 36, 1, 12, 5, 20),
    Buy(P_TOOLSMITH, 3, itm::Flint, 30, 12, 20), Ench(P_TOOLSMITH, 3, itm::IronAxe, 1, 3, 10, 20),
    Ench(P_TOOLSMITH, 3, itm::IronShovel, 2, 3, 10, 20), Ench(P_TOOLSMITH, 3, itm::IronPickaxe, 3, 3, 10, 20),
    Sell(P_TOOLSMITH, 3, itm::DiamondHoe, 4, 1, 3, 10, 20),
    Buy(P_TOOLSMITH, 4, itm::Diamond, 1, 12, 30), Ench(P_TOOLSMITH, 4, itm::DiamondAxe, 12, 3, 15, 20),
    Ench(P_TOOLSMITH, 4, itm::DiamondShovel, 5, 3, 15, 20),
    Ench(P_TOOLSMITH, 5, itm::DiamondPickaxe, 13, 3, 30, 20),
    // butcher
    Buy(P_BUTCHER, 1, itm::Chicken, 14, 16, 2), Buy(P_BUTCHER, 1, itm::Porkchop, 7, 16, 2), Buy(P_BUTCHER, 1, itm::Rabbit, 4, 16, 2),
    Sell(P_BUTCHER, 1, itm::RabbitStew, 1, 1, 12, 1),
    Buy(P_BUTCHER, 2, itm::Coal, 15, 16, 2), Sell(P_BUTCHER, 2, itm::CookedPorkchop, 1, 5, 16, 5),
    Sell(P_BUTCHER, 2, itm::CookedChicken, 1, 8, 16, 5),
    Buy(P_BUTCHER, 3, itm::Mutton, 7, 16, 20), Buy(P_BUTCHER, 3, itm::Beef, 10, 16, 20),
    Buy(P_BUTCHER, 4, itm::DriedKelpBlock, 10, 12, 30),
    Buy(P_BUTCHER, 5, itm::SweetBerries, 10, 12, 30),
    // leatherworker (DyedArmorForEmeralds: undyed here)
    Buy(P_LEATHERWORKER, 1, itm::Leather, 6, 16, 2), Sell(P_LEATHERWORKER, 1, itm::LeatherLeggings, 3, 1, 12, 1, 20),
    Sell(P_LEATHERWORKER, 1, itm::LeatherChestplate, 7, 1, 12, 1, 20),
    Buy(P_LEATHERWORKER, 2, itm::Flint, 26, 12, 10), Sell(P_LEATHERWORKER, 2, itm::LeatherHelmet, 5, 1, 12, 5, 20),
    Sell(P_LEATHERWORKER, 2, itm::LeatherBoots, 4, 1, 12, 5, 20),
    Buy(P_LEATHERWORKER, 3, itm::RabbitHide, 9, 12, 20), Sell(P_LEATHERWORKER, 3, itm::LeatherChestplate, 7, 1, 12, 1, 20),
    Buy(P_LEATHERWORKER, 4, itm::TurtleScute, 4, 12, 30), Sell(P_LEATHERWORKER, 4, itm::LeatherHorseArmor, 6, 1, 12, 15, 20),
    Sell(P_LEATHERWORKER, 5, itm::Saddle, 6, 1, 12, 30, 20), Sell(P_LEATHERWORKER, 5, itm::LeatherHelmet, 5, 1, 12, 30, 20),
    // mason
    Buy(P_MASON, 1, itm::ClayBall, 10, 16, 2), Sell(P_MASON, 1, itm::Brick, 1, 10, 16, 1),
    Buy(P_MASON, 2, itm::Stone, 20, 16, 10), Sell(P_MASON, 2, itm::ChiseledStoneBricks, 1, 4, 16, 5),
    Buy(P_MASON, 3, itm::Granite, 16, 16, 20), Buy(P_MASON, 3, itm::Andesite, 16, 16, 20), Buy(P_MASON, 3, itm::Diorite, 16, 16, 20),
    Sell(P_MASON, 3, itm::DripstoneBlock, 1, 4, 16, 10), Sell(P_MASON, 3, itm::PolishedAndesite, 1, 4, 16, 10),
    Sell(P_MASON, 3, itm::PolishedDiorite, 1, 4, 16, 10), Sell(P_MASON, 3, itm::PolishedGranite, 1, 4, 16, 10),
    Buy(P_MASON, 4, itm::Quartz, 12, 12, 30), Sell(P_MASON, 4, itm::WhiteTerracotta, 1, 1, 12, 15, 5, 1),
    Sell(P_MASON, 4, itm::WhiteGlazedTerracotta, 1, 1, 12, 15, 5, 1),
    Sell(P_MASON, 5, itm::QuartzPillar, 1, 1, 12, 30), Sell(P_MASON, 5, itm::QuartzBlock, 1, 1, 12, 30),
};
static_assert(itm::BlackWool == itm::WhiteWool + 15 && itm::BlackCarpet == itm::WhiteCarpet + 15 &&
                  itm::BlackBed == itm::WhiteBed + 15 && itm::BlackBanner == itm::WhiteBanner + 15 &&
                  itm::BlackTerracotta == itm::WhiteTerracotta + 15 &&
                  itm::BlackGlazedTerracotta == itm::WhiteGlazedTerracotta + 15,
              "the 16 colours in a row");

// EnchantBookForEmeralds: a random tradeable enchantment at a random level; 2 to 4 + 10
// a level emeralds more, plus 3 a level, twice that for treasure, at most 64; and a book
bool enchantedBook(TradeOffer& o) {
    int pool[ench::COUNT], n = 0;
    for (int e = 0; e < ench::COUNT; e++)
        if (ENCHANTS[e].flags & EF_TRADEABLE) pool[n++] = e;
    if (!n) return false;
    Rng& r = mobs::rng();
    int e = pool[r.range(n)];
    int level = 1 + r.range(ENCHANTS[e].maxLevel);
    int cost = 2 + r.range(5 + level * 10) + 3 * level;
    if (ENCHANTS[e].flags & EF_TREASURE) cost *= 2;
    if (cost > 64) cost = 64;
    uint8_t lvl[ench::COUNT] = {};
    lvl[e] = (uint8_t)level;
    o.sell = ItemStack::of(itm::EnchantedBook);
    setItemEnchants(o.sell, lvl, true);
    o.buyA = ItemStack::of(itm::Emerald, cost);
    o.buyB = ItemStack::of(itm::Book);
    return true;
}

uint16_t boatOfType(uint8_t type) {
    switch (type) {
        case VT_TAIGA: case VT_SNOW: return itm::SpruceBoat;
        case VT_DESERT: case VT_JUNGLE: return itm::JungleBoat;
        case VT_SAVANNA: return itm::AcaciaBoat;
        case VT_SWAMP: return itm::DarkOakBoat;
        default: return itm::OakBoat;
    }
}

// The offer a listing makes (ItemListing#getOffer); false: none.
bool makeOffer(const Listing& l, int color, uint8_t type, TradeOffer& o) {
    o = TradeOffer();
    o.maxUses = l.uses;
    o.xp = l.xp;
    o.priceMult = l.pm / 100.0f;
    uint16_t item = (uint16_t)(l.item + color);
    Rng& r = mobs::rng();
    switch (l.kind) {
        case BUY:
            o.buyA = ItemStack::of(item, l.a);
            o.sell = ItemStack::of(itm::Emerald);
            return true;
        case SELL:
            o.buyA = ItemStack::of(itm::Emerald, l.a);
            o.sell = ItemStack::of(item, l.b);
            return true;
        case SWAP:
            o.buyA = ItemStack::of(itm::Emerald, l.c);
            o.buyB = ItemStack::of(item, l.a);
            o.sell = ItemStack::of(l.item2, l.b);
            return true;
        case ENCH_ITEM: {   // EnchantedItemForEmeralds: enchanted at 5 to 19, the price as much higher
            int level = 5 + r.range(15);
            o.sell = randomlyEnchanted(ItemStack::of(item), level);
            int cost = l.a + level;
            o.buyA = ItemStack::of(itm::Emerald, cost > 64 ? 64 : cost);
            return true;
        }
        case ENCH_BOOK: return enchantedBook(o);
        case TYPE_BOAT:   // EmeraldsForVillagerTypeItem
            o.buyA = ItemStack::of(boatOfType(type), l.a);
            o.sell = ItemStack::of(itm::Emerald);
            return true;
        case TIPPED: {   // TippedArrowForItemsAndEmeralds: a random potion with effects
            int pot[potion::COUNT], n = 0;
            for (int p = 0; p < potion::COUNT; p++)
                if (p != potion::Water && p != potion::Mundane && p != potion::Thick && p != potion::Awkward) pot[n++] = p;
            o.buyA = ItemStack::of(itm::Emerald, l.c);
            o.buyB = ItemStack::of(item, l.a);
            o.sell = potionItem(l.item2, pot[r.range(n)], l.b);
            return true;
        }
        case STEW:
            o.buyA = ItemStack::of(itm::Emerald, l.a);
            o.sell = ItemStack::of(item, l.b);
            return true;
        default: return false;
    }
}

// VillagerData's experience to reach the next level (NEXT_LEVEL_XP_THRESHOLDS)
const int LEVEL_XP[6] = {0, 10, 70, 150, 250, 250};

}  // namespace

// AbstractVillager#addOffersFromItemListings: two different listings of the level.
void addVillagerOffers(VillagerState& v, uint8_t type) {
    struct Pick { uint16_t listing; uint8_t color; };
    Pick pool[64];
    int n = 0;
    for (size_t i = 0; i < sizeof(LISTINGS) / sizeof(LISTINGS[0]); i++) {
        const Listing& l = LISTINGS[i];
        if (l.prof != v.profession || l.level != v.level) continue;
        for (int c = 0; c < (l.colors ? 16 : 1) && n < 64; c++) pool[n++] = {(uint16_t)i, (uint8_t)c};
    }
    int added = 0;
    while (added < 2 && n > 0) {
        int k = mobs::rng().range(n);
        Pick p = pool[k];
        pool[k] = pool[--n];
        TradeOffer o;
        if (makeOffer(LISTINGS[p.listing], p.color, type, o)) {
            v.trades.push_back(o);
            added++;
        }
    }
}

// MerchantOffer#getCostA: the first price with the demand (and a special price).
int tradeCostA(const TradeOffer& o) {
    int base = o.buyA.count;
    int extra = (int)floorf((float)(base * o.demand) * o.priceMult);
    if (extra < 0) extra = 0;
    int c = base + extra + o.special;
    int max = maxStack(o.buyA.id);
    return c < 1 ? 1 : c > max ? max : c;
}

uint8_t Server::villagerType(Entity& e) {
    if (!e.variant) {   // chosen once, by the biome it first stands in
        Chunk* c = world.peek(e.dim, (int)floor(e.x) >> 4, (int)floor(e.z) >> 4);
        uint8_t b = c ? c->biomeAt((int)floor(e.x) & 15, (int)floor(e.y), (int)floor(e.z) & 15) : biome::Plains;
        e.variant = (uint8_t)(typeOfBiome(b) + 1);
    }
    return (uint8_t)(e.variant - 1);
}

void Server::restoreVillager(Entity& e, const VillagerState& v) {
    e.villager = v;
    if (e.villager.profession >= P_COUNT) e.villager.profession = P_NONE;
}

void Server::writeVillagerData(Writer& w, Entity& e) {
    static_assert(meta::Villager::VillagerData == meta::ZombieVillager::VillagerData - 2, "a zombie villager's after its own");
    if (e.type == ent::ZombieVillager) {
        w.u8(meta::ZombieVillager::Converting);
        w.varint(mt::Boolean);
        w.boolean(e.villager.convertTicks > 0);
        w.u8(meta::ZombieVillager::VillagerData);
        w.varint(mt::VillagerData);
        w.varint(villagerType(e));
        w.varint(e.villager.profession);
        w.varint(e.villager.level);
        return;
    }
    w.u8(meta::Villager::VillagerData);
    w.varint(mt::VillagerData);
    w.varint(villagerType(e));
    w.varint(e.villager.profession);
    w.varint(e.villager.level);
}

static bool villagerTrades(const Entity& e) {
    return e.villager.profession != P_NONE && e.villager.profession != P_NITWIT && !e.villager.trades.empty();
}

// A free workstation for the villager within 16 blocks (4 up and down), the nearest:
// of its own profession if it has one.
bool Server::findJobSite(Entity& e, int& jx, int& jy, int& jz) {
    int ex = (int)floor(e.x), ey = (int)floor(e.y), ez = (int)floor(e.z);
    int want = e.villager.profession;
    double best = 1e18;
    bool found = false;
    auto isWorkstation = [&](uint16_t st) {
        int p = professionOf(blockIdOf(st));
        return p != P_NONE && (want == P_NONE || p == want);
    };
    for (int cx = (ex - 16) >> 4; cx <= (ex + 16) >> 4; cx++)
        for (int cz = (ez - 16) >> 4; cz <= (ez + 16) >> 4; cz++) {
            Chunk* c = world.peek(e.dim, cx, cz);
            if (!c) continue;
            for (int y0 = (ey - 4) & ~15; y0 <= ey + 4; y0 += 16) {
                if (!c->hasY(y0)) continue;
                const Section* sec = c->section(c->sectionIndex(y0));
                if (!sec || !sec->anyState(isWorkstation)) continue;   // (most sections: the palette says no)
                for (int i = 0; i < SECTION_BLOCKS; i++) {
                    if (!isWorkstation(sec->get(i))) continue;
                    int x = cx * 16 + (i & 15), y = y0 + (i >> 8), z = cz * 16 + ((i >> 4) & 15);
                    if (abs(x - ex) > 16 || abs(z - ez) > 16 || abs(y - ey) > 4) continue;
                    double d = (x - e.x) * (x - e.x) + (y - e.y) * (y - e.y) + (z - e.z) * (z - e.z);
                    if (d >= best) continue;
                    bool taken = false;   // another villager's job site
                    for (int k = 0; k < MC_MAX_ENTITIES && !taken; k++) {
                        const Entity& o = entities[k];
                        taken = &o != &e && o.kind == EK_MOB && !o.removed && o.type == ent::Villager && o.dim == e.dim &&
                                o.villager.hasJob && o.villager.jobX == x && o.villager.jobY == y && o.villager.jobZ == z;
                    }
                    if (taken) continue;
                    best = d;
                    jx = x; jy = y; jz = z;
                    found = true;
                }
            }
        }
    return found;
}

void Server::restockVillager(Entity& e) {
    VillagerState& v = e.villager;
    for (TradeOffer& o : v.trades) {   // MerchantOffer#updateDemand, then the uses back
        int d = o.demand + o.uses - (o.maxUses - o.uses);
        o.demand = (int16_t)(d < -1000 ? -1000 : d > 1000 ? 1000 : d);
        o.uses = 0;
    }
    v.lastRestock = worldTick();
    v.restocks++;
    if (e.tradingWith >= 0) sendTrades(players[e.tradingWith], e);
}

// What a villager does each tick besides walking (Villager#customServerAiStep and its
// brain's work): trading keeps it still, a level up waits for the trading to end, it
// finds and keeps a job site and restocks there. true: it stands still this tick.
bool Server::tickVillager(Entity& e) {
    VillagerState& v = e.villager;
    decayGossip(e);
    villagerFlee(e);
    villageGolem(e);
    if (e.tradingWith >= 0) {
        Player& p = players[e.tradingWith];
        if (!p.inPlay() || p.winKind != WK_MERCHANT || p.merchant != e.id) e.tradingWith = -1;
    }
    if (e.tradingWith < 0 && !e.levelUpPending && v.level < 5 && v.xp >= LEVEL_XP[v.level] && villagerTrades(e)) {
        e.levelUpPending = true;   // (earned before a restart: the level up was not saved)
        e.levelUpTimer = 40;
    }
    if (e.tradingWith < 0 && e.levelUpTimer > 0 && --e.levelUpTimer == 0 && e.levelUpPending) {
        e.levelUpPending = false;   // Villager#increaseMerchantCareer
        if (v.level < 5) {
            v.level++;
            addVillagerOffers(v, villagerType(e));
            e.metaDirty = true;
            broadcastStatus(e, 14);   // happy
        }
    }
    uint32_t phase = (uint32_t)e.id * 7u + e.age;
    if (phase % 40 == 0 && v.hasJob && world.isResident(e.dim, v.jobX >> 4, v.jobZ >> 4)) {
        // its workstation broken or replaced: no job site; one it never traded at, no profession
        int p = professionOf(blockIdOf(world.getBlock(e.dim, v.jobX, v.jobY, v.jobZ)));
        if (p != v.profession) {
            v.hasJob = false;
            if (v.xp == 0 && v.level == 1 && v.profession != P_NITWIT) {
                v.profession = P_NONE;
                v.trades.clear();
                e.metaDirty = true;
                if (e.tradingWith >= 0) closeWindow(players[e.tradingWith], true);
            }
        }
    }
    if (phase % 100 == 50 && !v.hasJob && v.profession != P_NITWIT) {
        int x, y, z;
        if (findJobSite(e, x, y, z)) {
            v.hasJob = true;
            v.jobX = x; v.jobY = y; v.jobZ = z;
            if (v.profession == P_NONE) {
                v.profession = (uint8_t)professionOf(blockIdOf(world.getBlock(e.dim, x, y, z)));
                v.level = 1;
                v.xp = 0;
                v.trades.clear();
                addVillagerOffers(v, villagerType(e));
                e.metaDirty = true;
                broadcastStatus(e, 14);
                char sound[48];
                snprintf(sound, sizeof(sound), "entity.villager.work_%s", PROF_NAME[v.profession]);
                playSound(sound, x + 0.5, y + 0.5, z + 0.5, 1, 1, 5);
            }
        }
    }
    // restocking at its job site in working hours (WorkAtPoi; at most twice a day, 2 minutes
    // apart), when something was traded
    if (phase % 20 == 10 && v.hasJob && villagerTrades(e)) {
        uint32_t day = (uint32_t)(meta.timeOfDay / 24000);
        if (day != v.restockDay) {
            v.restockDay = day;
            v.restocks = 0;
        }
        int64_t t = meta.timeOfDay % 24000;
        bool used = false;
        for (const TradeOffer& o : v.trades) used |= o.uses > 0;
        double dx = v.jobX + 0.5 - e.x, dz = v.jobZ + 0.5 - e.z;
        if (used && t >= 2000 && t < 9000 && dx * dx + dz * dz < 16 * 16 &&
            (v.restocks == 0 || (v.restocks < 2 && worldTick() > v.lastRestock + 2400)))
            restockVillager(e);
    }
    if (e.tradingWith >= 0) {   // it looks at its customer
        Player& p = players[e.tradingWith];
        double dx = p.e.x - e.x, dz = p.e.z - e.z;
        e.yaw = e.headYaw = (float)(atan2(-dx, dz) * 180.0 / M_PI);
        e.vx = e.vz = 0;
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ the merchant window
void Server::interactVillager(Player& p, Entity& e) {
    if (!villagerTrades(e) || e.health <= 0) {   // no trades: it shakes its head (Villager#setUnhappy)
        playSound("entity.villager.no", e.x, e.y, e.z, 1, 1, 5);
        return;
    }
    if (e.tradingWith >= 0 && e.tradingWith != p.slot) return;   // busy with someone else
    closeWindow(p, true);
    p.merchant = e.id;
    p.merchantOffer = -1;
    for (auto& c : p.craft) c.clear();
    p.winX = (int)floor(e.x); p.winY = (int)floor(e.y); p.winZ = (int)floor(e.z);
    e.tradingWith = (int8_t)p.slot;
    specialPrices(e, &p);
    char title[64];
    snprintf(title, sizeof(title), "entity.minecraft.villager.%s", PROF_NAME[e.villager.profession]);
    openWorkstation(p, WK_MERCHANT, menu::Merchant, title);
    sendTrades(p, e);
    playSound("entity.villager.trade", e.x, e.y, e.z, 1, 1, 5);
}

static void writeItemCost(Writer& w, const ItemStack& st) {
    w.varint(st.id);
    w.varint(st.count);
    w.varint(0);   // no components required
}

// ClientboundMerchantOffersPacket
void Server::sendTrades(Player& p, Entity& e) {
    const VillagerState& v = e.villager;
    p.conn.sendStreamed([&](Writer& w) {
        w.varint(pkt::s2c::TradeList);
        w.varint(p.winId);
        w.varint((int32_t)v.trades.size());
        for (const TradeOffer& o : v.trades) {
            writeItemCost(w, o.buyA);
            writeSlot(w, o.sell);
            w.boolean(!o.buyB.empty());
            if (!o.buyB.empty()) writeItemCost(w, o.buyB);
            w.boolean(o.uses >= o.maxUses);
            w.i32(o.uses);
            w.i32(o.maxUses);
            w.i32(o.xp);
            w.i32(o.special);
            w.f32(o.priceMult);
            w.i32(o.demand);
        }
        w.varint(v.level);
        w.varint(v.xp);
        w.boolean(true);   // a villager (its level and experience shown)
        w.boolean(true);   // it restocks
    });
}

Entity* Server::merchantOf(Player& p) {
    if (p.winKind != WK_MERCHANT) return nullptr;
    Entity* e = findEntity(p.merchant);
    return e && e->kind == EK_MOB && e->type == ent::Villager ? e : nullptr;
}

// MerchantOffer#satisfiedBy
static bool satisfies(const TradeOffer& o, const ItemStack& a, const ItemStack& b) {
    if (a.empty() || a.id != o.buyA.id || a.count < tradeCostA(o)) return false;
    if (o.buyB.empty()) return b.empty();
    return !b.empty() && b.id == o.buyB.id && b.count >= o.buyB.count;
}

// MerchantContainer#updateSellItem: the trade the payment pays for (the one selected first).
void Server::merchantChanged(Player& p) {
    Entity* e = merchantOf(p);
    p.craft[2].clear();
    p.merchantOffer = -1;
    if (!e) return;
    const std::vector<TradeOffer>& t = e->villager.trades;
    int found = -1;
    if (p.tradeHint > 0 && p.tradeHint < (int)t.size()) {
        if (satisfies(t[p.tradeHint], p.craft[0], p.craft[1])) found = p.tradeHint;
    } else {
        for (int i = 0; i < (int)t.size() && found < 0; i++)
            if (satisfies(t[i], p.craft[0], p.craft[1])) found = i;
    }
    if (found < 0 || t[found].uses >= t[found].maxUses) return;
    p.merchantOffer = (int8_t)found;
    p.craft[2] = t[found].sell;
}

bool Server::merchantMayTake(Player& p) { return merchantOf(p) && p.merchantOffer >= 0 && !p.craft[2].empty(); }

// MerchantResultSlot#onTake and Villager#rewardTradeXp: the payment used up, the trade
// counted, experience for both.
void Server::merchantTaken(Player& p) {
    Entity* e = merchantOf(p);
    if (!e || p.merchantOffer < 0) return;
    VillagerState& v = e->villager;
    TradeOffer& o = v.trades[p.merchantOffer];
    ItemStack *a = &p.craft[0], *b = &p.craft[1];
    if (!satisfies(o, *a, *b)) std::swap(a, b);
    if (!satisfies(o, *a, *b) || o.uses >= o.maxUses) {   // not paid for (any more)
        merchantChanged(p);
        return;
    }
    a->count = (uint8_t)(a->count - tradeCostA(o));
    if (!a->count) a->clear();
    if (!o.buyB.empty()) {
        b->count = (uint8_t)(b->count - o.buyB.count);
        if (!b->count) b->clear();
    }
    o.uses++;
    v.xp += o.xp;
    int reward = 3 + mobs::rng().range(4);
    if (v.level < 5 && v.xp >= LEVEL_XP[v.level] && !e->levelUpPending) {
        e->levelUpPending = true;   // after the trading (updateMerchantTimer)
        e->levelUpTimer = 40;
        reward += 5;
    }
    giveXp(p, reward);
    gossipTraded(*e, p);
    playSound("entity.villager.yes", e->x, e->y, e->z, 1, 1, 5);
    merchantChanged(p);
    sendTrades(p, *e);   // the uses and the experience bar
}

// ServerboundSelectTradePacket: the trade chosen; the payment moved in from the inventory
// (MerchantMenu#tryMoveItems).
void Server::selectTrade(Player& p, int index) {
    Entity* e = merchantOf(p);
    if (!e) return;
    p.tradeHint = (int16_t)index;
    const std::vector<TradeOffer>& t = e->villager.trades;
    if (index >= 0 && index < (int)t.size()) {
        for (int s = 0; s < 2; s++)   // what was in the payment slots back to the inventory first
            if (!p.craft[s].empty()) {
                int left = giveItem(p, p.craft[s]);
                if (left) {
                    p.craft[s].count = (uint8_t)left;
                    merchantChanged(p);
                    resendWindow(p);
                    return;
                }
                p.craft[s].clear();
            }
        const TradeOffer& o = t[index];
        for (int s = 0; s < 2; s++) {
            uint16_t want = s == 0 ? o.buyA.id : o.buyB.id;
            if (s == 1 && o.buyB.empty()) break;
            int max = maxStack(want);
            for (int i = SLOT_MAIN_START; i < SLOT_OFFHAND && p.craft[s].count < max; i++) {
                ItemStack& inv = p.inv[i];
                if (inv.empty() || inv.id != want) continue;
                if (!p.craft[s].empty() && !p.craft[s].sameItem(inv)) continue;
                int k = max - p.craft[s].count < inv.count ? max - p.craft[s].count : inv.count;
                if (p.craft[s].empty()) {
                    p.craft[s] = inv;
                    p.craft[s].count = 0;
                }
                p.craft[s].count = (uint8_t)(p.craft[s].count + k);
                inv.count = (uint8_t)(inv.count - k);
                if (!inv.count) inv.clear();
            }
        }
    }
    merchantChanged(p);
    resendWindow(p);
}

void Server::merchantClosed(Player& p) {
    Entity* e = findEntity(p.merchant);
    if (e && e->tradingWith == p.slot) {
        e->tradingWith = -1;
        specialPrices(*e, nullptr);
    }
    p.merchant = -1;
}

}  // namespace mc
