#include <chrono>
#include "testing.h"
#include "mc/world/generator.h"
#include "mc/registry.h"
#include "mc/world/chunk.h"
#include "mc/world/light.h"

using namespace mc;

// The tests' y counts from the world's bottom (B: y -64 in the overworld).
static const int B = dimMinY(DIM_OVERWORLD);

static int skyAt(const ChunkLight& L, int x, int y, int z) { return L.skyAt(x, B + y, z); }
static int blockAtL(const ChunkLight& L, int x, int y, int z) { return L.blockAt(x, B + y, z); }

static void floorChunk(Chunk& c) {
    for (int x = 0; x < 16; x++)
        for (int z = 0; z < 16; z++)
            for (int y = 0; y <= 3; y++) c.set(x, B + y, z, bs::Stone);
}

TEST(light_open_sky_is_full) {
    Chunk c(0, 0);
    floorChunk(c);
    ChunkLight L;
    CHECK(L.compute(c, nullptr));
    CHECK_EQ(L.sections(), 2);
    CHECK_EQ(skyAt(L, 5, 4, 5), 15);
    CHECK_EQ(skyAt(L, 5, 20, 5), 15);
    CHECK_EQ(skyAt(L, 5, 2, 5), 0);   // inside the stone
}

TEST(light_under_a_roof_fades_with_distance) {
    Chunk c(0, 0);
    floorChunk(c);
    for (int x = 4; x <= 11; x++)
        for (int z = 4; z <= 11; z++) c.set(x, B + 8, z, bs::Stone);
    ChunkLight L;
    CHECK(L.compute(c, nullptr));
    CHECK_EQ(skyAt(L, 3, 5, 8), 15);  // open column next to the roof
    CHECK_EQ(skyAt(L, 4, 5, 8), 14);  // one step under the roof
    CHECK_EQ(skyAt(L, 5, 5, 8), 13);
    CHECK_EQ(skyAt(L, 7, 5, 7), 11);  // 4 steps from the nearest open side
    CHECK_EQ(skyAt(L, 4, 7, 4), 14);  // corner
    CHECK_EQ(skyAt(L, 8, 9, 8), 15);  // on top of the roof
}

TEST(light_torch_in_a_closed_box) {
    Chunk c(0, 0);
    // stone box from 2..12 with air inside 3..11
    for (int x = 2; x <= 12; x++)
        for (int y = 2; y <= 12; y++)
            for (int z = 2; z <= 12; z++) {
                bool shell = x == 2 || x == 12 || y == 2 || y == 12 || z == 2 || z == 12;
                c.set(x, B + y, z, shell ? bs::Stone : bs::Air);
            }
    c.set(7, B + 3, 7, bs::Torch);
    ChunkLight L;
    CHECK(L.compute(c, nullptr));
    CHECK_EQ(skyAt(L, 7, 6, 7), 0);       // sealed from the sky
    CHECK_EQ(blockAtL(L, 7, 3, 7), 14);   // torch
    CHECK_EQ(blockAtL(L, 8, 3, 7), 13);
    CHECK_EQ(blockAtL(L, 7, 5, 7), 12);
    CHECK_EQ(blockAtL(L, 11, 3, 11), 6);  // manhattan distance 8
    CHECK_EQ(blockAtL(L, 7, 3, 1), 0);    // outside the box
    CHECK(!L.blockSectionEmpty(0));
}

TEST(light_leaves_and_water_dim_sky_light) {
    Chunk c(0, 0);
    floorChunk(c);
    c.set(6, B + 10, 6, bs::OakLeaves);
    c.set(9, B + 4, 9, bs::Water);
    c.set(9, B + 5, 9, bs::Water);
    ChunkLight L;
    CHECK(L.compute(c, nullptr));
    CHECK(skyAt(L, 6, 9, 6) < 15);        // shaded by leaves
    CHECK(skyAt(L, 6, 9, 6) >= 13);
    CHECK(skyAt(L, 9, 4, 9) < 15);        // under water
    printf("    under leaves %d, 2 deep in water %d (filter: leaves %d, water %d)\n", skyAt(L, 6, 9, 6), skyAt(L, 9, 4, 9),
           BLOCKS[blk::OakLeaves].filterLight, BLOCKS[blk::Water].filterLight);
}

// ---------------------------------------------------------------- light across chunk borders
// A 3x3 block of chunks around (0, 0); index (dz + 1) * 3 + (dx + 1).
struct Nine {
    Chunk* c[9];
    Nine() {
        for (int k = 0; k < 9; k++) c[k] = new Chunk(k % 3 - 1, k / 3 - 1);
    }
    ~Nine() {
        for (Chunk* p : c) delete p;
    }
    // world coordinates, x and z in [-16, 32)
    void set(int x, int y, int z, uint16_t st) { c[((z + 16) >> 4) * 3 + ((x + 16) >> 4)]->set((x + 16) & 15, B + y, (z + 16) & 15, st); }
    uint16_t get(int x, int y, int z) const { return c[((z + 16) >> 4) * 3 + ((x + 16) >> 4)]->get((x + 16) & 15, B + y, (z + 16) & 15); }
    const Chunk* const* all() const { return c; }
};

static void floorNine(Nine& n) {
    for (int x = -16; x < 32; x++)
        for (int z = -16; z < 32; z++)
            for (int y = 0; y <= 3; y++) n.set(x, y, z, bs::Stone);
}

TEST(light_region_torch_crosses_the_border) {
    Nine n;
    floorNine(n);
    n.set(-2, 4, 8, bs::Torch);   // 2 blocks into the west neighbour
    ChunkLight L;
    CHECK(L.computeRegion(n.all()));
    CHECK_EQ(blockAtL(L, 0, 4, 8), 12);   // the centre's first column: 14 - 2
    CHECK_EQ(blockAtL(L, 5, 4, 8), 7);
    ChunkLight P;   // the per-chunk mode stops at the border
    CHECK(P.compute(*n.c[4], nullptr));
    CHECK_EQ(blockAtL(P, 0, 4, 8), 0);
}

TEST(light_region_overhang_across_the_border_shades) {
    Nine n;
    floorNine(n);
    // a roof over x in [-8, 8), z in [4, 12) at y = 8: half of it in the west neighbour
    for (int x = -8; x < 8; x++)
        for (int z = 4; z < 12; z++) n.set(x, 8, z, bs::Stone);
    ChunkLight L;
    CHECK(L.computeRegion(n.all()));
    CHECK_EQ(skyAt(L, 0, 5, 8), 11);   // 4 steps in from the nearest open side (z = 4 or 11)
    CHECK_EQ(skyAt(L, 7, 5, 8), 14);   // next to the open column at x = 8
    CHECK_EQ(skyAt(L, 8, 5, 8), 15);   // open column east of the roof
}

// Reference: relax the light rules over the whole 3x3 world until nothing changes.
static void referenceLight(const Nine& n, int H, std::vector<uint8_t>& sky, std::vector<uint8_t>& blk) {
    const int W = 48;
    auto idx = [&](int x, int y, int z) { return ((size_t)y * W + z) * W + x; };
    std::vector<uint8_t> f((size_t)W * W * H), e((size_t)W * W * H);
    for (int y = 0; y < H; y++)
        for (int z = 0; z < W; z++)
            for (int x = 0; x < W; x++) {
                const BlockDef& b = blockOf(n.get(x - 16, y, z - 16));
                f[idx(x, y, z)] = b.filterLight;
                e[idx(x, y, z)] = b.emitLight;
            }
    for (int pass = 0; pass < 2; pass++) {
        bool isSky = pass == 0;
        std::vector<uint8_t>& L = isSky ? sky : blk;
        L.assign((size_t)W * W * H, 0);
        if (isSky) {
            for (int z = 0; z < W; z++)
                for (int x = 0; x < W; x++)
                    for (int y = H - 1; y >= 0 && f[idx(x, y, z)] == 0; y--) L[idx(x, y, z)] = 15;
        } else {
            for (size_t i = 0; i < L.size(); i++) L[i] = e[i];
        }
        for (bool changed = true; changed;) {
            changed = false;
            for (int y = 0; y < H; y++)
                for (int z = 0; z < W; z++)
                    for (int x = 0; x < W; x++) {
                        size_t i = idx(x, y, z);
                        int fi = f[i];
                        if (fi >= 15) continue;
                        int best = L[i];
                        static const int D[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, -1, 0}};
                        for (const auto& d : D) {
                            int nx = x + d[0], ny = y + d[1], nz = z + d[2];
                            if (nx < 0 || nx >= W || nz < 0 || nz >= W || ny < 0 || ny >= H) continue;
                            int Ln = L[idx(nx, ny, nz)];
                            if (Ln <= 1) continue;
                            bool fromAbove = d[1] == 1;
                            int v = (isSky && fromAbove && Ln == 15 && fi == 0) ? 15 : Ln - (fi > 1 ? fi : 1);
                            if (v > best) best = v;
                        }
                        if (best != L[i]) { L[i] = (uint8_t)best; changed = true; }
                    }
        }
    }
}

TEST(light_region_matches_a_whole_world_reference) {
    static const uint16_t pick[] = {bs::Stone, bs::Stone, bs::Glass, bs::OakLeaves, bs::Water, bs::Torch, bs::Glowstone};
    uint32_t rng = 12345;
    auto rnd = [&](uint32_t m) { rng = rng * 1103515245u + 12345u; return (rng >> 8) % m; };
    for (int round = 0; round < 3; round++) {
        Nine n;
        floorNine(n);
        // random blocks in a 10-high slab, more of them near the centre's borders
        for (int k = 0; k < 2500; k++) {
            int x = -16 + (int)rnd(48), z = -16 + (int)rnd(48), y = 4 + (int)rnd(10);
            n.set(x, y, z, pick[rnd(sizeof(pick) / sizeof(pick[0]))]);
        }
        ChunkLight L;
        CHECK(L.computeRegion(n.all()));
        int H = L.sections() * 16;
        std::vector<uint8_t> sky, blk;
        referenceLight(n, H, sky, blk);
        int bad = 0;
        for (int y = 0; y < H; y++)
            for (int z = 0; z < 16; z++)
                for (int x = 0; x < 16; x++) {
                    size_t i = ((size_t)y * 48 + (z + 16)) * 48 + (x + 16);
                    if (skyAt(L, x, y, z) != sky[i] || blockAtL(L, x, y, z) != blk[i]) {
                        if (bad++ < 3)
                            printf("    (%d,%d,%d): sky %d vs %d, block %d vs %d\n", x, y, z, skyAt(L, x, y, z), sky[i],
                                   blockAtL(L, x, y, z), blk[i]);
                    }
                }
        CHECK_EQ(bad, 0);
    }
}

// Not a check: the relative cost of the two modes on generated terrain (the device
// numbers come from the firmware bench, test/hardware_bench.js).
TEST(light_region_cost_on_generated_terrain) {
    Generator gen;
    CHECK(gen.init(42, WORLD_NORMAL));
    const int R = 3;   // chunks -R..R; the inner ones have all neighbours
    std::vector<Chunk*> cs;
    auto at = [&](int cx, int cz) { return cs[(size_t)((cz + R) * (2 * R + 1) + (cx + R))]; };
    for (int cz = -R; cz <= R; cz++)
        for (int cx = -R; cx <= R; cx++) {
            Chunk* c = new Chunk(cx + 20, cz + 20);
            gen.generate(*c);
            cs.push_back(c);
        }
    ChunkLight L, LR;
    double perChunk = 0, region = 0;
    uint64_t pushC = 0, pushR = 0;
    int n = 0;
    for (int rep = 0; rep < 3; rep++)
        for (int cz = -R + 1; cz < R; cz++)
            for (int cx = -R + 1; cx < R; cx++) {
                const Chunk* nine[9];
                for (int k = 0; k < 9; k++) nine[k] = at(cx + k % 3 - 1, cz + k / 3 - 1);
                auto t0 = std::chrono::steady_clock::now();
                CHECK(L.compute(*at(cx, cz), nullptr));
                auto t1 = std::chrono::steady_clock::now();
                CHECK(LR.computeRegion(nine));
                auto t2 = std::chrono::steady_clock::now();
                pushC += L.skyPushes;
                pushR += LR.skyPushes;
                perChunk += std::chrono::duration<double, std::milli>(t1 - t0).count();
                region += std::chrono::duration<double, std::milli>(t2 - t1).count();
                n++;
            }
    printf("    per chunk %.3f ms, region %.3f ms (x%.1f), %d chunks; sky flood pushes %llu vs %llu\n", perChunk / n,
           region / n, region / perChunk, n, (unsigned long long)(pushC / n), (unsigned long long)(pushR / n));
    for (Chunk* c : cs) delete c;
}
