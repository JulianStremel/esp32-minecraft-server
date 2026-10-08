// The vanilla density-function prototype (lib/mcore/src/mc/world/vanilla): plausible
// terrain from the 1.21.8 noise router, and what it costs per chunk.
#include <string.h>
#include <vector>
#include "testing.h"
#include "mc/platform.h"
#include "mc/world/vanilla/density.h"

using namespace mc;
using namespace mc::vanilla;

TEST(vanilla_density_makes_terrain) {
    Router r;
    CHECK(r.init(42));
    std::vector<uint8_t> out(16 * 16 * DF_SHAPE.height);
    int land = 0, sea = 0, cols = 0, caves = 0;
    int heights[64] = {};
    uint64_t corner = 0, block = 0;
    const int N = 6;
    for (int cz = -N / 2; cz < N / 2; cz++)
        for (int cx = -N / 2; cx < N / 2; cx++) {
            r.fillChunk(cx * 37, cz * 37, out.data());   // spread out: different landscapes
            corner += r.cornerUs;
            block += r.blockUs;
            for (int z = 0; z < 16; z++)
                for (int x = 0; x < 16; x++) {
                    int top = -1;
                    for (int y = DF_SHAPE.height - 1; y >= 0; y--)
                        if (out[((size_t)y * 16 + z) * 16 + x]) { top = y + DF_SHAPE.minY; break; }
                    // air below the surface: caves
                    for (int y = 0; y + DF_SHAPE.minY < top - 8; y++) caves += !out[((size_t)y * 16 + z) * 16 + x];
                    cols++;
                    if (top >= DF_SHAPE.seaLevel) land++;
                    else sea++;
                    int bucket = (top + 64) / 6;
                    if (bucket >= 0 && bucket < 64) heights[bucket]++;
                }
        }
    printf("    %d chunks: land %d%%, sea %d%%, cave cells %d; %.1f ms per chunk (corners %.1f, blocks %.1f)\n", N * N,
           100 * land / cols, 100 * sea / cols, caves, (corner + block) / 1000.0 / (N * N), corner / 1000.0 / (N * N),
           block / 1000.0 / (N * N));
    printf("    surface heights (6-block bins from y -64): ");
    for (int i = 0; i < 64; i++)
        if (heights[i]) printf("%d:%d ", i * 6 - 64, heights[i]);
    printf("\n");
    CHECK(land > 0);
    CHECK(sea > 0);
    CHECK(caves > 0);
    // climate parameters are in vanilla's range
    for (int root = ROOT_TEMPERATURE; root <= ROOT_RIDGES; root++) {
        float lo = 1e9f, hi = -1e9f;
        for (int i = 0; i < 400; i++) {
            float v = r.sample(root, i * 97, 64, i * 61);
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        printf("    root %d: %.2f .. %.2f\n", root, lo, hi);
        CHECK(lo > -3 && hi < 3 && hi > lo);
    }
}

TEST(vanilla_density_approximations_against_exact) {
    struct Mode { float cut, margin; bool gen; } modes[] = {{0, 0, false}, {0, 0, true}, {1.0f / 64, 0, false}, {0, 0.1f, false},
                                                       {0, 0.25f, false}, {1.0f / 64, 0.1f, false}, {1.0f / 64, 0.1f, true}};
    const int N = 4;
    size_t vol = 16 * 16 * DF_SHAPE.height;
    std::vector<uint8_t> exact(vol * N * N), out(vol);
    for (const Mode& m : modes) {
        Router r;
        CHECK(r.init(42, m.cut, m.margin, false, m.gen));
        uint64_t us = 0;
        size_t diff = 0;
        uint32_t skipped = 0, total = 0;
        double heightDiff = 0;
        for (int i = 0; i < N * N; i++) {
            r.fillChunk((i % N) * 23 - 40, (i / N) * 23 - 40, out.data());
            us += r.cornerUs + r.blockUs;
            skipped += r.cellsSkipped;
            total += r.cellsTotal;
            uint8_t* ref = exact.data() + vol * i;
            if (m.cut == 0 && m.margin == 0 && !m.gen) memcpy(ref, out.data(), vol);
            for (size_t k = 0; k < vol; k++) diff += out[k] != ref[k];
            for (int c = 0; c < 256; c++) {
                int a = -1, b = -1;
                for (int y = DF_SHAPE.height - 1; y >= 0 && (a < 0 || b < 0); y--) {
                    if (a < 0 && out[(size_t)y * 256 + c]) a = y;
                    if (b < 0 && ref[(size_t)y * 256 + c]) b = y;
                }
                heightDiff += a > b ? a - b : b - a;
            }
        }
        printf("    octave cut %.4f, cell margin %.2f: %.1f ms/chunk, %.0f%% cells skipped, %.3f%% blocks differ, surface off by %.2f\n",
               m.cut, m.margin, us / 1000.0 / (N * N), total ? 100.0 * skipped / total : 0.0, 100.0 * diff / (vol * N * N),
               heightDiff / (256.0 * N * N));
        if (m.gen && m.cut == 0 && m.margin == 0) CHECK_EQ(diff, 0u);   // the same function, compiled
    }
}

// MC_MAPS=dir: writes height maps (32 x 32 chunks, int16 per column) of the current
// generator and of the vanilla prototype, for tools/worldgen/render_maps.js
#include <stdlib.h>
#include "mc/world/chunk.h"
#include "mc/world/generator.h"
TEST(vanilla_density_height_maps) {
    const char* dir = getenv("MC_MAPS");
    if (!dir) return;
    const int C = getenv("MC_MAPS_CHUNKS") ? atoi(getenv("MC_MAPS_CHUNKS")) : 32;
    std::vector<int16_t> cur(C * 16 * C * 16), van(C * 16 * C * 16);
    Generator g;
    g.init(42, WORLD_NORMAL);
    Router r;
    CHECK(r.init(42, 1.0f / 64, 0.1f, true, true));
    std::vector<uint8_t> out(16 * 16 * DF_SHAPE.height);
    for (int cz = 0; cz < C; cz++)
        for (int cx = 0; cx < C; cx++) {
            Chunk c(cx - C / 2, cz - C / 2);
            g.generate(c);
            r.fillChunk(cx - C / 2, cz - C / 2, out.data());
            for (int z = 0; z < 16; z++)
                for (int x = 0; x < 16; x++) {
                    size_t i = (size_t)(cz * 16 + z) * C * 16 + cx * 16 + x;
                    cur[i] = (int16_t)(c.height(x, z) - 1);
                    int top = DF_SHAPE.minY - 1;
                    for (int y = DF_SHAPE.height - 1; y >= 0; y--)
                        if (out[((size_t)y * 16 + z) * 16 + x]) { top = y + DF_SHAPE.minY; break; }
                    van[i] = (int16_t)top;
                }
        }
    char path[512];
    snprintf(path, sizeof(path), "%s/current.bin", dir);
    FILE* f = fopen(path, "wb");
    fwrite(cur.data(), 2, cur.size(), f);
    fclose(f);
    snprintf(path, sizeof(path), "%s/vanilla.bin", dir);
    f = fopen(path, "wb");
    fwrite(van.data(), 2, van.size(), f);
    fclose(f);
}

// The board's /vanillabench prints the same fingerprints for chunks (i * 7, i * 3)
// when float arithmetic matches (settings 0.0156 0.1 1 1).
TEST(vanilla_density_fingerprint) {
    Router r;
    CHECK(r.init(42, 1.0f / 64, 0.1f, true, true));
    std::vector<uint8_t> out(16 * 16 * DF_SHAPE.height);
    for (int i = 0; i < 3; i++) {
        r.fillChunk(i * 7, i * 3, out.data());
        uint32_t h = 2166136261u;
        for (uint8_t v : out) h = (h ^ v) * 16777619u;
        printf("    chunk %d: fingerprint %08x\n", i, (unsigned)h);
    }
}
