#include "testing.h"
#include "mc/server/server.h"
#include "mc/server/piston.h"
#include "mc/server/automation.h"
#include "mc/server/books.h"
#include "mc/nbt.h"
#include "mc/registry.h"
#include "mc/world/light.h"
#include <memory>
using namespace mc;
namespace {
struct CircuitConn : Conn {
    int read(uint8_t*, size_t) override { return 0; }
    int write(const uint8_t*, size_t n) override { return (int)n; }
    bool connected() override { return true; }
    void close() override {}
};
struct Clock : TickSource {
    uint32_t n = 0;
    uint32_t take() override {
        uint32_t v = n;
        n = 0;
        return v;
    }
    uint32_t msUntilNext() override { return 0; }
};
struct Circuit {
    Clock clock;
    std::unique_ptr<Server> s{new Server};
    Circuit() {
        ServerConfig cfg;
        cfg.port = 0;
        cfg.worldType = WORLD_FLAT;
        cfg.spawnMobs = false;
        cfg.seed = 1;
        cfg.workerThreads = 0;
        CHECK(s->begin(cfg, nullptr));
        s->setTickSource(&clock);
        for (int z = -3; z <= 3; ++z)
            for (int x = -3; x <= 20; ++x)
                s->world.setBlock(0, x, 79, z, bs::Stone, false);
    }
    void put(int x, int z, uint16_t st) { s->setBlock(x, 80, z, st); }
    uint16_t at(int x, int z = 0) { return s->blockAt(x, 80, z); }
    void tick(int n = 1) {
        while (n--) {
            clock.n = 1;
            s->loop();
        }
    }
};
uint16_t eastRepeater(int delay = 1) {
    return setProp(setPropStr(bs::Repeater, "facing", "west"), "delay", delay - 1);
}
uint16_t wire() {
    uint16_t st = bs::RedstoneWire;
    for (const char* d : {"north", "south", "east", "west"})
        st = setPropStr(st, d, "side");
    return st;
}
} // namespace
TEST(redstone_wire_attenuates_and_clears_across_chunk_border) {
    Circuit c;
    for (int x = 0; x < 17; ++x)
        c.put(x, 0, wire());
    c.put(-1, 0, bs::RedstoneBlock);
    for (int x = 0; x < 17; ++x)
        CHECK_EQ(getProp(c.at(x), "power"), x < 15 ? 15 - x : 0);
    c.put(-1, 0, bs::Air);
    for (int x = 0; x < 17; ++x)
        CHECK_EQ(getProp(c.at(x), "power"), 0);
    CHECK_EQ(c.s->redstone.failures, 0u);
}
TEST(redstone_weak_and_strong_power_do_not_propagate_through_two_cubes) {
    Circuit c;
    c.put(0, 0, bs::Stone);
    c.put(1, 0, bs::Stone);
    c.s->setBlock(0, 81, 0, setBool(setPropStr(bs::Lever, "face", "floor"), "powered", true));
    CHECK_EQ(c.s->redstone.signal(*c.s, 0, 80, 0, 4), 15);
    CHECK_EQ(c.s->redstone.signal(*c.s, 1, 80, 0, 4), 0);
    c.s->setBlock(0, 81, 0, bs::Air);
    c.put(-1, 0, bs::RedstoneBlock);
    CHECK_EQ(c.s->redstone.signal(*c.s, 0, 80, 0, 4), 0);
}
TEST(redstone_repeater_delays_and_stretches_a_short_pulse) {
    for (int delay = 1; delay <= 4; ++delay) {
        Circuit c;
        c.put(0, 0, eastRepeater(delay));
        c.put(-1, 0, bs::RedstoneBlock);
        c.tick();
        c.put(-1, 0, bs::Air);
        CHECK(!getBool(c.at(0), "powered"));
        c.tick(delay * 2 - 1);
        CHECK(getBool(c.at(0), "powered"));
        c.tick(delay * 2 - 1);
        CHECK(getBool(c.at(0), "powered"));
        c.tick();
        CHECK(!getBool(c.at(0), "powered"));
    }
}
TEST(redstone_repeater_lock_accepts_diodes_but_not_dust) {
    Circuit c;
    c.put(0, 0, eastRepeater());
    c.put(-1, 0, bs::RedstoneBlock);
    c.tick(2);
    c.put(0, -1, setPropStr(bs::Repeater, "facing", "north"));
    c.put(0, -2, bs::RedstoneBlock);
    c.tick(2);
    CHECK(getBool(c.at(0), "locked"));
    c.put(-1, 0, bs::Air);
    c.tick(8);
    CHECK(getBool(c.at(0), "powered"));
    c.put(0, -2, bs::Air);
    c.tick(2);
    CHECK(!getBool(c.at(0), "locked"));
    c.tick(2);
    CHECK(!getBool(c.at(0), "powered"));
    c.put(0, -1, wire());
    c.put(0, -2, bs::RedstoneBlock);
    CHECK(!getBool(c.at(0), "locked"));
}
TEST(redstone_observer_only_watches_front_and_pulses_for_two_ticks) {
    Circuit c;
    c.put(0, 0, setPropStr(bs::Observer, "facing", "west"));
    c.put(0, 1, bs::Stone);
    c.tick(3);
    CHECK(!getBool(c.at(0), "powered"));
    c.put(-1, 0, bs::Stone);
    c.tick();
    CHECK(!getBool(c.at(0), "powered"));
    c.tick();
    CHECK(getBool(c.at(0), "powered"));
    CHECK_EQ(c.s->redstone.signal(*c.s, 0, 80, 0, 4), 15);
    CHECK_EQ(c.s->redstone.signal(*c.s, 0, 80, 0, 5), 0);
    c.tick();
    CHECK(getBool(c.at(0), "powered"));
    c.tick();
    CHECK(!getBool(c.at(0), "powered"));
}
TEST(redstone_lamp_switches_on_immediately_and_off_after_four_ticks) {
    Circuit c;
    c.put(0, 0, bs::RedstoneLamp);
    CHECK_EQ(stateEmission(c.at(0)), 0);
    c.put(-1, 0, bs::RedstoneBlock);
    CHECK(getBool(c.at(0), "lit"));
    CHECK_EQ(stateEmission(c.at(0)), 15);
    c.put(-1, 0, bs::Air);
    c.tick(3);
    CHECK(getBool(c.at(0), "lit"));
    c.tick();
    CHECK(!getBool(c.at(0), "lit"));
    c.put(-1, 0, bs::RedstoneBlock);
    c.put(-1, 0, bs::Air);
    c.tick(2);
    c.put(-1, 0, bs::RedstoneBlock);
    c.tick(2);
    CHECK(getBool(c.at(0), "lit"));
}
TEST(redstone_torch_inverts_support_power_after_two_ticks) {
    Circuit c;
    c.put(0, 0, bs::Stone);
    c.s->setBlock(0, 81, 0, bs::RedstoneTorch);
    c.put(-1, 0, setPropStr(setPropStr(bs::Lever, "face", "wall"), "facing", "west"));
    bool handled = false;
    c.s->interactBlock(c.s->players[0], -1, 80, 0, c.at(-1), handled);
    CHECK(getBool(c.s->blockAt(0, 81, 0), "lit"));
    c.tick();
    CHECK(getBool(c.s->blockAt(0, 81, 0), "lit"));
    c.tick();
    CHECK(!getBool(c.s->blockAt(0, 81, 0), "lit"));
    c.put(-1, 0, bs::Air);
    c.tick(2);
    CHECK(getBool(c.s->blockAt(0, 81, 0), "lit"));
}
TEST(redstone_comparator_analog_compare_and_subtract) {
    Circuit c;
    c.put(0, 0, setPropStr(bs::Comparator, "facing", "west"));
    c.put(-1, 0, bs::Cake);
    c.tick(2);
    CHECK_EQ(c.s->redstone.signal(*c.s, 0, 80, 0, 4), 14);
    c.put(0, -1, bs::RedstoneBlock);
    c.tick(2);
    CHECK_EQ(c.s->redstone.signal(*c.s, 0, 80, 0, 4), 0);
    c.put(0, 0, setPropStr(c.at(0), "mode", "subtract"));
    c.tick(2);
    CHECK_EQ(c.s->redstone.signal(*c.s, 0, 80, 0, 4), 0);
    c.put(0, -1, bs::Air);
    c.tick(2);
    CHECK_EQ(c.s->redstone.signal(*c.s, 0, 80, 0, 4), 14);
    c.put(-1, 0, setProp(bs::Cake, "bites", 6));
    c.tick(2);
    CHECK_EQ(c.s->redstone.signal(*c.s, 0, 80, 0, 4), 2);
}
TEST(redstone_idle_circuits_do_not_scan_blocks_each_tick) {
    Circuit c;
    c.put(0, 0, wire());
    c.put(-1, 0, bs::RedstoneBlock);
    c.tick(10);
    auto before = c.s->redstone.updates;
    c.tick(100);
    CHECK_EQ(c.s->redstone.updates, before);
}
TEST(redstone_scheduling_marks_an_unchanged_chunk_dirty) {
    Circuit c;
    c.put(0, 0, bs::Observer);
    Chunk* ch = c.s->world.get(0, 0, 0);
    ch->dirty = false;
    c.s->scheduleTick(0, 80, 0, 2);
    CHECK(ch->dirty);
}
TEST(redstone_dimensions_have_separate_power_and_timers) {
    Circuit c;
    c.put(0, 0, bs::RedstoneLamp);
    {
        Server::InDim in(*c.s, DIM_NETHER);
        c.s->world.load(DIM_NETHER, 0, 0);
        c.put(0, 0, bs::RedstoneLamp);
        c.put(1, 0, bs::RedstoneBlock);
        CHECK(getBool(c.at(0), "lit"));
    }
    CHECK(!getBool(c.at(0), "lit"));
}

TEST(redstone_torch_burnout_requires_eight_off_transitions_and_recovers) {
    Circuit c;
    c.put(0, 0, bs::Stone);
    c.s->setBlock(0, 81, 0, bs::RedstoneTorch);
    uint16_t lever = setPropStr(setPropStr(bs::Lever, "face", "wall"), "facing", "west");
    c.put(-1, 0, lever);
    bool handled = false;
    for (int i = 0; i < 8; ++i) {
        c.s->interactBlock(c.s->players[0], -1, 80, 0, c.at(-1), handled);
        c.tick(2);
        CHECK(!getBool(c.s->blockAt(0, 81, 0), "lit"));
        c.s->interactBlock(c.s->players[0], -1, 80, 0, c.at(-1), handled);
        if (i < 7) {
            c.tick(2);
            CHECK(getBool(c.s->blockAt(0, 81, 0), "lit"));
        }
    }
    c.tick(159);
    CHECK(!getBool(c.s->blockAt(0, 81, 0), "lit"));
    c.tick();
    CHECK(getBool(c.s->blockAt(0, 81, 0), "lit"));
}
TEST(redstone_inventory_changes_reach_comparators_through_a_cube) {
    Circuit c;
    c.put(0, 0, setPropStr(bs::Comparator, "facing", "west"));
    c.put(-1, 0, bs::Stone);
    c.put(-2, 0, bs::Barrel);
    Chunk* ch = c.s->world.get(0, -1, 0);
    TileEntity* t = ch->addTile(TILE_BARREL, 14, 80, 0);
    t->items[0] = ItemStack::of(itm::Stone, 64);
    c.s->containerChanged(-2, 80, 0);
    c.tick(2);
    CHECK_EQ(c.s->redstone.signal(*c.s, 0, 80, 0, 4), 1);
    for (auto& stack : t->items)
        stack = ItemStack::of(itm::Stone, 64);
    c.s->containerChanged(-2, 80, 0);
    c.tick(2);
    CHECK_EQ(c.s->redstone.signal(*c.s, 0, 80, 0, 4), 15);
    for (auto& stack : t->items)
        stack.clear();
    c.s->containerChanged(-2, 80, 0);
    c.tick(2);
    CHECK_EQ(c.s->redstone.signal(*c.s, 0, 80, 0, 4), 0);
}
TEST(redstone_more_than_256_due_events_keep_their_tick) {
    Circuit c;
    for (int i = 0; i < 300; ++i) {
        int x = (i % 20) * 2, z = (i / 20) * 2;
        c.s->world.setBlock(0, x, 100, z, bs::Observer, false);
        c.s->scheduleTick(x, 100, z, 2);
    }
    c.tick(2);
    for (int i = 0; i < 300; ++i)
        CHECK(getBool(c.s->blockAt((i % 20) * 2, 100, (i / 20) * 2), "powered"));
    c.tick(2);
    for (int i = 0; i < 300; ++i)
        CHECK(!getBool(c.s->blockAt((i % 20) * 2, 100, (i / 20) * 2), "powered"));
}
TEST(redstone_saved_ticks_preserve_order_after_timer_slot_reuse) {
    Circuit c;
    for (int x = 1; x <= 4; ++x) {
        c.s->world.setBlock(0, x, 100, 0, bs::Observer, false);
        if (x < 4) c.s->scheduleTick(x, 100, 0, 10);
    }
    c.s->timers.cancel(TimerKey::block(1, 100, 0, blk::Observer, 0));
    c.s->scheduleTick(4, 100, 0, 10);
    Chunk saved(0, 0);
    c.s->attachTicks(saved);
    CHECK_EQ(saved.tickCount, 3);
    if (saved.tickCount == 3) {
        CHECK_EQ(saved.ticks[0].lx, 2);
        CHECK_EQ(saved.ticks[1].lx, 3);
        CHECK_EQ(saved.ticks[2].lx, 4);
    }
}
TEST(redstone_emission_uses_state_in_the_actual_light_solver) {
    Chunk ch(0, 0);
    ChunkLight light;
    ch.set(8, 8, 8, bs::RedstoneLamp);
    CHECK(light.compute(ch, nullptr));
    uint32_t i = (8 << 8) | (8 << 4) | 9;
    CHECK_EQ((light.block(0)[i >> 1] >> ((i & 1) * 4)) & 15, 0);
    ch.set(8, 8, 8, setBool(bs::RedstoneLamp, "lit", true));
    CHECK(light.compute(ch, nullptr));
    CHECK_EQ((light.block(0)[i >> 1] >> ((i & 1) * 4)) & 15, 14);
    ch.set(8, 8, 8, bs::RedstoneLamp);
    CHECK(light.compute(ch, nullptr));
    CHECK_EQ((light.block(0)[i >> 1] >> ((i & 1) * 4)) & 15, 0);
}

TEST(redstone_conduction_is_independent_of_light_opacity) {
    for (uint16_t st : {bs::Furnace, bs::RedstoneLamp, bs::Stone})
        CHECK(stateConductsRedstone(st));
    for (uint16_t st : {bs::Glass, bs::Glowstone, bs::OakSlab, bs::HoneyBlock, bs::Observer})
        CHECK(!stateConductsRedstone(st));
    CHECK(stateConductsRedstone(setPropStr(bs::OakSlab, "type", "double")));
    CHECK(!stateFaceSturdy(bs::OakSlab, 1));
    CHECK(stateFaceSturdy(setPropStr(bs::OakSlab, "type", "top"), 1));
    CHECK(stateFaceSturdy(bs::Glass, 1));
    Circuit c;
    c.put(0, 0, bs::RedstoneLamp);
    c.s->setBlock(0, 81, 0, setBool(setPropStr(bs::Lever, "face", "floor"), "powered", true));
    CHECK_EQ(c.s->redstone.signal(*c.s, 0, 80, 0, 4), 15);
}
TEST(redstone_replacing_directional_sources_with_other_sources_is_safe) {
    Circuit c;
    c.put(0, 0, setBool(setPropStr(bs::Lever, "face", "floor"), "powered", true));
    c.put(0, 0, bs::RedstoneBlock);
    c.put(0, 0, bs::Observer);
    c.put(0, 0, bs::RedstoneBlock);
    CHECK_EQ(c.s->redstone.signal(*c.s, 0, 80, 0, 4), 15);
}
TEST(redstone_changed_analog_provider_notifies_through_a_conductor) {
    Circuit c;
    c.put(0, 0, setPropStr(bs::Comparator, "facing", "west"));
    c.put(-1, 0, bs::Stone);
    c.put(-2, 0, bs::Cake);
    c.tick(2);
    CHECK_EQ(c.s->redstone.signal(*c.s, 0, 80, 0, 4), 14);
    c.put(-2, 0, setProp(bs::Cake, "bites", 5));
    c.tick(2);
    CHECK_EQ(c.s->redstone.signal(*c.s, 0, 80, 0, 4), 4);
}
TEST(redstone_button_release_propagates_through_its_support) {
    for (uint16_t base : {bs::StoneButton, bs::PolishedBlackstoneButton, bs::OakButton, bs::CrimsonButton}) {
        Circuit c;
        c.put(0, 0, bs::Stone);
        c.put(1, 0, bs::RedstoneLamp);
        uint16_t button = setPropStr(base, "face", "floor");
        c.s->setBlock(0, 81, 0, button);
        bool handled = false;
        c.s->interactBlock(c.s->players[0], 0, 81, 0, button, handled);
        CHECK(handled);
        CHECK(getBool(c.at(1), "lit"));
        int duration =
            blockIdOf(base) == blk::StoneButton || blockIdOf(base) == blk::PolishedBlackstoneButton ? 20 : 30;
        c.tick(duration - 1);
        CHECK(getBool(c.s->blockAt(0, 81, 0), "powered"));
        c.tick();
        CHECK(!getBool(c.s->blockAt(0, 81, 0), "powered"));
        c.tick(4);
        CHECK(!getBool(c.at(1), "lit"));
    }
}
TEST(redstone_comparators_from_older_worlds_materialize_their_output) {
    Circuit c;
    c.s->world.setBlock(0, 0, 80, 0, setPropStr(bs::Comparator, "facing", "west"), false);
    CHECK(c.s->world.get(0, 0, 0)->tileAt(0, 80, 0) == nullptr);
    c.put(-1, 0, bs::RedstoneBlock);
    c.tick(2);
    CHECK_EQ(c.s->redstone.signal(*c.s, 0, 80, 0, 4), 15);
    TileEntity* t = c.s->world.get(0, 0, 0)->tileAt(0, 80, 0);
    CHECK(t && t->type == TILE_COMPARATOR && t->signal == 15);
}
TEST(redstone_gates_and_hopper_locks_do_not_require_a_half_property) {
    Circuit c;
    c.put(0, 0, bs::OakFenceGate);
    c.put(2, 0, bs::Hopper);
    c.put(1, 0, bs::RedstoneBlock);
    CHECK(getBool(c.at(0), "powered"));
    CHECK(getBool(c.at(0), "open"));
    CHECK(!getBool(c.at(2), "enabled"));
    c.put(1, 0, bs::Air);
    CHECK(!getBool(c.at(0), "powered"));
    CHECK(!getBool(c.at(0), "open"));
    CHECK(getBool(c.at(2), "enabled"));
}
TEST(redstone_note_block_events_are_deferred_deduplicated_and_edge_triggered) {
    Circuit c;
    c.put(0, 0, bs::NoteBlock);
    CHECK_STR(getPropStr(c.at(0), "instrument"), "basedrum");
    c.put(1, 0, bs::RedstoneBlock);
    CHECK_EQ(c.s->redstone.blockEventsExecuted, 0);
    CHECK_EQ(c.s->redstone.pendingBlockEvents(), 1);
    CHECK(c.s->redstone.pinsChunk(0, 0, 0));
    c.put(1, 0, bs::Air);
    c.put(1, 0, bs::RedstoneBlock);
    CHECK_EQ(c.s->redstone.pendingBlockEvents(), 1);
    c.tick();
    CHECK_EQ(c.s->redstone.blockEventsExecuted, 1);
    CHECK_EQ(c.s->redstone.pendingBlockEvents(), 0);
    CHECK(!c.s->redstone.pinsChunk(0, 0, 0));
    c.tick(5);
    CHECK_EQ(c.s->redstone.blockEventsExecuted, 1);
    c.put(1, 0, bs::Air);
    c.put(1, 0, bs::RedstoneBlock);
    c.tick();
    CHECK_EQ(c.s->redstone.blockEventsExecuted, 2);
}
TEST(redstone_note_block_checks_cover_when_queued_and_block_type_when_executed) {
    Circuit c;
    c.put(0, 0, bs::NoteBlock);
    c.s->setBlock(0, 81, 0, bs::Stone);
    c.s->redstone.playNote(*c.s, 0, 80, 0);
    CHECK_EQ(c.s->redstone.pendingBlockEvents(), 0);
    c.s->setBlock(0, 81, 0, bs::Air);
    c.s->redstone.playNote(*c.s, 0, 80, 0);
    c.s->setBlock(0, 81, 0, bs::Stone);
    c.tick();
    CHECK_EQ(c.s->redstone.blockEventsExecuted, 1);
    c.s->setBlock(0, 81, 0, bs::Air);
    c.s->redstone.playNote(*c.s, 0, 80, 0);
    c.put(0, 0, bs::Stone);
    c.tick();
    CHECK_EQ(c.s->redstone.blockEventsExecuted, 1);
}
TEST(redstone_note_instrument_changes_follow_the_support_material) {
    Circuit c;
    c.put(0, 0, bs::NoteBlock);
    c.s->setBlock(0, 79, 0, bs::GoldBlock);
    CHECK_STR(getPropStr(c.at(0), "instrument"), "bell");
    c.s->setBlock(0, 79, 0, bs::Clay);
    CHECK_STR(getPropStr(c.at(0), "instrument"), "flute");
    bool handled = false;
    c.s->interactBlock(c.s->players[0], 0, 80, 0, c.at(0), handled);
    CHECK(handled);
    CHECK_EQ(getProp(c.at(0), "note"), 1);
    CHECK_EQ(c.s->redstone.pendingBlockEvents(), 1);
    c.tick();
    CHECK_EQ(c.s->redstone.blockEventsExecuted, 1);
}

TEST(redstone_piston_plan_twelve_block_limit_and_directional_boundaries) {
    Circuit c;
    PistonPlan p;
    for (int f = 0; f < 6; ++f) {
        PistonPos base{0, 110, 0};
        for (int n = 1; n <= 12; ++n) {
            auto q = base.offset(f, n);
            c.s->world.setBlock(0, q.x, q.y, q.z, bs::Stone, false);
        }
        CHECK(p.resolve(*c.s, base, f, true));
        CHECK_EQ(p.moveCount, 12);
        for (int n = 0; n < 12; ++n)
            CHECK(p.moved[n] == base.offset(f, n + 1));
        auto q = base.offset(f, 13);
        c.s->world.setBlock(0, q.x, q.y, q.z, bs::Stone, false);
        CHECK(!p.resolve(*c.s, base, f, true));
        for (int n = 1; n <= 13; ++n) {
            q = base.offset(f, n);
            c.s->world.setBlock(0, q.x, q.y, q.z, bs::Air, false);
        }
    }
    c.s->world.setBlock(0, 0, 255, 0, bs::Stone, false);
    CHECK(!PistonPlan::pushable(*c.s, {0, 255, 0}, 1, true, 1));
    CHECK(PistonPlan::pushable(*c.s, {0, 255, 0}, 5, true, 5));
    CHECK(!PistonPlan::pushable(*c.s, {0, -1, 0}, 1, true, 1));
    CHECK(!PistonPlan::pushable(*c.s, {1024, 100, 0}, 5, true, 5));
}
TEST(redstone_piston_plan_immovable_and_destroy_reactions_match_runtime) {
    Circuit c;
    PistonPlan p;
    for (uint16_t st :
         {bs::Obsidian, bs::CryingObsidian, bs::RespawnAnchor, bs::Bedrock, bs::Chest, bs::Furnace, bs::Hopper,
          bs::Dispenser, bs::Spawner, bs::EndPortalFrame, bs::MovingPiston, setBool(bs::Piston, "extended", true)}) {
        c.s->world.setBlock(0, 1, 100, 0, st, false);
        CHECK(!p.resolve(*c.s, {0, 100, 0}, 5, true));
    }
    c.s->world.setBlock(0, 1, 100, 0, bs::Piston, false);
    CHECK(p.resolve(*c.s, {0, 100, 0}, 5, true));
    CHECK_EQ(p.moveCount, 1);
    c.s->world.setBlock(0, 1, 100, 0, bs::RedstoneWire, false);
    CHECK(p.resolve(*c.s, {0, 100, 0}, 5, true));
    CHECK_EQ(p.moveCount, 0);
    CHECK_EQ(p.breakCount, 1);
    c.s->world.setBlock(0, 2, 100, 0, bs::RedstoneWire, false);
    CHECK(!p.resolve(*c.s, {0, 100, 0}, 5, false));
}
TEST(redstone_piston_plan_slime_and_honey_branches_and_push_only) {
    Circuit c;
    PistonPlan p;
    c.s->world.setBlock(0, 1, 100, 0, bs::SlimeBlock, false);
    c.s->world.setBlock(0, 1, 101, 0, bs::Stone, false);
    c.s->world.setBlock(0, 1, 100, 1, bs::HoneyBlock, false);
    c.s->world.setBlock(0, 1, 100, -1, bs::WhiteGlazedTerracotta, false);
    c.s->world.setBlock(0, 1, 99, 0, bs::Obsidian, false);
    CHECK(p.resolve(*c.s, {0, 100, 0}, 5, true));
    CHECK_EQ(p.moveCount, 2);
    CHECK(p.moved[0] == (PistonPos{1, 100, 0}));
    CHECK(p.moved[1] == (PistonPos{1, 101, 0}));
    // Glazed terracotta is push-only: it can be pushed directly, but not pulled
    // by sticky pistons or carried sideways by slime/honey.
    c.s->world.setBlock(0, 2, 100, 0, bs::WhiteGlazedTerracotta, false);
    CHECK(p.resolve(*c.s, {0, 100, 0}, 5, true));
    CHECK_EQ(p.moveCount, 3);
    CHECK(!p.resolve(*c.s, {0, 100, 0}, 5, false));
    // An immovable block ahead of a branch prevents the entire operation.
    c.s->world.setBlock(0, 2, 101, 0, bs::Obsidian, false);
    CHECK(!p.resolve(*c.s, {0, 100, 0}, 5, true));
}
TEST(redstone_piston_plan_sticky_loop_has_unique_dependency_order) {
    Circuit c;
    PistonPlan p;
    for (int x = 1; x <= 3; ++x)
        for (int z = 0; z <= 1; ++z)
            c.s->world.setBlock(0, x, 100, z, bs::SlimeBlock, false);
    CHECK(p.resolve(*c.s, {0, 100, 0}, 5, true));
    CHECK_EQ(p.moveCount, 6);
    for (int i = 0; i < p.moveCount; ++i)
        for (int j = i + 1; j < p.moveCount; ++j)
            CHECK(!(p.moved[i] == p.moved[j]));
    CHECK_EQ(p.breakCount, 0);
    c.s->world.setBlock(0, 4, 100, 1, bs::Torch, false);
    CHECK(p.resolve(*c.s, {0, 100, 0}, 5, true));
    CHECK_EQ(p.breakCount, 1);
    CHECK(p.broken[0] == (PistonPos{4, 100, 1}));
}

TEST(redstone_piston_extends_defers_motion_and_retracts) {
    Circuit c;
    c.put(0, 0, setPropStr(bs::Piston, "facing", "east"));
    c.put(1, 0, bs::Stone);
    c.put(-1, 0, bs::RedstoneBlock);
    CHECK(!getBool(c.at(0), "extended"));
    CHECK_EQ(c.s->redstone.pendingBlockEvents(), 1);
    c.tick();
    CHECK(getBool(c.at(0), "extended"));
    CHECK_EQ(blockIdOf(c.at(1)), blk::MovingPiston);
    CHECK_EQ(blockIdOf(c.at(2)), blk::MovingPiston);
    c.tick(2);
    CHECK_EQ(blockIdOf(c.at(1)), blk::PistonHead);
    CHECK_EQ(c.at(2), bs::Stone);
    c.put(-1, 0, bs::Air);
    c.tick();
    CHECK_EQ(blockIdOf(c.at(0)), blk::MovingPiston);
    CHECK_EQ(c.at(1), bs::Air);
    c.tick(2);
    CHECK_EQ(blockIdOf(c.at(0)), blk::Piston);
    CHECK(!getBool(c.at(0), "extended"));
    CHECK_EQ(c.at(2), bs::Stone);
    CHECK_EQ(c.s->redstone.failures, 0u);
}
TEST(redstone_sticky_piston_pulls_after_completed_extension_and_drops_short_pulse) {
    for (bool shortPulse : {false, true}) {
        Circuit c;
        c.put(0, 0, setPropStr(bs::StickyPiston, "facing", "east"));
        c.put(1, 0, bs::Stone);
        c.put(-1, 0, bs::RedstoneBlock);
        c.tick(shortPulse ? 1 : 4);
        c.put(-1, 0, bs::Air);
        c.tick(3);
        CHECK_EQ(blockIdOf(c.at(0)), blk::StickyPiston);
        CHECK(!getBool(c.at(0), "extended"));
        CHECK_EQ(c.at(1), shortPulse ? bs::Air : bs::Stone);
        CHECK_EQ(c.at(2), shortPulse ? bs::Stone : bs::Air);
    }
}
TEST(redstone_piston_quasi_connectivity_requires_neighbour_notification) {
    Circuit c;
    c.put(0, 0, setPropStr(bs::Piston, "facing", "east"));
    c.s->setBlock(0, 82, 0, bs::RedstoneBlock);
    c.tick(4);
    CHECK(!getBool(c.at(0), "extended"));
    CHECK_EQ(c.s->redstone.pendingBlockEvents(), 0);
    c.put(0, 1, bs::Stone);
    c.tick(3);
    CHECK(getBool(c.at(0), "extended"));
    c.s->setBlock(0, 82, 0, bs::Air);
    c.tick(3);
    CHECK(getBool(c.at(0), "extended"));
    c.put(0, 1, bs::Air);
    c.tick(3);
    CHECK(!getBool(c.at(0), "extended"));
}
TEST(redstone_piston_event_rechecks_power_and_block_identity) {
    Circuit c;
    c.put(0, 0, setPropStr(bs::Piston, "facing", "east"));
    c.put(-1, 0, bs::RedstoneBlock);
    c.put(-1, 0, bs::Air);
    c.tick(3);
    CHECK(!getBool(c.at(0), "extended"));
    CHECK_EQ(c.s->redstone.blockEventsExecuted, 0u);
    c.put(-1, 0, bs::RedstoneBlock);
    c.put(0, 0, bs::Stone);
    c.tick(3);
    CHECK_EQ(c.at(0), bs::Stone);
    CHECK_EQ(c.s->redstone.blockEventsExecuted, 0u);
}

TEST(redstone_piston_moving_collision_and_entity_displacement) {
    Circuit c;
    // Directly execute the block-event phase so entity gravity/AI cannot hide a
    // piston collision error in this test.
    c.put(0, 0, setPropStr(bs::Piston, "facing", "east"));
    c.put(1, 0, bs::Stone);
    Entity* e = c.s->dropItem(2.1, 80.1, .5, ItemStack::of(itm::Stone, 1), false);
    CHECK(e != nullptr);
    if (!e) return;
    c.put(-1, 0, bs::RedstoneBlock);
    c.s->redstone.runBlockEvents(*c.s);
    PistonBox boxes[16];
    int n = Pistons::collision(*c.s, {2, 80, 0}, boxes);
    CHECK_EQ(n, 1);
    CHECK(boxes[0].lo[0] == 1);
    CHECK(boxes[0].hi[0] == 2);
    c.s->tickBlockEntities();
    CHECK(e->x > 2.6 && e->x < 2.62); // half-block plus the vanilla separation epsilon
    n = Pistons::collision(*c.s, {2, 80, 0}, boxes);
    CHECK_EQ(n, 1);
    CHECK(boxes[0].lo[0] == 1.5);
    CHECK_EQ(Pistons::collision(*c.s, {2, 80, 0}, boxes, 5), 0);
}
TEST(redstone_piston_slime_velocity_and_honey_carry) {
    for (bool honey : {false, true}) {
        Circuit c;
        c.s->world.setBlock(0, 1, 79, 0, bs::Air, false); // keep the sticky block off the test platform
        c.put(0, 0, setPropStr(bs::Piston, "facing", "east"));
        c.put(1, 0, honey ? bs::HoneyBlock : bs::SlimeBlock);
        Entity* e = c.s->dropItem(honey ? 1.5 : 2.1, honey ? 80.9375 : 80.1, .5, ItemStack::of(itm::Stone, 1), false);
        CHECK(e != nullptr);
        if (!e) return;
        e->onGround = true;
        c.put(-1, 0, bs::RedstoneBlock);
        c.s->redstone.runBlockEvents(*c.s);
        c.s->tickBlockEntities();
        if (honey) {
            CHECK_EQ((int)(e->x * 1000), 2000);
            CHECK(e->vx == 0);
        } else {
            CHECK(e->vx == 1);
            CHECK(e->velDirty);
        }
    }
}
TEST(redstone_piston_chunk_boundary_and_dimension_isolation) {
    Circuit c;
    c.put(15, 0, setPropStr(bs::StickyPiston, "facing", "east"));
    c.put(16, 0, bs::Stone);
    c.put(14, 0, bs::RedstoneBlock);
    c.tick(3);
    CHECK_EQ(c.at(17), bs::Stone);
    CHECK_EQ(blockIdOf(c.at(16)), blk::PistonHead);
    c.put(14, 0, bs::Air);
    c.tick(3);
    CHECK_EQ(c.at(16), bs::Stone);
    CHECK_EQ(c.at(17), bs::Air);
    CHECK_EQ(c.s->curDim, DIM_OVERWORLD);
    CHECK_EQ(c.s->world.get(0, 0, 0)->movingPistons(), 0);
    CHECK_EQ(c.s->world.get(0, 1, 0)->movingPistons(), 0);
}

TEST(redstone_pressure_plates_filter_entities_and_release_on_scheduled_check) {
    for (uint16_t plate : {bs::StonePressurePlate, bs::PolishedBlackstonePressurePlate, bs::OakPressurePlate}) {
        Circuit c;
        c.put(0, 0, plate);
        Entity* item = c.s->dropItem(.5, 80, .5, ItemStack::of(itm::Stone, 64), false);
        c.s->redstone.entityInside(*c.s, *item);
        bool stone = plate != bs::OakPressurePlate;
        CHECK_EQ(getBool(c.at(0), "powered"), !stone);
        Entity* mob = c.s->spawnMob(ent::Pig, .5, 80, .5);
        c.s->redstone.entityInside(*c.s, *mob);
        CHECK(getBool(c.at(0), "powered"));
        CHECK_EQ(c.s->redstone.directSignal(*c.s, 0, 80, 0, 1), 15);
        c.s->removeEntity(*mob);
        c.s->removeEntity(*item);
        c.tick(19);
        CHECK(getBool(c.at(0), "powered"));
        c.tick();
        CHECK(!getBool(c.at(0), "powered"));
    }
}
TEST(redstone_weighted_plates_count_entities_not_stack_size_and_sample_every_ten_ticks) {
    for (uint16_t plate : {bs::LightWeightedPressurePlate, bs::HeavyWeightedPressurePlate}) {
        Circuit c;
        c.put(0, 0, plate);
        Entity* item = c.s->dropItem(.5, 80, .5, ItemStack::of(itm::Stone, 64), false);
        c.s->redstone.entityInside(*c.s, *item);
        CHECK_EQ(getProp(c.at(0), "power"), 1);
        for (int i = 0; i < 10; ++i) {
            Entity* e = c.s->spawnEntity(EK_ARROW, ent::Arrow, .5, 80.1, .5);
            CHECK(e != nullptr);
            if (e) {
                e->onGround = true;
                c.s->redstone.entityInside(*c.s, *e);
            }
        }
        CHECK_EQ(getProp(c.at(0), "power"), 1);
        c.tick(9);
        CHECK_EQ(getProp(c.at(0), "power"), 1);
        c.tick();
        CHECK_EQ(getProp(c.at(0), "power"), plate == bs::LightWeightedPressurePlate ? 11 : 2);
        for (Entity& e : c.s->entities)
            if (e.kind != EK_NONE) c.s->removeEntity(e);
        c.tick(10);
        CHECK_EQ(getProp(c.at(0), "power"), 0);
    }
}
TEST(redstone_wooden_buttons_are_held_by_arrows_stone_buttons_ignore_them) {
    for (uint16_t button : {bs::OakButton, bs::CrimsonButton, bs::StoneButton, bs::PolishedBlackstoneButton}) {
        Circuit c;
        c.put(0, 0, setPropStr(button, "face", "floor"));
        Entity* e = c.s->spawnEntity(EK_ARROW, ent::Arrow, .5, 80, .5);
        e->onGround = true;
        c.s->redstone.entityInside(*c.s, *e);
        bool wood = button == bs::OakButton || button == bs::CrimsonButton;
        CHECK_EQ(getBool(c.at(0), "powered"), wood);
        c.tick(30);
        CHECK_EQ(getBool(c.at(0), "powered"), wood);
        c.s->removeEntity(*e);
        c.tick(29);
        CHECK_EQ(getBool(c.at(0), "powered"), wood);
        c.tick();
        CHECK(!getBool(c.at(0), "powered"));
    }
}
TEST(redstone_target_strength_and_pulse_do_not_retrigger_until_expired) {
    Circuit c;
    c.put(0, 0, bs::Target);
    c.s->redstone.targetHit(*c.s, 0, 80, 0, 4, 0, 80.5, .5, true);
    CHECK_EQ(getProp(c.at(0), "power"), 15);
    CHECK_EQ(c.s->redstone.directSignal(*c.s, 0, 80, 0, 4), 0);
    c.tick(10);
    c.s->redstone.targetHit(*c.s, 0, 80, 0, 4, 0, 80, .5, true);
    CHECK_EQ(getProp(c.at(0), "power"), 15);
    c.tick(9);
    CHECK_EQ(getProp(c.at(0), "power"), 15);
    c.tick();
    CHECK_EQ(getProp(c.at(0), "power"), 0);
    c.s->redstone.targetHit(*c.s, 0, 80, 0, 1, .5, 81, .75, false);
    CHECK_EQ(getProp(c.at(0), "power"), 8);
    c.tick(7);
    CHECK_EQ(getProp(c.at(0), "power"), 8);
    c.tick();
    CHECK_EQ(getProp(c.at(0), "power"), 0);
}
TEST(redstone_target_receives_actual_arrow_impact) {
    Circuit c;
    c.put(0, 0, bs::Target);
    Entity* e = c.s->spawnEntity(EK_ARROW, ent::Arrow, -1, 80.5, .5);
    e->vx = 2;
    c.tick();
    CHECK_EQ(getProp(c.at(0), "power"), 15);
    CHECK(e->onGround);
    c.tick(20);
    CHECK_EQ(getProp(c.at(0), "power"), 0);
}
TEST(redstone_trapped_chest_counts_viewers_and_excludes_spectators) {
    Circuit c;
    c.put(0, 0, bs::TrappedChest);
    c.put(1, 0, bs::RedstoneLamp);
    Player& a = c.s->players[0];
    Player& b = c.s->players[1];
    a.conn.attach(new CircuitConn);
    b.conn.attach(new CircuitConn);
    a.state = b.state = CS_PLAY;
    a.e.dim = b.e.dim = DIM_OVERWORLD;
    c.s->openContainer(a, 0, 80, 0);
    CHECK_EQ(c.s->redstone.signal(*c.s, 0, 80, 0, 5), 1);
    CHECK(getBool(c.at(1), "lit"));
    c.s->openContainer(b, 0, 80, 0);
    CHECK_EQ(c.s->redstone.signal(*c.s, 0, 80, 0, 5), 2);
    CHECK_EQ(c.s->redstone.directSignal(*c.s, 0, 80, 0, 1), 2);
    CHECK_EQ(c.s->redstone.directSignal(*c.s, 0, 80, 0, 0), 0);
    c.s->closeWindow(a, false);
    CHECK_EQ(c.s->redstone.signal(*c.s, 0, 80, 0, 5), 1);
    c.s->closeWindow(b, false);
    CHECK_EQ(c.s->redstone.signal(*c.s, 0, 80, 0, 5), 0);
    a.gamemode = GM_SPECTATOR;
    c.s->openContainer(a, 0, 80, 0);
    CHECK_EQ(c.s->redstone.signal(*c.s, 0, 80, 0, 5), 0);
    c.s->closeWindow(a, false);
    a.state = b.state = CS_FREE;
}

TEST(redstone_tripwire_attaches_triggers_both_hooks_and_releases_after_ten_ticks) {
    Circuit c;
    c.put(-1, 0, bs::Stone);
    c.put(5, 0, bs::Stone);
    c.put(0, 0, setPropStr(bs::TripwireHook, "facing", "east"));
    c.put(4, 0, setPropStr(bs::TripwireHook, "facing", "west"));
    for (int x = 1; x < 4; ++x)
        c.put(x, 0, bs::Tripwire);
    CHECK(getBool(c.at(0), "attached"));
    CHECK(getBool(c.at(4), "attached"));
    for (int x = 1; x < 4; ++x) {
        CHECK(getBool(c.at(x), "attached"));
        CHECK(getBool(c.at(x), "east"));
        CHECK(getBool(c.at(x), "west"));
    }
    Entity* item = c.s->dropItem(2.5, 80.1, .5, ItemStack::of(itm::Stone), false);
    c.s->redstone.entityInside(*c.s, *item);
    CHECK(getBool(c.at(0), "powered"));
    CHECK(getBool(c.at(4), "powered"));
    CHECK_EQ(c.s->redstone.directSignal(*c.s, 0, 80, 0, 5), 15);
    CHECK_EQ(c.s->redstone.signal(*c.s, -1, 80, 0, 4), 15);
    c.s->removeEntity(*item);
    c.tick(9);
    CHECK(getBool(c.at(0), "powered"));
    c.tick();
    CHECK(!getBool(c.at(0), "powered"));
    CHECK(!getBool(c.at(4), "powered"));
}
TEST(redstone_tripwire_break_pulses_but_shears_disarm) {
    for (bool shears : {false, true}) {
        Circuit c;
        c.put(-1, 0, bs::Stone);
        c.put(5, 0, bs::Stone);
        c.put(0, 0, setPropStr(bs::TripwireHook, "facing", "east"));
        c.put(4, 0, setPropStr(bs::TripwireHook, "facing", "west"));
        for (int x = 1; x < 4; ++x)
            c.put(x, 0, bs::Tripwire);
        c.tick(10);
        Player& p = c.s->players[0];
        p.inv[SLOT_HOTBAR_START] = ItemStack::of(shears ? itm::Shears : itm::Stone);
        c.s->breakBlock(2, 80, 0, &p, true);
        CHECK_EQ(getBool(c.at(0), "powered"), !shears);
        CHECK_EQ(getBool(c.at(4), "powered"), !shears);
        CHECK_EQ(c.at(2), bs::Air);
        c.tick(10);
        CHECK(!getBool(c.at(0), "powered"));
        CHECK(!getBool(c.at(4), "powered"));
        CHECK(!getBool(c.at(0), "attached"));
        CHECK(!getBool(c.at(4), "attached"));
    }
}
TEST(redstone_tripwire_maximum_span_and_hook_support_removal) {
    for (int span : {41, 42}) {
        Circuit c;
        c.put(-1, 0, bs::Stone);
        c.put(span + 1, 0, bs::Stone);
        c.put(0, 0, setPropStr(bs::TripwireHook, "facing", "east"));
        c.put(span, 0, setPropStr(bs::TripwireHook, "facing", "west"));
        for (int x = 1; x < span; ++x)
            c.put(x, 0, bs::Tripwire);
        CHECK_EQ(getBool(c.at(0), "attached"), span == 41);
        CHECK_EQ(getBool(c.at(span), "attached"), span == 41);
        c.put(-1, 0, bs::Air);
        CHECK_EQ(c.at(0), bs::Air);
        CHECK(!getBool(c.at(span), "attached"));
    }
}

TEST(redstone_tnt_primes_with_eighty_tick_fuse_instead_of_exploding_immediately) {
    Circuit c;
    c.put(0, 0, bs::Tnt);
    c.put(-1, 0, bs::RedstoneBlock);
    CHECK_EQ(c.at(0), bs::Air);
    Entity* tnt = nullptr;
    for (Entity& e : c.s->entities)
        if (e.kind == EK_TNT) tnt = &e;
    CHECK(tnt != nullptr);
    if (!tnt) return;
    CHECK_EQ(tnt->type, ent::Tnt);
    CHECK_EQ(tnt->fuse, 80);
    c.put(2, 0, bs::Stone);
    c.tick(79);
    CHECK_EQ(tnt->fuse, 1);
    CHECK_EQ(c.at(2), bs::Stone);
    c.tick();
    CHECK(tnt->removed || tnt->kind == EK_NONE);
}
TEST(redstone_tnt_chain_fuse_and_unstable_breaking) {
    Circuit c;
    c.put(0, 0, bs::Tnt);
    Entity* e = c.s->primeTnt(0, 80, 0, 42, true);
    CHECK(e != nullptr);
    if (!e) return;
    CHECK(e->fuse >= 10 && e->fuse < 30);
    CHECK_EQ(e->owner, 42);
    c.put(3, 0, setBool(bs::Tnt, "unstable", true));
    Player& p = c.s->players[0];
    p.gamemode = GM_SURVIVAL;
    p.e.id = 43;
    c.s->breakBlock(3, 80, 0, &p, true);
    CHECK_EQ(c.at(3), bs::Air);
    int count = 0;
    for (Entity& t : c.s->entities)
        if (t.kind == EK_TNT) ++count;
    CHECK_EQ(count, 2);
    c.put(6, 0, setBool(bs::Tnt, "unstable", true));
    p.gamemode = GM_CREATIVE;
    c.s->breakBlock(6, 80, 0, &p, false);
    count = 0;
    for (Entity& t : c.s->entities)
        if (t.kind == EK_TNT) ++count;
    CHECK_EQ(count, 2);
}

TEST(redstone_hopper_pushes_and_pulls_one_item_with_eight_tick_cooldown_and_lock) {
    Circuit c;
    c.put(0, 0, setPropStr(bs::Hopper, "facing", "east"));
    c.put(1, 0, bs::Chest);
    c.s->setBlock(0, 81, 0, bs::Chest);
    Chunk* chunk = c.s->world.get(0, 0, 0);
    TileEntity* hopper = chunk->tileAt(0, 80, 0);
    CHECK(hopper && hopper->type == TILE_HOPPER);
    if (!hopper) return;
    TileEntity* above = chunk->addTile(TILE_CHEST, 0, 81, 0);
    hopper->items[0] = ItemStack::of(itm::IronIngot, 3);
    above->items[0] = ItemStack::of(itm::Coal, 3);
    c.tick();
    TileEntity* target = chunk->tileAt(1, 80, 0);
    CHECK(target != nullptr);
    if (!target) return;
    CHECK_EQ(target->items[0].count, 1);
    CHECK_EQ(hopper->items[0].count, 2);
    CHECK_EQ(hopper->items[1].id, itm::Coal);
    CHECK_EQ(above->items[0].count, 2);
    CHECK_EQ(hopper->transferCooldown, 8);
    c.tick(7);
    CHECK_EQ(target->items[0].count, 1);
    c.tick();
    CHECK_EQ(target->items[0].count, 2);
    c.put(-1, 0, bs::RedstoneBlock);
    CHECK(!getBool(c.at(0), "enabled"));
    c.tick(12);
    CHECK_EQ(target->items[0].count, 2);
    CHECK_EQ(hopper->transferCooldown, 0);
    c.put(-1, 0, bs::Air);
    c.tick();
    CHECK_EQ(target->items[0].count, 3);
    CHECK(above->items[0].empty());
}
TEST(redstone_hopper_receiving_cooldown_is_independent_of_tick_insertion_order) {
    for (bool reverse : {false, true}) {
        Circuit c;
        for (int i = 0; i < 2; ++i)
            c.put(reverse ? 1 - i : i, 0, setPropStr(bs::Hopper, "facing", "east"));
        c.put(2, 0, bs::Chest);
        Chunk* chunk = c.s->world.get(0, 0, 0);
        TileEntity* from = chunk->tileAt(0, 80, 0);
        TileEntity* to = chunk->tileAt(1, 80, 0);
        from->items[0] = ItemStack::of(itm::Stone, 1);
        c.tick();
        CHECK(from->items[0].empty());
        CHECK_EQ(to->items[0].count, 1);
        CHECK_EQ(to->transferCooldown, 7);
        TileEntity* out = chunk->tileAt(2, 80, 0);
        CHECK(out != nullptr);
        if (!out) return;
        CHECK(out->items[0].empty());
        c.tick(6);
        CHECK(out->items[0].empty());
        c.tick();
        CHECK_EQ(out->items[0].count, 1);
    }
}
TEST(redstone_hopper_furnace_sided_slots_and_output_before_bucket) {
    Circuit c;
    c.put(0, 0, bs::Hopper);
    c.s->setBlock(0, 81, 0, bs::Furnace);
    ItemStack stone = ItemStack::of(itm::Stone), coal = ItemStack::of(itm::Coal), iron = ItemStack::of(itm::IronIngot);
    CHECK(Automation::insertOne(*c.s, {0, 81, 0}, stone, 1));
    CHECK(Automation::insertOne(*c.s, {0, 81, 0}, coal, 4));
    CHECK(!Automation::insertOne(*c.s, {0, 81, 0}, iron, 4));
    CHECK_EQ(iron.count, 1);
    Chunk* chunk = c.s->world.get(0, 0, 0);
    TileEntity* f = chunk->tileAt(0, 81, 0);
    TileEntity* h = chunk->tileAt(0, 80, 0);
    c.tick();
    CHECK(h->items[0].empty()); // neither input nor unused fuel may be sucked out below
    f->items[2] = ItemStack::of(itm::IronIngot);
    f->items[1] = ItemStack::of(itm::Bucket);
    c.tick();
    CHECK_EQ(h->items[0].id, itm::IronIngot);
    CHECK_EQ(f->items[1].id, itm::Bucket);
    c.tick(8);
    CHECK_EQ(h->items[1].id, itm::Bucket);
    CHECK(f->items[1].empty());
}
TEST(redstone_hopper_collects_whole_item_stack_and_not_items_outside_suction_shape) {
    Circuit c;
    c.put(0, 0, bs::Hopper);
    Entity* item = c.s->dropItem(.5, 81.1, .5, ItemStack::of(itm::Stone, 32), false);
    Entity* outside = c.s->dropItem(1.5, 81.1, .5, ItemStack::of(itm::Coal, 4), false);
    c.tick();
    TileEntity* h = c.s->world.get(0, 0, 0)->tileAt(0, 80, 0);
    CHECK_EQ(h->items[0].count, 32);
    CHECK(item->removed || item->kind == EK_NONE);
    CHECK_EQ(outside->item.count, 4);
    CHECK_EQ(h->transferCooldown, 8);
}
TEST(redstone_dropper_four_tick_pulse_transfers_one_item_and_full_target_does_not_eject) {
    Circuit c;
    c.put(0, 0, setPropStr(bs::Dropper, "facing", "east"));
    c.put(1, 0, bs::Chest);
    TileEntity* d = c.s->world.get(0, 0, 0)->tileAt(0, 80, 0);
    d->items[0] = ItemStack::of(itm::Stone, 3);
    c.put(-1, 0, bs::RedstoneBlock);
    c.tick();
    c.put(-1, 0, bs::Air);
    c.tick(2);
    CHECK_EQ(d->items[0].count, 3);
    c.tick();
    CHECK_EQ(d->items[0].count, 2);
    TileEntity* chest = c.s->world.get(0, 0, 0)->tileAt(1, 80, 0);
    CHECK(chest != nullptr);
    if (!chest) return;
    CHECK_EQ(chest->items[0].count, 1);
    for (ItemStack& item : chest->items)
        item = ItemStack::of(itm::Stone, 64);
    c.put(-1, 0, bs::RedstoneBlock);
    c.tick(20);
    CHECK_EQ(d->items[0].count, 2);
    for (Entity& e : c.s->entities)
        CHECK(e.kind != EK_ITEM);
}

TEST(redstone_dispenser_tnt_and_arrows_use_entities_while_dropper_ejects_items) {
    for (bool dropper : {false, true})
        for (uint16_t item : {itm::Tnt, itm::Arrow}) {
            Circuit c;
            c.put(0, 0, setPropStr(dropper ? bs::Dropper : bs::Dispenser, "facing", "east"));
            TileEntity* t = c.s->world.get(0, 0, 0)->tileAt(0, 80, 0);
            t->items[0] = ItemStack::of(item, 2);
            c.put(-1, 0, bs::RedstoneBlock);
            c.tick(3);
            CHECK_EQ(t->items[0].count, 2);
            c.tick();
            CHECK_EQ(t->items[0].count, 1);
            int found = 0;
            for (Entity& e : c.s->entities)
                if (e.kind != EK_NONE && !e.removed) {
                    CHECK_EQ(e.kind, dropper ? EK_ITEM : item == itm::Tnt ? EK_TNT : EK_ARROW);
                    ++found;
                    if (e.kind == EK_TNT) CHECK_EQ(e.fuse, 79);
                    if (e.kind == EK_ARROW) CHECK(e.vx > .8);
                }
            CHECK_EQ(found, 1);
            c.tick(4);
            CHECK_EQ(t->items[0].count, 1); // steady power is not a clock
        }
}
TEST(redstone_dispenser_buckets_replace_the_stack_and_capture_sources) {
    Circuit c;
    c.put(0, 0, setPropStr(bs::Dispenser, "facing", "east"));
    TileEntity* t = c.s->world.get(0, 0, 0)->tileAt(0, 80, 0);
    t->items[0] = ItemStack::of(itm::WaterBucket);
    Automation::dispense(*c.s, {0, 80, 0});
    CHECK_EQ(c.at(1), bs::Water);
    CHECK_EQ(t->items[0].id, itm::Bucket);
    Automation::dispense(*c.s, {0, 80, 0});
    CHECK_EQ(c.at(1), bs::Air);
    CHECK_EQ(t->items[0].id, itm::WaterBucket);
    c.put(1, 0, setBool(bs::OakSlab, "waterlogged", false));
    Automation::dispense(*c.s, {0, 80, 0});
    CHECK(getBool(c.at(1), "waterlogged"));
    CHECK_EQ(t->items[0].id, itm::Bucket);
    Automation::dispense(*c.s, {0, 80, 0});
    CHECK(!getBool(c.at(1), "waterlogged"));
    CHECK_EQ(t->items[0].id, itm::WaterBucket);
}
TEST(redstone_dispenser_flint_and_steel_durability_and_failed_use) {
    Circuit c;
    c.put(0, 0, setPropStr(bs::Dispenser, "facing", "east"));
    TileEntity* t = c.s->world.get(0, 0, 0)->tileAt(0, 80, 0);
    t->items[0] = ItemStack::of(itm::FlintAndSteel);
    c.put(1, 0, bs::Stone);
    Automation::dispense(*c.s, {0, 80, 0});
    CHECK_EQ(t->items[0].damage, 0);
    c.put(1, 0, bs::Air);
    Automation::dispense(*c.s, {0, 80, 0});
    CHECK_EQ(c.at(1), bs::Fire);
    CHECK_EQ(t->items[0].damage, 1);
    c.put(1, 0, bs::Tnt);
    Automation::dispense(*c.s, {0, 80, 0});
    CHECK_EQ(c.at(1), bs::Air);
    CHECK_EQ(t->items[0].damage, 2);
    t->items[0].damage = ITEMS[itm::FlintAndSteel].durability - 1;
    Automation::dispense(*c.s, {0, 80, 0});
    CHECK(t->items[0].empty());
}

TEST(redstone_daylight_cadence_weather_and_inversion) {
    Circuit c;
    c.s->meta.worldAge = 0;
    c.s->meta.timeOfDay = 5980;
    c.put(0, 0, bs::DaylightDetector);
    c.tick(19);
    CHECK_EQ(getProp(c.at(0), "power"), 0);
    c.tick();
    CHECK_EQ(getProp(c.at(0), "power"), 15);
    auto sample = [&](int time, int weather) {
        c.s->meta.timeOfDay = time;
        c.s->meta.raining = weather;
        c.s->redstone.daylightDetector(*c.s, 0, 80, 0, c.at(0));
        return getProp(c.at(0), "power");
    };
    CHECK_EQ(sample(6000, 1), 12);
    CHECK_EQ(sample(6000, 2), 10);
    CHECK_EQ(sample(18000, 0), 0);
    Player& p = c.s->players[0];
    p.gamemode = GM_SURVIVAL;
    bool handled = false;
    c.s->interactBlock(p, 0, 80, 0, c.at(0), handled);
    CHECK(handled);
    CHECK(getBool(c.at(0), "inverted"));
    CHECK_EQ(getProp(c.at(0), "power"), 11);
    CHECK_EQ(sample(6000, 0), 0);
    CHECK_EQ(sample(6000, 2), 5);
    CHECK_EQ(c.s->redstone.daylightComputations, 0u);
    // Dimensions without skylight keep even a deliberately prepowered state.
    Server::InDim nether(*c.s, DIM_NETHER);
    uint16_t state = setProp(setBool(bs::DaylightDetector, "inverted", true), "power", 7);
    c.s->world.setBlock(DIM_NETHER, 0, 80, 0, state);
    c.s->redstone.daylightDetector(*c.s, 0, 80, 0, state);
    CHECK_EQ(getProp(c.s->blockAt(0, 80, 0), "power"), 7);
    CHECK_EQ(c.s->redstone.failures, 0u);
}

TEST(redstone_daylight_reuses_sky_until_occlusion_changes) {
    Circuit c;
    c.s->meta.timeOfDay = 6000;
    c.put(14, 0, bs::DaylightDetector);
    c.put(15, 0, bs::DaylightDetector);
    auto sample = [&](int x) {
        c.s->redstone.daylightDetector(*c.s, x, 80, 0, c.at(x));
        return getProp(c.at(x), "power");
    };
    c.s->setBlock(15, 82, 0, bs::Glass);
    CHECK_EQ(sample(15), 15);
    CHECK_EQ(c.s->redstone.daylightComputations, 0u);
    for (int z = -3; z <= 3; ++z)
        for (int x = 12; x <= 18; ++x)
            c.s->setBlock(x, 82, z, bs::Stone);
    int shade = sample(15);
    CHECK(shade > 0 && shade < 15);
    CHECK_EQ(c.s->redstone.daylightComputations, 1u);
    CHECK(sample(14) >= shade);
    CHECK_EQ(c.s->redstone.daylightComputations, 1u);
    sample(15);
    CHECK_EQ(c.s->redstone.daylightComputations, 1u);
    // A change in the neighbouring chunk invalidates the regional cache.
    c.s->setBlock(16, 82, 0, bs::Air);
    CHECK(sample(15) > shade);
    CHECK_EQ(c.s->redstone.daylightComputations, 2u);
    c.s->setBlock(15, 82, 0, bs::Air);
    CHECK_EQ(sample(15), 15);
    CHECK_EQ(c.s->redstone.daylightComputations, 2u);
    CHECK_EQ(c.s->redstone.failures, 0u);
    for (int i = 0; i < c.s->world.tableSize(); ++i)
        if (Chunk* chunk = c.s->world.slot(i)) CHECK_EQ(chunk->jobRefs, 0);
}

namespace {
ItemStack circuitBook(int pages) {
    ByteBuf bytes;
    Writer w(bytes);
    NbtWriter n(w);
    n.beginRoot();
    n.listHeader("pages", NBT_STRING, pages);
    for (int i = 0; i < pages; ++i) {
        w.u16(4);
        w.bytes((const uint8_t*)"page", 4);
    }
    n.end();
    ItemStack book = ItemStack::of(itm::WritableBook);
    CHECK(book.setTag(bytes.data(), bytes.size()));
    return book;
}
void bookButton(Player& p, int button, int window = -1) {
    uint8_t bytes[] = {(uint8_t)(window < 0 ? p.winId : window), (uint8_t)button};
    Reader r(bytes, sizeof(bytes));
    p.onPacket(pkt::c2s::EnchantItem, r);
}
} // namespace
TEST(redstone_lectern_pages_comparator_and_two_tick_pulse) {
    Circuit c;
    c.put(0, 0, bs::Lectern);
    Player& p = c.s->players[0];
    p.gamemode = GM_SURVIVAL;
    ItemStack book = circuitBook(3);
    CHECK(Books::place(*c.s, p, 0, 80, 0, book));
    CHECK(book.empty());
    CHECK(getBool(c.at(0), "has_book"));
    CHECK_EQ(c.s->redstone.analog(*c.s, 0, 80, 0), 1);
    CHECK_EQ(c.s->redstone.signal(*c.s, 0, 80, 0, 5), 0);
    Books::turnPage(*c.s, 0, 80, 0, 1);
    CHECK_EQ(c.s->redstone.analog(*c.s, 0, 80, 0), 8);
    for (int f = 0; f < 6; ++f) {
        CHECK_EQ(c.s->redstone.signal(*c.s, 0, 80, 0, f), 15);
        CHECK_EQ(c.s->redstone.directSignal(*c.s, 0, 80, 0, f), f == 1 ? 15 : 0);
    }
    c.tick();
    Books::turnPage(*c.s, 0, 80, 0, 2);
    CHECK_EQ(c.s->redstone.analog(*c.s, 0, 80, 0), 15);
    CHECK(getBool(c.at(0), "powered"));
    c.tick(); // second page change does not refresh the pending block tick
    CHECK(!getBool(c.at(0), "powered"));
    Books::turnPage(*c.s, 0, 80, 0, 200); // clamps to current page, no pulse
    CHECK(!getBool(c.at(0), "powered"));
    CHECK_EQ(c.s->redstone.failures, 0u);
}
TEST(redstone_lectern_menu_buttons_and_removal_preserve_book_data) {
    Circuit c;
    c.put(0, 0, bs::Lectern);
    Player& p = c.s->players[0];
    p.conn.attach(new CircuitConn);
    p.state = CS_PLAY;
    p.e.dim = DIM_OVERWORLD;
    p.e.x = .5;
    p.e.y = 80;
    p.e.z = 2;
    p.gamemode = GM_CREATIVE;
    ItemStack book = circuitBook(15), original = book;
    CHECK(Books::place(*c.s, p, 0, 80, 0, book));
    CHECK_EQ(book.count, 1);
    c.s->openLectern(p, 0, 80, 0);
    CHECK_EQ(p.winKind, WK_LECTERN);
    bookButton(p, 109);
    CHECK_EQ(c.s->redstone.analog(*c.s, 0, 80, 0), 10);
    bookButton(p, 100, p.winId + 1);
    CHECK_EQ(c.s->redstone.analog(*c.s, 0, 80, 0), 10);
    p.gamemode = GM_ADVENTURE;
    bookButton(p, 3);
    CHECK(getBool(c.at(0), "has_book"));
    bookButton(p, 1);
    CHECK_EQ(c.s->redstone.analog(*c.s, 0, 80, 0), 9);
    p.gamemode = GM_SURVIVAL;
    bookButton(p, 3);
    CHECK(!getBool(c.at(0), "has_book"));
    CHECK(!getBool(c.at(0), "powered"));
    CHECK_EQ(c.s->redstone.analog(*c.s, 0, 80, 0), 0);
    int found = 0;
    for (const ItemStack& slot : p.inv)
        if (slot.sameItem(original)) found += slot.count;
    CHECK_EQ(found, 1);
    p.state = CS_FREE;
}
TEST(redstone_lectern_one_page_outputs_fifteen_and_replacement_drops_the_book) {
    Circuit c;
    c.put(0, 0, bs::Lectern);
    Player& p = c.s->players[0];
    p.gamemode = GM_SURVIVAL;
    ItemStack book = circuitBook(1), original = book;
    CHECK(Books::place(*c.s, p, 0, 80, 0, book));
    CHECK_EQ(c.s->redstone.analog(*c.s, 0, 80, 0), 15);
    Books::turnPage(*c.s, 0, 80, 0, 0);
    CHECK(!getBool(c.at(0), "powered"));
    c.put(0, 0, bs::Stone);
    int drops = 0;
    for (const Entity& e : c.s->entities)
        if (!e.removed && e.kind == EK_ITEM && e.item.sameItem(original)) drops += e.item.count;
    CHECK_EQ(drops, 1);
}
TEST(redstone_book_edit_signing_only_changes_pages_and_server_owned_author) {
    Circuit c;
    Player& p = c.s->players[0];
    p.gamemode = GM_SURVIVAL;
    p.state = CS_PLAY;
    snprintf(p.name, sizeof(p.name), "Author");
    p.inv[SLOT_HOTBAR_START] = circuitBook(1);
    auto edit = [&](bool sign, int inventorySlot) {   // 1.21.8: slot, pages, optional title
        ByteBuf packet;
        Writer w(packet);
        w.varint(inventorySlot);
        w.varint(3);
        for (int i = 0; i < 3; i++) w.string("page");
        w.boolean(sign);
        if (sign) w.string("Circuits");
        Reader r(packet.data(), packet.size());
        p.onPacket(pkt::c2s::EditBook, r);
    };
    edit(false, 0);
    CHECK_EQ(Books::pages(p.inv[SLOT_HOTBAR_START]), 3);
    CHECK_EQ(p.inv[SLOT_HOTBAR_START].id, itm::WritableBook);
    edit(true, 0);
    CHECK_EQ(p.inv[SLOT_HOTBAR_START].id, itm::WrittenBook);
    CHECK_EQ(Books::pages(p.inv[SLOT_HOTBAR_START]), 3);
    struct Found {
        bool author = false, page = false;
    } found;
    ItemStack& signedBook = p.inv[SLOT_HOTBAR_START];
    Reader tags(signedBook.tagData(), signedBook.tagSize());
    CHECK(nbtVisitRoot(
        tags,
        [](void* context, uint8_t type, const char* name, Reader& r) {
            Found& f = *(Found*)context;
            if (type == NBT_STRING && !strcmp(name, "author")) {
                uint16_t size = r.u16();
                const uint8_t* text = r.take(size);
                f.author = size == 6 && text && !memcmp(text, "Author", 6);
                return true;
            }
            if (type == NBT_LIST && !strcmp(name, "pages")) {
                r.u8();
                r.i32();
                uint16_t size = r.u16();
                const uint8_t* text = r.take(size);
                f.page = size == 15 && text && !memcmp(text, "{\"text\":\"page\"}", 15);
            }
            return false;
        },
        &found));
    CHECK(found.author);
    CHECK(found.page);
    ItemStack before = signedBook;
    edit(false, 0);
    CHECK(signedBook.sameItem(before));
    p.inv[SLOT_OFFHAND] = circuitBook(1);
    edit(false, 40);
    CHECK_EQ(Books::pages(p.inv[SLOT_OFFHAND]), 3);
}


// ---------------------------------------------------------------- 1.17 - 1.21 components
TEST(redstone_copper_bulb_toggles_on_each_rising_edge) {
    Circuit c;
    c.put(0, 0, bs::CopperBulb);
    CHECK(!getBool(c.at(0), "lit"));
    c.put(-1, 0, bs::RedstoneBlock);
    c.tick();
    CHECK(getBool(c.at(0), "lit"));
    CHECK(getBool(c.at(0), "powered"));
    CHECK_EQ(c.s->redstone.analog(*c.s, 0, 80, 0), 15);
    c.put(-1, 0, bs::Air);   // power off: it stays lit
    c.tick();
    CHECK(getBool(c.at(0), "lit"));
    CHECK(!getBool(c.at(0), "powered"));
    c.put(-1, 0, bs::RedstoneBlock);   // the next pulse turns it off
    c.tick();
    CHECK(!getBool(c.at(0), "lit"));
    CHECK_EQ(c.s->redstone.analog(*c.s, 0, 80, 0), 0);
    // its light follows the oxidation stage
    CHECK_EQ(stateEmission(setBool(bs::CopperBulb, "lit", true)), 15);
    CHECK_EQ(stateEmission(setBool(bs::OxidizedCopperBulb, "lit", true)), 4);
    CHECK_EQ(stateEmission(bs::CopperBulb), 0);
}

TEST(redstone_note_block_takes_the_instrument_of_a_head_on_top) {
    Circuit c;
    c.put(0, 0, bs::NoteBlock);
    c.s->setBlock(0, 81, 0, bs::ZombieHead);
    c.put(-1, 0, bs::RedstoneBlock);
    c.tick();
    CHECK_STR(getPropStr(c.at(0), "instrument"), "zombie");
    CHECK(getBool(c.at(0), "powered"));
}

TEST(redstone_lightning_rod_powers_like_a_lever) {
    Circuit c;
    c.put(1, 0, bs::RedstoneLamp);
    c.put(0, 0, setBool(setPropStr(bs::LightningRod, "facing", "up"), "powered", true));
    c.tick();
    CHECK(getBool(c.at(1), "lit"));                                    // weak power beside it
    CHECK_EQ(c.s->redstone.directSignal(*c.s, 0, 80, 0, 1), 15);       // strong into its support below
    CHECK_EQ(c.s->redstone.directSignal(*c.s, 0, 80, 0, 2), 0);
}

TEST(redstone_crafter_crafts_four_ticks_after_a_rising_edge) {
    Circuit c;
    c.put(0, 0, setPropStr(bs::Crafter, "orientation", "east_up"));
    TileEntity* t = Automation::container(*c.s, {0, 80, 0});
    CHECK(t && t->type == TILE_CRAFTER);
    // four planks in the top left: a crafting table
    for (int i : {0, 1, 3, 4}) t->items[i] = ItemStack::of(itm::OakPlanks, 2);
    t->disabledSlots = 1u << 8;
    CHECK_EQ(c.s->redstone.analog(*c.s, 0, 80, 0), 5);   // four filled, one disabled
    c.put(0, 1, bs::RedstoneBlock);
    c.tick(3);
    CHECK_EQ(t->items[0].count, 2);
    c.tick(2);
    CHECK_EQ(t->items[0].count, 1);
    int tables = 0;
    for (const Entity& e : c.s->entities)
        if (!e.removed && e.kind == EK_ITEM && e.item.id == itm::CraftingTable) {
            tables += e.item.count;
            CHECK(e.x > 1.0);   // out of its east face
        }
    CHECK_EQ(tables, 1);
    CHECK(getBool(c.at(0), "triggered"));
    // still powered: no second craft
    c.tick(10);
    CHECK_EQ(t->items[0].count, 1);
    // automation fills the grid evenly and never a disabled slot
    TileEntity g;
    g.type = TILE_CRAFTER;
    g.disabledSlots = 1u << 1;
    g.items[0] = ItemStack::of(itm::Stone, 2);
    g.items[2] = ItemStack::of(itm::Stone, 1);
    ItemStack stone = ItemStack::of(itm::Stone);
    CHECK(!Automation::crafterAccepts(g, 0, stone));   // a later slot has fewer
    CHECK(!Automation::crafterAccepts(g, 1, stone));   // disabled
    CHECK(!Automation::crafterAccepts(g, 2, stone));   // later empty slots come first
    CHECK(Automation::crafterAccepts(g, 3, stone));
}

TEST(redstone_chiseled_bookshelf_slots_and_comparator) {
    Circuit c;
    c.put(0, 0, setPropStr(bs::ChiseledBookshelf, "facing", "south"));
    Player& p = c.s->players[0];
    p.state = CS_PLAY;
    p.gamemode = GM_SURVIVAL;
    p.inv[SLOT_HOTBAR_START] = ItemStack::of(itm::Book, 3);
    p.held = 0;
    // the bottom right slot seen from the south: x near 1, y low
    p.clickFace = 3;
    p.clickX = 0.9f; p.clickY = 0.2f; p.clickZ = 1.0f;
    CHECK(c.s->useBookshelf(p, 0, 80, 0, c.at(0)));
    CHECK(getBool(c.at(0), "slot_5_occupied"));
    CHECK_EQ(p.inv[SLOT_HOTBAR_START].count, 2);
    CHECK_EQ(c.s->redstone.analog(*c.s, 0, 80, 0), 6);
    // the top left slot
    p.clickX = 0.1f; p.clickY = 0.8f;
    CHECK(c.s->useBookshelf(p, 0, 80, 0, c.at(0)));
    CHECK(getBool(c.at(0), "slot_0_occupied"));
    CHECK_EQ(c.s->redstone.analog(*c.s, 0, 80, 0), 1);
    // taking it back
    CHECK(c.s->useBookshelf(p, 0, 80, 0, c.at(0)));
    CHECK(!getBool(c.at(0), "slot_0_occupied"));
    CHECK_EQ(p.inv[SLOT_HOTBAR_START].count, 2);
    // the side does nothing; a hopper puts books in the first free slot
    p.clickFace = 4;
    CHECK(!c.s->useBookshelf(p, 0, 80, 0, c.at(0)));
    ItemStack book = ItemStack::of(itm::EnchantedBook);
    CHECK(Automation::insertOne(*c.s, {0, 80, 0}, book, 1));
    CHECK(getBool(c.at(0), "slot_0_occupied"));
    ItemStack stone = ItemStack::of(itm::Stone);
    CHECK(!Automation::insertOne(*c.s, {0, 80, 0}, stone, 1));   // books only
    p.state = CS_FREE;
}


TEST(redstone_sculk_sensor_hears_vibrations_by_distance) {
    Circuit c;
    c.put(0, 0, bs::SculkSensor);
    c.put(0, 1, bs::RedstoneLamp);
    c.tick();
    // a block placed 4 blocks away: it arrives after 4 ticks, power 15 - floor(15 / 8 * 4)
    c.s->vibration(4.5, 80.5, 0.5, GE_BLOCK_PLACE);
    c.tick(3);
    CHECK_STR(getPropStr(c.at(0), "sculk_sensor_phase"), "inactive");
    c.tick(2);
    CHECK_STR(getPropStr(c.at(0), "sculk_sensor_phase"), "active");
    CHECK_EQ(getProp(c.at(0), "power"), 8);
    CHECK(getBool(c.at(0, 1), "lit"));
    CHECK_EQ(c.s->redstone.analog(*c.s, 0, 80, 0), GE_BLOCK_PLACE);
    // deaf while active and cooling down
    c.s->vibration(1.5, 80.5, 0.5, GE_EXPLODE);
    c.tick(30);
    CHECK_STR(getPropStr(c.at(0), "sculk_sensor_phase"), "cooldown");
    CHECK_EQ(getProp(c.at(0), "power"), 0);
    c.tick(10);
    CHECK_STR(getPropStr(c.at(0), "sculk_sensor_phase"), "inactive");
    CHECK_EQ(c.s->redstone.analog(*c.s, 0, 80, 0), GE_BLOCK_PLACE);
    // out of range, and behind wool
    c.s->vibration(9.5, 80.5, 0.5, GE_EXPLODE);
    c.put(2, 0, bs::WhiteWool);
    c.s->vibration(3.5, 80.5, 0.5, GE_EXPLODE);
    c.tick(12);
    CHECK_STR(getPropStr(c.at(0), "sculk_sensor_phase"), "inactive");
    // a block broken next to it is heard (a game event from the server)
    c.put(-2, 0, bs::Stone);
    c.tick();
    c.s->breakBlock(-2, 80, 0, nullptr, false);
    c.tick(3);
    CHECK_STR(getPropStr(c.at(0), "sculk_sensor_phase"), "active");
    CHECK_EQ(c.s->redstone.analog(*c.s, 0, 80, 0), GE_BLOCK_DESTROY);
}

TEST(redstone_calibrated_sculk_sensor_filters_by_its_input) {
    Circuit c;
    c.put(0, 0, setPropStr(bs::CalibratedSculkSensor, "facing", "west"));
    c.put(-1, 0, bs::RedstoneBlock);   // input 15 on the west side
    c.tick();
    c.s->vibration(12.5, 80.5, 0.5, GE_BLOCK_PLACE);   // 12 blocks: in its range of 16, wrong frequency
    c.tick(14);
    CHECK_STR(getPropStr(c.at(0), "sculk_sensor_phase"), "inactive");
    c.s->vibration(12.5, 80.5, 0.5, GE_EXPLODE);
    c.tick(13);
    CHECK_STR(getPropStr(c.at(0), "sculk_sensor_phase"), "active");
    CHECK_EQ(getProp(c.at(0), "power"), 15 - 11);   // 15 - floor(15 / 16 * 12)
    c.tick(10);   // active for 10 ticks
    CHECK_STR(getPropStr(c.at(0), "sculk_sensor_phase"), "cooldown");
}

#include "data/redstone_traces.inc"
