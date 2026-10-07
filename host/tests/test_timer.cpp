// Timer wheel (scheduled ticks) and tick pacer (overrun counting) tests.
#include <stdlib.h>
#include <map>
#include <vector>
#include "testing.h"
#include "mc/tick_pacer.h"
#include "mc/timer_wheel.h"

using namespace mc;

namespace {

TimerKey blockKey(int i) { return TimerKey::block(i, 64, -i, 7); }

// Advances `ticks` ticks and returns (tick, x) of every event that ran, in order.
std::vector<std::pair<uint32_t, int>> runFor(TimerWheel& w, uint32_t ticks, int max = 64) {
    std::vector<std::pair<uint32_t, int>> got;
    std::vector<TimerEvent> out((size_t)max);
    for (uint32_t t = 0; t < ticks; t++) {
        uint32_t tick = w.now();
        int n = w.advance(out.data(), max);
        for (int i = 0; i < n; i++) got.push_back({tick, out[i].key.x});
    }
    return got;
}

uint32_t s_fakeNow = 0;
uint32_t fakeClock() { return s_fakeNow; }

}  // namespace

TEST(timer_wheel_orders_by_tick_priority_then_insertion) {
    TimerWheel w;
    CHECK(w.init(64));
    w.reset(1000);
    // same tick: priority first (lower runs first), then the order they were scheduled
    CHECK(w.schedule(blockKey(1), 1005, 0));
    CHECK(w.schedule(blockKey(2), 1005, -3));
    CHECK(w.schedule(blockKey(3), 1005, 0));
    CHECK(w.schedule(blockKey(4), 1005, -3));
    CHECK(w.schedule(blockKey(5), 1003, 2));   // earlier tick wins over any priority
    CHECK(w.schedule(blockKey(6), 999, 0));    // in the past: runs at the next tick
    CHECK_EQ(w.size(), 6);
    auto got = runFor(w, 10);
    CHECK_EQ((int)got.size(), 6);
    int expect[] = {6, 5, 2, 4, 1, 3};
    uint32_t ticks[] = {1000, 1003, 1005, 1005, 1005, 1005};
    for (int i = 0; i < 6 && i < (int)got.size(); i++) {
        CHECK_EQ(got[i].second, expect[i]);
        CHECK_EQ(got[i].first, ticks[i]);
    }
    CHECK_EQ(w.size(), 0);
    CHECK_EQ(w.now(), 1010u);
}

TEST(timer_wheel_long_horizons_cascade_to_the_exact_tick) {
    // events across every level, from start ticks that are not aligned to any bucket
    TimerWheel w;
    CHECK(w.init(4096));
    srand(7);
    uint32_t starts[] = {0, 77, 0xFFFFFF00u, 123456789u, (1u << 20) - 3};
    for (uint32_t start : starts) {
        w.reset(start);
        std::map<int, uint32_t> due;
        const uint32_t HORIZON = 3u << 20;   // 3.1M ticks: levels 0..3
        for (int i = 0; i < 2000; i++) {
            uint32_t d;
            switch (i % 5) {
                case 0: d = (uint32_t)rand() % 256; break;
                case 1: d = 250 + (uint32_t)rand() % 20; break;            // around the level 0/1 edge
                case 2: d = (uint32_t)rand() % (1u << 14); break;
                case 3: d = (1u << 14) - 8 + (uint32_t)rand() % 16; break;  // level 1/2 edge
                default: d = (uint32_t)rand() % HORIZON; break;
            }
            CHECK(w.schedule(blockKey(i), start + d));
            due[i] = start + d;
        }
        auto got = runFor(w, HORIZON + 1, 4096);
        CHECK_EQ((int)got.size(), 2000);
        int wrong = 0;
        uint32_t prev = start;
        for (auto& g : got) {
            if (g.first != due[g.second]) wrong++;
            if ((int32_t)(g.first - prev) < 0) wrong++;
            prev = g.first;
        }
        CHECK_EQ(wrong, 0);
        CHECK_EQ(w.size(), 0);
    }
}

TEST(timer_wheel_overflow_beyond_the_last_level) {
    TimerWheel w;
    CHECK(w.init(8));
    uint32_t start = (1u << 26) - 100;   // just below a level-3 wrap
    w.reset(start);
    uint32_t far = start + (1u << 26) + 1234;   // beyond level 3: the overflow list
    CHECK(w.schedule(blockKey(1), far));
    CHECK(w.schedule(blockKey(2), start + 50));
    TimerEvent out[8];
    uint32_t firstTick = 0, farTick = 0;
    for (uint64_t t = 0; t <= (1ull << 26) + 2000; t++) {
        uint32_t tick = w.now();
        int n = w.advance(out, 8);
        for (int i = 0; i < n; i++) {
            if (out[i].key.x == 1) farTick = tick;
            else firstTick = tick;
        }
        if (farTick) break;
    }
    CHECK_EQ(firstTick, start + 50);
    CHECK_EQ(farTick, far);
}

TEST(timer_wheel_cancel_and_duplicates) {
    TimerWheel w;
    CHECK(w.init(16));
    w.reset(0);
    CHECK(w.schedule(blockKey(1), 10));
    CHECK(w.schedule(blockKey(2), 10));
    CHECK(w.schedule(blockKey(3), 20000));   // level 2
    // the same key again is ignored, even with another time (vanilla keeps the first)
    CHECK(!w.schedule(blockKey(2), 5));
    CHECK(w.pending(blockKey(2)));
    CHECK_EQ(w.find(blockKey(2))->due, 10u);
    // a different block at the same position is a different key
    CHECK(w.schedule(TimerKey::block(2, 64, -2, 8), 10));
    CHECK(w.cancel(blockKey(2)));
    CHECK(!w.cancel(blockKey(2)));
    CHECK(!w.pending(blockKey(2)));
    CHECK(w.cancel(blockKey(3)));
    CHECK_EQ(w.size(), 2);
    auto got = runFor(w, 30000);
    CHECK_EQ((int)got.size(), 2);
    if (got.size() == 2) {
        CHECK_EQ(got[0].second, 1);
        CHECK_EQ(got[1].second, 2);   // the other block's tick
    }
    // once it ran, the key can be scheduled again
    CHECK(w.schedule(blockKey(1), w.now() + 1));
    // heavy churn keeps the hash consistent (backward-shift deletion)
    srand(3);
    for (int round = 0; round < 20000; round++) {
        int k = rand() % 12;
        if (rand() % 2) w.schedule(blockKey(100 + k), w.now() + 1 + rand() % 500);
        else w.cancel(blockKey(100 + k));
    }
    int pending = 0;
    for (int k = 0; k < 12; k++) pending += w.pending(blockKey(100 + k));
    CHECK_EQ(w.size(), pending + 1);
}

TEST(timer_wheel_full_and_per_tick_limit) {
    TimerWheel w;
    CHECK(w.init(10));
    w.reset(0);
    for (int i = 0; i < 10; i++) CHECK(w.schedule(blockKey(i), 5, (int8_t)(i % 2)));
    CHECK(!w.schedule(blockKey(99), 5));
    CHECK_EQ(w.dropped(), 1u);
    CHECK(w.schedule(blockKey(50), 6) == false);   // still full
    // at most 4 per tick: the rest runs at the next ticks, before that tick's own events
    TimerEvent out[4];
    for (int t = 0; t < 5; t++) CHECK_EQ(w.advance(out, 4), 0);
    CHECK_EQ(w.advance(out, 4), 4);   // tick 5: priority 0 first: 0 2 4 6
    CHECK_EQ(out[0].key.x, 0);
    CHECK_EQ(out[3].key.x, 6);
    CHECK(w.schedule(blockKey(20), 6, -5));   // room again; due at tick 6
    CHECK_EQ(w.advance(out, 4), 4);   // tick 6: left-overs 8 1 3 5, then 7 9 20 wait
    CHECK_EQ(out[0].key.x, 8);
    CHECK_EQ(out[1].key.x, 1);
    CHECK_EQ(out[3].key.x, 5);
    int n = w.advance(out, 4);        // tick 7: 7 9 (left over), then 20
    CHECK_EQ(n, 3);
    CHECK_EQ(out[0].key.x, 7);
    CHECK_EQ(out[1].key.x, 9);
    CHECK_EQ(out[2].key.x, 20);
    CHECK_EQ(w.size(), 0);
}

TEST(tick_pacer_counts_overruns_with_a_fake_clock) {
    s_fakeNow = 1000;
    ClockTickSource src(50, fakeClock);
    TickPacer p;
    uint32_t ran = 0;
    auto pass = [&]() {
        uint32_t n = p.add(src.take());
        for (uint32_t i = 0; i < n; i++) {
            p.started();
            ran++;
        }
        return n;
    };
    // on time: one tick per period, no overruns
    CHECK_EQ(pass(), 1u);                 // the first tick is due at once
    CHECK_EQ(src.msUntilNext(), 50u);
    s_fakeNow += 20;
    CHECK_EQ(pass(), 0u);
    CHECK_EQ(src.msUntilNext(), 30u);
    for (int i = 0; i < 10; i++) {
        s_fakeNow += 50;
        pass();
    }
    TickPacer::Counts c = p.takeWindow();
    CHECK_EQ(c.overruns, 0u);
    CHECK_EQ(c.late, 0u);
    CHECK_EQ(ran, 11u);
    // a 180 ms stall: 3 periods pass at once (plus the one in progress) -> one overrun;
    // the late ticks are caught up right away
    s_fakeNow += 180;
    CHECK_EQ(pass(), 4u);
    c = p.takeWindow();
    CHECK_EQ(c.overruns, 1u);
    CHECK_EQ(c.late, 3u);
    CHECK_EQ(c.skipped, 0u);
    // 8 ticks due: 5 now, 3 at the next pass (late, but counted once)
    s_fakeNow += 50 * 8;
    CHECK_EQ(pass(), 5u);
    CHECK_EQ(p.backlog(), 3u);
    CHECK_EQ(pass(), 3u);
    c = p.takeWindow();
    CHECK_EQ(c.overruns, 1u);
    CHECK_EQ(c.late, 7u);
    // a 3 s stall: the backlog is dropped, one tick runs
    ran = 0;
    s_fakeNow += 3000;
    CHECK_EQ(pass(), 1u);
    CHECK_EQ(p.skippedNow(), 59u);
    c = p.takeWindow();
    CHECK_EQ(c.overruns, 1u);
    CHECK_EQ(c.skipped, 59u);
    CHECK_EQ(c.late, 0u);
    CHECK_EQ(ran, 1u);
    // and the pace is back to normal afterwards
    s_fakeNow += 50;
    CHECK_EQ(pass(), 1u);
    c = p.takeWindow();
    CHECK_EQ(c.overruns, 0u);
    // clock wraparound
    s_fakeNow = 0xFFFFFFF0u;
    ClockTickSource w(50, fakeClock);
    CHECK_EQ(w.take(), 1u);
    s_fakeNow += 60;   // wraps
    CHECK_EQ(w.take(), 1u);
    CHECK_EQ(w.msUntilNext(), 40u);
}

// ---------------------------------------------------------------- with a server
#include "mc/registry.h"
#include "mc/server/server.h"
#include "mc/storage/block_device.h"
#include "mc/storage/world_store.h"

namespace {

// Hands out exactly the ticks a test asks for, so a few hundred ticks take no time.
struct ManualTicks : TickSource {
    uint32_t pending = 0;
    uint32_t take() override {
        uint32_t n = pending;
        pending = 0;
        return n;
    }
    uint32_t msUntilNext() override { return 0; }
};

void runTicksFast(Server& s, ManualTicks& src, int n) {
    while (n > 0) {
        int k = n < (int)TickPacer::MAX_CATCH_UP ? n : (int)TickPacer::MAX_CATCH_UP;
        src.pending = (uint32_t)k;
        uint32_t before = s.ticks;
        s.loop();
        n -= (int)(s.ticks - before);
    }
}

Server* flatServer(Storage* st, ManualTicks& src) {
    Server* s = new Server();
    ServerConfig cfg;
    cfg.port = 0;
    cfg.worldType = WORLD_FLAT;
    cfg.spawnMobs = false;
    cfg.seed = 1;
    cfg.workerThreads = 0;
    if (!s->begin(cfg, st)) {
        delete s;
        return nullptr;
    }
    s->setTickSource(&src);
    return s;
}

}  // namespace

TEST(furnace_smelts_lazily_through_the_timer_wheel) {
    ManualTicks src;
    Server* s = flatServer(nullptr, src);
    CHECK(s != nullptr);
    if (!s) return;
    runTicksFast(*s, src, 5);   // world age 0 means "not yet updated" to a furnace
    Chunk* c = s->world.load(0, 0);
    int x = 3, y = 4, z = 3;
    s->world.setBlock(x, y, z, BLOCKS[blk::Furnace].defState);
    TileEntity* t = c->addTile(TILE_FURNACE, x, y, z);
    t->items[0] = ItemStack::of(itm::IronOre);
    t->items[0].count = 3;
    t->items[1] = ItemStack::of(itm::Coal);   // 1600 ticks: 8 items
    s->containerChanged(x, y, z);             // like a click in the window
    TimerKey key = TimerKey::furnace(x, y, z);
    CHECK(s->timers.pending(key));
    // 3 items: 600 ticks. The furnace only wakes up per item, not per tick.
    int events = 0;
    uint32_t lastDue = s->timers.pending(key) ? s->timers.find(key)->due : 0;
    for (int i = 0; i < 650; i++) {
        runTicksFast(*s, src, 1);
        const TimerEvent* ev = s->timers.find(key);
        uint32_t due = ev ? ev->due : 0;
        if (due != lastDue) { events++; if (getenv("FURNACE_DEBUG")) printf("    tick %u: next furnace event %u (out %d)\n", s->worldTick(), due, t->items[2].count); }
        lastDue = due;
    }
    CHECK_EQ(t->items[2].id, itm::IronIngot);
    CHECK_EQ(t->items[2].count, 3);
    CHECK(t->items[0].empty());
    CHECK(t->items[1].empty());               // the coal went in
    CHECK(getBool(s->blockAt(x, y, z), "lit"));   // still burning: 1000 ticks of fuel left
    CHECK(events <= 4);                      // 3 items done, then "fuel used up"
    // it goes out when the fuel is used up (1600 ticks after it was lit)
    runTicksFast(*s, src, 1000);
    CHECK(!getBool(s->blockAt(x, y, z), "lit"));
    CHECK(!s->timers.pending(key));
    delete s;
}

TEST(scheduled_ticks_are_saved_with_their_chunk) {
    MemDevice dev(64u << 20);
    WorldStore ws(&dev);
    StoreParams sp;
    sp.radius = 16;
    CHECK(ws.open(sp));
    ManualTicks src;
    Server* s = flatServer(&ws, src);
    CHECK(s != nullptr);
    if (!s) return;
    runTicksFast(*s, src, 3);
    // lava flows slowly (30 ticks): the tick is still pending when the chunk is stored
    int x = 5 * 16 + 4, y = 4, z = 5 * 16 + 7;
    s->world.load(5, 5);
    s->setBlock(x, y, z, BLOCKS[blk::Lava].defState);
    TimerKey key = TimerKey::block(x, y, z, blk::Lava);
    CHECK(s->timers.pending(key));
    runTicksFast(*s, src, 10);
    const TimerEvent* ev = s->timers.find(key);
    CHECK(ev != nullptr);
    if (!ev) return;
    int32_t left = (int32_t)(ev->due - s->worldTick());
    CHECK(left > 0 && left <= 30);
    int8_t prio = ev->prio;
    // saved and unloaded: the tick goes with the chunk, out of the wheel
    CHECK(s->world.saveAll() >= 1);
    CHECK(s->world.evictUnpinned(1000) >= 1);
    CHECK(s->world.peek(5, 5) == nullptr);
    CHECK(!s->timers.pending(key));
    // time passes while it is unloaded (unloaded chunks do not tick), then it comes back
    runTicksFast(*s, src, 100);
    s->world.load(5, 5);
    ev = s->timers.find(key);
    CHECK(ev != nullptr);
    if (ev) {
        CHECK_EQ((int32_t)(ev->due - s->worldTick()), left);
        CHECK_EQ(ev->prio, prio);
    }
    delete s;
}

TEST(chunk_records_without_ticks_still_load) {
    // a record written before scheduled ticks were stored (no FLAG_TICKS) decodes as before
    MemDevice dev(16u << 20);
    WorldStore ws(&dev);
    StoreParams sp;
    sp.radius = 4;
    sp.compress = false;   // so the payload can be edited
    CHECK(ws.open(sp));
    Chunk a(1, 1);
    a.set(2, 70, 3, BLOCKS[blk::Stone].defState);
    ChunkTick tk;
    tk.lx = 2;
    tk.y = 71;
    tk.lz = 3;
    tk.block = blk::Water;
    tk.delay = 5;
    tk.prio = -1;
    CHECK(a.setTicks(&tk, 1));
    ChunkRecord rec;
    CHECK(ws.encodeChunk(a, rec, nullptr));
    CHECK(rec.flags & 2);
    // round trip with ticks
    Chunk b(1, 1);
    CHECK(ws.decodeChunk(rec, b));
    CHECK_EQ(b.tickCount, 1);
    if (b.tickCount == 1) {
        CHECK_EQ(b.ticks[0].lx, 2);
        CHECK_EQ(b.ticks[0].y, 71);
        CHECK_EQ(b.ticks[0].lz, 3);
        CHECK_EQ(b.ticks[0].block, blk::Water);
        CHECK_EQ(b.ticks[0].delay, 5);
        CHECK_EQ(b.ticks[0].prio, -1);
    }
    CHECK_EQ(blockIdOf(b.get(2, 70, 3)), blk::Stone);
    // the old layout: same payload without the tick list and without the flag
    a.clearTicks();
    ChunkRecord v2;
    CHECK(ws.encodeChunk(a, v2, nullptr));
    ChunkRecord v1;
    size_t n = v2.bytes.size() - 2;   // the trailing tick count (0)
    uint8_t* p = v1.bytes.append(n);
    memcpy(p, v2.bytes.data(), n);
    v1.raw = (uint32_t)n;
    v1.crc = crc32(v1.bytes.data(), n);
    v1.flags = 0;
    Chunk c(1, 1);
    CHECK(ws.decodeChunk(v1, c));
    CHECK_EQ(c.tickCount, 0);
    CHECK_EQ(blockIdOf(c.get(2, 70, 3)), blk::Stone);
}
