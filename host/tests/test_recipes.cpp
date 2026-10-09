// Crafting and cooking from the data pack's recipes: tags as ingredient sets, transmute,
// and which block cooks what.
#include <string.h>
#include <string>
#include "testing.h"
#include "mc/bytebuf.h"
#include "mc/item.h"
#include "mc/nbt.h"
#include "mc/platform.h"
#include "mc/registry.h"
#include "mc/server/automation.h"

using namespace mc;

namespace {
ItemStack craft(std::initializer_list<uint16_t> cells) {   // a 3x3 grid, 0 = empty
    ItemStack grid[9];
    int i = 0;
    for (uint16_t id : cells) {
        if (id) grid[i] = ItemStack::of(id);
        i++;
    }
    return matchCraftingRecipe(grid, 3);
}
int firstOf(uint16_t set) { return INGREDIENT_ITEMS[INGREDIENT_SET_START[set]]; }
}  // namespace

TEST(crafting_takes_any_item_of_a_tag) {
    const uint16_t P = itm::SprucePlanks, C = itm::CherryPlanks, M = itm::MangrovePlanks;
    CHECK_EQ(craft({P, P, P, P, 0, P, P, P, P}).id, itm::Chest);
    CHECK_EQ(craft({P, C, M, M, 0, P, C, P, P}).id, itm::Chest);   // mixed woods too
    CHECK_EQ(craft({C, C, 0, C, C, 0, 0, 0, 0}).id, itm::CraftingTable);
    CHECK_EQ(craft({0, 0, 0, 0, C, C, 0, C, C}).id, itm::CraftingTable);   // anywhere in the grid
    ItemStack sticks = craft({M, 0, 0, M, 0, 0, 0, 0, 0});
    CHECK(sticks.id == itm::Stick && sticks.count == 4);
    CHECK_EQ(craft({itm::BirchLog, 0, 0, 0, 0, 0, 0, 0, 0}).id, itm::BirchPlanks);
    CHECK_EQ(craft({itm::StrippedBirchWood, 0, 0, 0, 0, 0, 0, 0, 0}).id, itm::BirchPlanks);
    // stone tools from any stone-crafting material
    CHECK_EQ(craft({itm::CobbledDeepslate, itm::CobbledDeepslate, itm::CobbledDeepslate, 0, itm::Stick, 0, 0, itm::Stick, 0}).id,
             itm::StonePickaxe);
    CHECK_EQ(craft({itm::Blackstone, itm::Blackstone, 0, itm::Blackstone, itm::Stick, 0, 0, itm::Stick, 0}).id, itm::StoneAxe);
    // dyeing from any colour, shapeless
    CHECK_EQ(craft({0, itm::BlueWool, 0, 0, 0, 0, itm::RedDye, 0, 0}).id, itm::RedWool);
    CHECK_EQ(craft({itm::RedDye, itm::GreenBed, 0, 0, 0, 0, 0, 0, 0}).id, itm::RedBed);
    // nothing for a wrong shape or an extra item
    CHECK(craft({P, P, P, P, 0, P, P, P, 0}).empty());
    CHECK(craft({itm::BirchLog, itm::Dirt, 0, 0, 0, 0, 0, 0, 0}).empty());
}

TEST(crafting_transmute_keeps_the_contents) {
    ByteBuf b;
    Writer w(b);
    NbtWriter n(w);
    n.beginRoot();
    n.str("note", "my things");
    n.end();
    ItemStack box = ItemStack::of(itm::ShulkerBox);
    CHECK(box.setTag(b.data(), b.size()));
    ItemStack grid[9];
    grid[4] = box;
    grid[7] = ItemStack::of(itm::LimeDye);
    ItemStack out = matchCraftingRecipe(grid, 3);
    CHECK_EQ(out.id, itm::LimeShulkerBox);
    CHECK_EQ(out.count, 1);
    CHECK(out.tagSize() == box.tagSize() && !memcmp(out.tagData(), box.tagData(), box.tagSize()));
    // the dye first, a coloured box re-dyed
    grid[4] = ItemStack::of(itm::RedDye);
    grid[7] = ItemStack::of(itm::BlueShulkerBox);
    CHECK_EQ(matchCraftingRecipe(grid, 3).id, itm::RedShulkerBox);
}

// Every recipe, with every item its ingredients allow in turn (the others at their
// first): the matcher must give that recipe's result. Catches a recipe another one
// shadows as well as a tag item that does not work.
TEST(every_recipe_crafts_with_every_item_of_its_ingredients) {
    int combos = 0, wrong = 0;
    uint64_t t0 = plat::micros();
    for (int r = 0; r < NUM_RECIPES; r++) {
        const RecipeDef& rd = RECIPES[r];
        const uint16_t* ing = RECIPE_INGREDIENTS + rd.start;
        int cells = rd.kind == RECIPE_SHAPED ? rd.w * rd.h : rd.w;
        for (int vary = 0; vary < cells; vary++) {
            uint16_t set = ing[vary];
            if (!set) continue;
            for (int k = INGREDIENT_SET_START[set]; k < INGREDIENT_SET_START[set + 1]; k++) {
                ItemStack grid[9];
                for (int c = 0; c < cells; c++) {
                    if (!ing[c]) continue;
                    uint16_t id = c == vary ? INGREDIENT_ITEMS[k] : firstOf(ing[c]);
                    int slot = rd.kind == RECIPE_SHAPED ? (c / rd.w) * 3 + c % rd.w : c;
                    grid[slot] = ItemStack::of(id);
                }
                // transmute: an input already of the result's kind is no recipe
                if (rd.kind == RECIPE_TRANSMUTE && grid[0].id == rd.result) continue;
                combos++;
                ItemStack got = matchCraftingRecipe(grid, 3);
                if (got.id != rd.result || got.count != rd.count) {
                    if (wrong++ < 10)
                        printf("    recipe %d (%s): with %s got %s\n", r, ITEMS[rd.result].name, ITEMS[INGREDIENT_ITEMS[k]].name,
                               got.empty() ? "nothing" : ITEMS[got.id].name);
                }
            }
        }
    }
    printf("    %d recipes, %d combinations, %d wrong, %.1f us a match\n", NUM_RECIPES, combos, wrong,
           (double)(plat::micros() - t0) / (combos ? combos : 1));
    CHECK(combos > 8000);
    CHECK_EQ(wrong, 0);
}

TEST(cooking_depends_on_the_block) {
    auto out = [](uint16_t in, uint8_t kind) {
        const CookingDef* r = cookingRecipe(in, kind);
        return r ? r->out : 0;
    };
    CHECK_EQ(out(itm::RawIron, COOK_FURNACE), itm::IronIngot);
    CHECK_EQ(out(itm::RawIron, COOK_BLAST), itm::IronIngot);
    CHECK_EQ(out(itm::RawGold, COOK_FURNACE), itm::GoldIngot);
    CHECK_EQ(out(itm::RawCopper, COOK_BLAST), itm::CopperIngot);
    CHECK_EQ(out(itm::DeepslateIronOre, COOK_FURNACE), itm::IronIngot);
    CHECK_EQ(out(itm::IronPickaxe, COOK_FURNACE), itm::IronNugget);   // tools melt down
    CHECK_EQ(out(itm::CherryLog, COOK_FURNACE), itm::Charcoal);
    CHECK_EQ(out(itm::Beef, COOK_FURNACE), itm::CookedBeef);
    CHECK_EQ(out(itm::Beef, COOK_SMOKER), itm::CookedBeef);
    CHECK_EQ(out(itm::Beef, COOK_CAMPFIRE), itm::CookedBeef);
    CHECK_EQ(out(itm::Beef, COOK_BLAST), 0);       // a blast furnace does not cook food
    CHECK_EQ(out(itm::RawIron, COOK_SMOKER), 0);   // nor a smoker ore
    CHECK_EQ(out(itm::Dirt, COOK_FURNACE), 0);
    const CookingDef* r = cookingRecipe(itm::RawIron, COOK_FURNACE);
    CHECK(r && r->xpCenti == 70);
}

TEST(fuels_are_vanillas) {
    CHECK_EQ(fuelBurnTicks(itm::Coal), 1600);
    CHECK_EQ(fuelBurnTicks(itm::LavaBucket), 20000);
    CHECK_EQ(fuelBurnTicks(itm::OakStairs), 300);       // none of these burned before
    CHECK_EQ(fuelBurnTicks(itm::Ladder), 300);
    CHECK_EQ(fuelBurnTicks(itm::SpruceDoor), 200);
    CHECK_EQ(fuelBurnTicks(itm::OakHangingSign), 800);
    CHECK_EQ(fuelBurnTicks(itm::BirchChestBoat), 1200);
    CHECK_EQ(fuelBurnTicks(itm::RedCarpet), 67);
    CHECK_EQ(fuelBurnTicks(itm::OakSlab), 150);
    CHECK_EQ(fuelBurnTicks(itm::CrimsonPlanks), 0);     // nether wood does not burn
    CHECK_EQ(fuelBurnTicks(itm::WarpedStem), 0);
    CHECK_EQ(fuelBurnTicks(itm::Dirt), 0);
}
