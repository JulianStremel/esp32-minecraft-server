#include "testing.h"
#include "mc/registry.h"

using namespace mc;

TEST(registry_sizes) {
    CHECK_EQ(NUM_BLOCKS, 1105);
    CHECK_EQ(NUM_STATES, 27946);
    CHECK_EQ(NUM_ITEMS, 1416);
    CHECK_EQ(blk::Stone, 1);
    CHECK_EQ(bs::Stone, 1);
    CHECK_EQ(bs::Air, 0);
}

TEST(state_to_block_is_consistent) {
    for (int b = 0; b < NUM_BLOCKS; b++) {
        const BlockDef& d = BLOCKS[b];
        CHECK(d.numStates >= 1);
        CHECK_EQ(STATE_TO_BLOCK[d.minState], b);
        CHECK_EQ(STATE_TO_BLOCK[d.minState + d.numStates - 1], b);
        CHECK(d.defState >= d.minState && d.defState < d.minState + d.numStates);
        int prod = 1;
        for (int i = 0; i < d.propCount; i++) prod *= PROPS[BLOCK_PROPS[d.propStart + i]].n;
        CHECK_EQ(prod, d.numStates);
    }
}

TEST(stairs_default_state_decodes_to_vanilla_defaults) {
    uint16_t s = bs::OakStairs;
    CHECK_STR(getPropStr(s, "facing"), "north");
    CHECK_STR(getPropStr(s, "half"), "bottom");
    CHECK_STR(getPropStr(s, "shape"), "straight");
    CHECK_STR(getPropStr(s, "waterlogged"), "false");
}

TEST(set_and_get_props_roundtrip) {
    uint16_t s = bs::OakStairs;
    s = setPropStr(s, "facing", "east");
    s = setPropStr(s, "half", "top");
    s = setPropStr(s, "shape", "outer_left");
    s = setBool(s, "waterlogged", true);
    CHECK_EQ(blockIdOf(s), blk::OakStairs);
    CHECK_STR(getPropStr(s, "facing"), "east");
    CHECK_STR(getPropStr(s, "half"), "top");
    CHECK_STR(getPropStr(s, "shape"), "outer_left");
    CHECK(getBool(s, "waterlogged"));
    CHECK_EQ(getProp(bs::Stone, "facing"), -1);
}

TEST(water_levels) {
    CHECK_EQ(blockIdOf(bs::Water), blk::Water);
    CHECK_EQ(getProp(bs::Water, "level"), 0);
    uint16_t flowing = setProp(bs::Water, "level", 5);
    CHECK_EQ(getProp(flowing, "level"), 5);
    CHECK(stateIsFluid(flowing));
}

TEST(lookup_by_name) {
    CHECK_EQ(findBlock("minecraft:stone"), blk::Stone);
    CHECK_EQ(findBlock("oak_log"), blk::OakLog);
    CHECK_EQ(findBlock("nonexistent"), -1);
    CHECK_EQ(findItem("diamond_pickaxe"), itm::DiamondPickaxe);
    CHECK_EQ(findEntityType("zombie"), ent::Zombie);
}

TEST(block_flags_and_light) {
    CHECK(stateCollides(bs::Stone));
    CHECK(!stateCollides(bs::Air));
    CHECK(!stateCollides(bs::Water));
    CHECK(stateIsFluid(bs::Water));
    CHECK(stateOpaque(bs::Stone));
    CHECK(!stateOpaque(bs::Glass));
    CHECK_EQ(BLOCKS[blk::Torch].emitLight, 14);
    CHECK_EQ(BLOCKS[blk::Glowstone].emitLight, 15);
    CHECK(BLOCKS[blk::Sand].flags & BF_GRAVITY);
    CHECK(BLOCKS[blk::Dandelion].flags & BF_NEEDS_SUPPORT);
}

TEST(drops_and_tools) {
    CHECK_EQ(BLOCKS[blk::Stone].dropItem, itm::Cobblestone);
    CHECK_EQ(BLOCKS[blk::GrassBlock].dropItem, itm::Dirt);
    CHECK_EQ(BLOCKS[blk::OakLog].dropItem, itm::OakLog);
    CHECK_EQ(BLOCKS[blk::DiamondOre].dropItem, itm::Diamond);
    CHECK_EQ(BLOCKS[blk::Stone].toolClass, TC_PICKAXE);
    CHECK_EQ(BLOCKS[blk::DiamondOre].minTier, 3);   // needs an iron pickaxe
    CHECK_EQ(BLOCKS[blk::OakLog].toolClass, TC_AXE);
    CHECK_EQ(BLOCKS[blk::Dirt].toolClass, TC_SHOVEL);
    CHECK_EQ(ITEMS[itm::DiamondPickaxe].kind, IK_PICKAXE);
    CHECK_EQ(ITEMS[itm::DiamondSword].attack, 7);
    CHECK_EQ(ITEMS[itm::IronChestplate].armor, 6);
    CHECK_EQ(ITEMS[itm::Bread].food, 5);
    CHECK_EQ(ITEMS[itm::EnderPearl].stack, 16);
    CHECK_EQ(ITEMS[itm::Stone].block, blk::Stone);
}

TEST(packet_ids_match_known_values) {
    // 1.21.8 (protocol 772)
    CHECK_EQ(PROTOCOL_VERSION, 772);
    CHECK_EQ(pkt::s2c::KeepAlive, 0x26);
    CHECK_EQ(pkt::s2c::MapChunk, 0x27);
    CHECK_EQ(pkt::s2c::Login, 0x2b);
    CHECK_EQ(pkt::c2s::BlockPlace, 0x3f);
    CHECK_EQ(pkt::c2s::BlockDig, 0x28);
    CHECK_EQ(pkt::cfg_s2c::RegistryData, 0x07);
    CHECK_EQ(pkt::cfg_c2s::FinishConfiguration, 0x03);
    CHECK_EQ(pkt::login_c2s::LoginAcknowledged, 0x03);
}

TEST(recipes_present) {
    CHECK(NUM_RECIPES > 1000);
    bool found = false;
    for (int i = 0; i < NUM_RECIPES; i++)
        if (RECIPES[i].result == itm::CraftingTable) { found = true; CHECK_EQ(RECIPES[i].w, 2); CHECK_EQ(RECIPES[i].h, 2); }
    CHECK(found);
}

TEST(mob_types_exclude_objects) {
    int mobs = 0;
    for (int i = 0; i < NUM_ENTITY_TYPES; i++) mobs += isMobType(ENTITY_TYPES[i].id);
    CHECK_EQ(mobs, 84);   // the 1.21.8 mobs (allay ... zombified_piglin)
    CHECK(isMobType(findEntityType("zombie")));
    CHECK(isMobType(findEntityType("minecraft:bee")));
    CHECK(!isMobType(findEntityType("boat")));
    CHECK(!isMobType(findEntityType("arrow")));
    CHECK(!isMobType(findEntityType("painting")));
    CHECK(!isMobType(findEntityType("evoker_fangs")));
    CHECK(!isMobType(-1));
}
