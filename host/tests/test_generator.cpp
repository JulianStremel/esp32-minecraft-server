// World generator hardening: the same blocks on every platform, in any order, on any
// thread, with the same detail everywhere up to the world border.
#include <set>
#include <thread>
#include <vector>
#include "testing.h"
#include "mc/server/server.h"
#include "mc/storage/block_device.h"
#include "mc/storage/world_store.h"
#include "mc/world/generator.h"

using namespace mc;

namespace {

uint32_t chunkHash(const Chunk& c) {
    uint32_t h = 2166136261u;
    for (int y = 0; y < WORLD_HEIGHT; y++)
        for (int z = 0; z < 16; z++)
            for (int x = 0; x < 16; x++) {
                h ^= c.get(x, y, z);
                h *= 16777619u;
            }
    for (int z = 0; z < 16; z++)
        for (int x = 0; x < 16; x++) {
            h ^= (uint32_t)c.height(x, z) | (uint32_t)c.biome(x, z) << 16;
            h *= 16777619u;
        }
    return h;
}

uint32_t generateHash(const Generator& g, int cx, int cz) {
    Chunk* c = new Chunk(cx, cz);
    g.generate(*c);
    uint32_t h = chunkHash(*c);
    delete c;
    return h;
}

}  // namespace

TEST(generator_matches_golden_fingerprints) {
    // The values were computed by the PC build; the device benchmark (tools/qemu/run.sh
    // --bench) checks the same table on the ESP32.
    for (int i = 0; i < NUM_GENERATOR_GOLDEN; i++) {
        const GeneratorGolden& g = GENERATOR_GOLDEN[i];
        uint32_t fp = generatorFingerprint(g.seed, g.version);
        if (fp != g.fingerprint)
            printf("    seed %llu v%d: fingerprint %08x, expected %08x\n", (unsigned long long)g.seed, g.version,
                   (unsigned)fp, (unsigned)g.fingerprint);
        CHECK_EQ(fp, g.fingerprint);
    }
}

TEST(generator_is_independent_of_order_and_thread) {
    for (uint8_t version = 1; version <= GENERATOR_LATEST; version++) {
        Generator g;
        g.init(7, WORLD_NORMAL, version);
        const int R = 3, N = (2 * R + 1) * (2 * R + 1);
        std::vector<uint32_t> raster(N), reverse(N), threaded(N), fresh(N);
        for (int i = 0; i < N; i++) raster[i] = generateHash(g, i % (2 * R + 1) - R, i / (2 * R + 1) - R);
        for (int i = N - 1; i >= 0; i--) reverse[i] = generateHash(g, i % (2 * R + 1) - R, i / (2 * R + 1) - R);
        // two threads on one generator at once, interleaved
        std::thread t1([&] {
            for (int i = 0; i < N; i += 2) threaded[i] = generateHash(g, i % (2 * R + 1) - R, i / (2 * R + 1) - R);
        });
        std::thread t2([&] {
            for (int i = N - 1 - ((N - 1) % 2 == 0 ? 1 : 0); i >= 0; i -= 2)
                threaded[i] = generateHash(g, i % (2 * R + 1) - R, i / (2 * R + 1) - R);
        });
        t1.join();
        t2.join();
        // a chunk generated alone, by a generator that never saw its neighbours
        for (int i = 0; i < N; i++) {
            Generator g2;
            g2.init(7, WORLD_NORMAL, version);
            fresh[i] = generateHash(g2, i % (2 * R + 1) - R, i / (2 * R + 1) - R);
        }
        int differ = 0;
        for (int i = 0; i < N; i++)
            if (raster[i] != reverse[i] || raster[i] != threaded[i] || raster[i] != fresh[i]) differ++;
        CHECK_EQ(differ, 0);
        // and the seed matters
        Generator other;
        other.init(8, WORLD_NORMAL, version);
        CHECK(generateHash(other, 0, 0) != raster[N / 2]);
    }
}

TEST(lattice_positions_are_exact_far_from_spawn) {
    constexpr Freq detail = {9, 200, 0.045f};
    // exact fractions: -1 * 9/200 = -0.045 -> cell -1, offset 191/200
    LatticePos p = latticePos(-1, detail);
    CHECK_EQ(p.cell, -1);
    CHECK(p.frac == 191.0f * (1.0f / 200.0f));
    p = latticePos(200, detail, 1);   // octave 1: 200 * 9/100 = 18 exactly
    CHECK_EQ(p.cell, 18);
    CHECK(p.frac == 0.0f);
    // the cursor (32-bit division, then doubling per octave) against 64-bit arithmetic,
    // over the whole range up to vanilla's border
    uint32_t r = 12345;
    int wrong = 0;
    for (int i = 0; i < 20000; i++) {
        r = r * 1103515245u + 12345u;
        int32_t v = (int32_t)(r % 60000001u) - 30000000;
        // the cursor's range: |v * num| < 2^31 and cells < 2^31 after 3 doublings (den >= 100)
        int32_t num = 1 + (int32_t)((r >> 8) % 71), den = 100 + (int32_t)((r >> 16) % 9901);
        Freq f(num, den, 0);
        LatticeCursor c(v, f);
        for (int oct = 0; oct < 4; oct++) {
            int64_t q = (int64_t)v * num * ((int64_t)1 << oct);
            int64_t cell = q / den, rem = q % den;
            if (rem < 0) { rem += den; cell--; }
            LatticePos lp = c.pos();
            if (lp.cell != cell || lp.frac != (float)rem * f.inv || !(lp.frac >= 0.0f && lp.frac < 1.0f)) wrong++;
            c.nextOctave();
        }
    }
    CHECK_EQ(wrong, 0);
    // 64 neighbouring blocks 29.9 million blocks out: every block gets its own noise input
    // in version 2; version 1's float coordinates only have room for about half of them
    std::set<std::pair<int32_t, float>> v2;
    std::set<float> v1;
    for (int i = 0; i < 64; i++) {
        int32_t x = 29900000 + i;
        LatticePos q = latticePos(x, detail);
        v2.insert({q.cell, q.frac});
        v1.insert((float)x * detail.approx);
    }
    CHECK_EQ((int)v2.size(), 64);
    CHECK((int)v1.size() <= 33);
    // and it is the same position as close to spawn, shifted by whole cells
    LatticePos a = latticePos(37, detail), b = latticePos(37 + 200 * 150000, detail);
    CHECK_EQ(b.cell - a.cell, 9 * 150000);
    CHECK(a.frac == b.frac);
}

TEST(lattice_noise_does_not_repeat) {
    // the table-based noise of version 1 repeats every 256 cells; version 2 does not
    Noise n1;
    n1.init(5);
    LatticeNoise n2;
    n2.init(5);
    int same1 = 0, same2 = 0;
    for (int i = 0; i < 200; i++) {
        // strictly inside the cells (on a lattice line, 8-gradient noise has only a few
        // possible values, so different cells match by chance), and exact binary fractions
        // so that version 1's float coordinates are exact too
        float fx = 0.125f * (float)(1 + i % 7), fz = 0.0625f * (float)(1 + i % 13);
        int cx = i * 3 - 300, cz = 1000 - i * 5;
        if (n1.noise2((float)cx + fx, (float)cz + fz) == n1.noise2((float)(cx + 256) + fx, (float)cz + fz)) same1++;
        if (n2.noise2({cx, fx}, {cz, fz}) == n2.noise2({cx + 256, fx}, {cz, fz})) same2++;
    }
    CHECK_EQ(same1, 200);
    CHECK(same2 < 10);
    // values stay in the expected range
    float lo = 1, hi = -1;
    for (int i = 0; i < 2000; i++) {
        float v = n2.noise2({i * 7, 0.31f}, {-i * 3, 0.77f});
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    CHECK(lo > -1.5f && lo < -0.3f);
    CHECK(hi < 1.5f && hi > 0.3f);
}

TEST(generator_version_is_stored_with_the_world) {
    MemDevice dev(32u << 20);
    {
        WorldStore ws(&dev);
        StoreParams sp;
        sp.radius = 8;
        CHECK(ws.open(sp));
        Server* s = new Server();
        ServerConfig cfg;
        cfg.port = 0;
        cfg.seed = 99;
        cfg.workerThreads = 0;
        CHECK(s->begin(cfg, &ws));
        CHECK_EQ(s->gen.version(), GENERATOR_LATEST);   // a new world gets the newest
        delete s;
    }
    {
        WorldStore ws(&dev);
        StoreParams sp;
        sp.radius = 8;
        CHECK(ws.open(sp, false));
        WorldMeta m;
        CHECK(ws.loadMeta(m));
        CHECK_EQ(m.generatorVersion, GENERATOR_LATEST);
        // a world from before versions were stored reads 0 and stays on version 1
        m.generatorVersion = 0;
        CHECK(ws.saveMeta(m));
        Server* s = new Server();
        ServerConfig cfg;
        cfg.port = 0;
        cfg.workerThreads = 0;
        CHECK(s->begin(cfg, &ws));
        CHECK_EQ(s->gen.version(), 1);
        CHECK_EQ(s->meta.seed, 99u);
        delete s;
    }
}
