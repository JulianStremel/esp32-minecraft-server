#include <chrono>
#include <vector>
#include "testing.h"
#include "mc/registry.h"
#include "mc/world/generator.h"
#include "mc/world/section.h"
#include "mc/world/world.h"

using namespace mc;

TEST(section_random_edits_match_reference) {
    Section s;
    std::vector<uint16_t> ref(4096, 0);
    Rng rng(42);
    // phase 1: few distinct states (stays indirect), phase 2: many (forces global palette)
    for (int phase = 0; phase < 2; phase++) {
        int distinct = phase == 0 ? 12 : 600;
        for (int i = 0; i < 20000; i++) {
            int idx = rng.range(4096);
            uint16_t st = (uint16_t)(1 + rng.range(distinct));
            s.set(idx, st);
            ref[idx] = st;
        }
        bool same = true;
        int nonAir = 0;
        for (int i = 0; i < 4096; i++) {
            if (s.get(i) != ref[i]) same = false;
            if (ref[i]) nonAir++;
        }
        CHECK(same);
        CHECK_EQ(s.nonAirCount(), nonAir);
        if (phase == 0) CHECK(s.bits() == 4);
        if (phase == 1) CHECK_EQ(s.bits(), GLOBAL_PALETTE_BITS);
    }
    // back to few states then optimize -> shrinks
    for (int i = 0; i < 4096; i++) { s.set(i, (uint16_t)(i % 3 + 1)); ref[i] = (uint16_t)(i % 3 + 1); }
    s.optimize();
    CHECK_EQ(s.bits(), 4);
    bool same = true;
    for (int i = 0; i < 4096; i++) if (s.get(i) != ref[i]) same = false;
    CHECK(same);
    for (int i = 0; i < 4096; i++) s.set(i, 7);
    s.optimize();
    CHECK(s.isUniform());
    CHECK_EQ(s.get(1234), 7);
}

TEST(section_wire_size_matches_written_bytes) {
    Section s;
    Rng rng(7);
    for (int round = 0; round < 4; round++) {
        int distinct = round == 0 ? 1 : round == 1 ? 10 : round == 2 ? 100 : 400;
        for (int i = 0; i < 4096; i++) s.set(i, (uint16_t)(rng.range(distinct) + 1));
        CountSink cs;
        Writer w(cs);
        s.writeWire(w);
        CHECK_EQ(cs.count, s.wireSize());
        CountSink cs2;
        Writer w2(cs2);
        s.writeStore(w2);
        CHECK_EQ(cs2.count, s.storeSize());
    }
    Section u;
    u.fill(bs::Stone);
    CountSink cs;
    Writer w(cs);
    u.writeWire(w);
    CHECK_EQ(cs.count, u.wireSize());
}

TEST(section_store_roundtrip) {
    Rng rng(9);
    for (int distinct : {1, 5, 40, 300}) {
        Section s;
        for (int i = 0; i < 4096; i++) s.set(i, (uint16_t)(rng.range(distinct) + (distinct == 1 ? 5 : 0)));
        std::vector<uint8_t> buf(s.storeSize());
        BufSink bs_(buf.data(), buf.size());
        Writer w(bs_);
        s.writeStore(w);
        Section t;
        Reader r(buf.data(), buf.size());
        CHECK(t.readStore(r));
        bool same = true;
        for (int i = 0; i < 4096; i++) if (s.get(i) != t.get(i)) same = false;
        CHECK(same);
        CHECK_EQ(t.nonAirCount(), s.nonAirCount());
    }
    // corrupt data is rejected
    uint8_t bad[3] = {4, 0, 0};
    Reader r(bad, 3);
    Section t;
    CHECK(!t.readStore(r));
}

TEST(chunk_heightmap_tracks_edits) {
    Chunk c(0, 0);
    CHECK_EQ(c.height(3, 4), dimMinY(DIM_OVERWORLD));   // empty: the bottom of the world
    c.set(3, 10, 4, bs::Stone);
    CHECK_EQ(c.height(3, 4), 11);
    c.set(3, 40, 4, bs::Stone);
    CHECK_EQ(c.height(3, 4), 41);
    c.set(3, 40, 4, bs::Air);
    CHECK_EQ(c.height(3, 4), 11);
    c.set(3, 20, 4, bs::ShortGrass);   // grass is not motion blocking
    CHECK_EQ(c.height(3, 4), 11);
    c.set(3, 12, 4, bs::Water);   // fluids are
    CHECK_EQ(c.height(3, 4), 13);
}

static uint64_t chunkHashOf(Chunk& c) {
    uint64_t h = 1469598103934665603ull;
    for (int y = 0; y < 256; y++)
        for (int z = 0; z < 16; z++)
            for (int x = 0; x < 16; x++) { h ^= c.get(x, y, z); h *= 1099511628211ull; }
    return h;
}

TEST(generator_is_deterministic_and_plausible) {
    Generator g;
    g.init(12345, WORLD_NORMAL);
    Chunk a(3, -7), b(3, -7);
    g.generate(a);
    g.generate(b);
    CHECK(chunkHashOf(a) == chunkHashOf(b));
    Generator g2;
    g2.init(999, WORLD_NORMAL);
    Chunk d(3, -7);
    g2.generate(d);
    CHECK(chunkHashOf(a) != chunkHashOf(d));
    // statistics over a 6x6 area
    int bedrock = 0, air = 0, stone = 0, ores = 0, logs = 0, water = 0, carved = 0;
    auto t0 = std::chrono::steady_clock::now();
    int n = 0;
    for (int cx = -3; cx < 3; cx++)
        for (int cz = -3; cz < 3; cz++, n++) {
            Chunk c(cx, cz);
            g.generate(c);
            for (int z = 0; z < 16; z++)
                for (int x = 0; x < 16; x++) {
                    if (c.get(x, c.minY(), z) == bs::Bedrock) bedrock++;
                    for (int y = 1; y < 256; y++) {
                        uint16_t s = c.get(x, y, z);
                        if (s == bs::Stone) stone++;
                        else if (s == bs::CaveAir) carved++;
                        else if (stateIsAir(s)) air++;
                        else if (s == bs::Water) water++;
                        else if (s == bs::CoalOre || s == bs::IronOre || s == bs::DiamondOre) ores++;
                        else if (blockIdOf(s) == blk::OakLog || blockIdOf(s) == blk::SpruceLog || blockIdOf(s) == blk::BirchLog) logs++;
                    }
                }
        }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    printf("    %d chunks in %.1f ms (%.2f ms/chunk); stone=%d carved=%d ores=%d logs=%d water=%d\n", n, ms, ms / n,
           stone, carved, ores, logs, water);
    CHECK_EQ(bedrock, n * 256);
    CHECK(stone > n * 256 * 30);
    CHECK(ores > n * 20);
    CHECK(carved > 0);
    CHECK(carved < stone / 5);
}

TEST(generator_spawn_is_dry_land) {
    Generator g;
    g.init(12345, WORLD_NORMAL);
    int x, y, z;
    g.findSpawn(x, y, z);
    ColumnInfo ci = g.column(x, z);
    CHECK(ci.height >= SEA_LEVEL);
    CHECK_EQ(y, ci.height + 1);
}

TEST(trees_cross_chunk_borders_consistently) {
    // Every log found in a chunk must have been produced by the same site as in its neighbour:
    // regenerate a 3x3 area twice in different order and compare.
    Generator g;
    g.init(777, WORLD_NORMAL);
    uint64_t h1 = 0, h2 = 0;
    for (int i = 0; i < 9; i++) { Chunk c(i % 3, i / 3); g.generate(c); h1 = h1 * 31 + chunkHashOf(c); }
    for (int i = 0; i < 9; i++) { Chunk c(i % 3, i / 3); g.generate(c); h2 = h2 * 31 + chunkHashOf(c); }
    CHECK(h1 == h2);
}

struct PinAll : ChunkPinner {
    int pinRadius = 0;
    bool isChunkPinned(uint8_t, int cx, int cz) override { return abs(cx) <= pinRadius && abs(cz) <= pinRadius; }
};

TEST(world_cache_evicts_lru_and_respects_pins) {
    Generator g;
    g.init(1, WORLD_FLAT);
    World w;
    Generator* const gens[NUM_DIMS] = {&g, &g, &g};
    w.init(gens, nullptr, 16, 100);
    PinAll pin;
    pin.pinRadius = 1;  // 9 chunks pinned
    w.setPinner(&pin);
    for (int i = 0; i < 200; i++) {
        w.load(DIM_OVERWORLD, i % 20 - 10, i / 20 - 5);
        w.maintain();
    }
    CHECK(w.residentCount() <= 16 + 1);
    for (int cx = -1; cx <= 1; cx++)
        for (int cz = -1; cz <= 1; cz++) {
            w.load(DIM_OVERWORLD, cx, cz);
        }
    for (int i = 0; i < 100; i++) { w.load(DIM_OVERWORLD, 50 + i, 50); w.maintain(); }
    for (int cx = -1; cx <= 1; cx++)
        for (int cz = -1; cz <= 1; cz++) CHECK(w.isResident(DIM_OVERWORLD, cx, cz));
    // hash table consistency: every resident chunk can be found
    int found = 0;
    for (int i = 0; i < w.tableSize(); i++) {
        Chunk* c = w.slot(i);
        if (c) { CHECK(w.get(DIM_OVERWORLD, c->cx, c->cz) == c); found++; }
    }
    CHECK_EQ(found, w.residentCount());
    // block edits
    w.setBlock(DIM_OVERWORLD, 5, 10, 5, bs::Stone);
    CHECK_EQ(w.getBlock(DIM_OVERWORLD, 5, 10, 5), bs::Stone);
    CHECK_EQ(w.getBlock(DIM_OVERWORLD, 5, 3, 5), bs::GrassBlock);
}
