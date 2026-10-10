// Generator version 3 (vanilla's terrain shape, vanilla_gen.cpp): plausible chunks, what a
// chunk costs, and the default staying version 2.
#include <string.h>
#include "testing.h"
#include "mc/platform.h"
#include "mc/registry.h"
#include "mc/world/chunk.h"
#include "mc/world/generator.h"
#include "mc/world/vanilla/aquifer.h"

using namespace mc;

TEST(vanilla_generator_makes_a_world) {
    CHECK_EQ(GENERATOR_LATEST, 2);   // new worlds keep the classic terrain unless they choose
    Generator g;
    CHECK(g.init(42, WORLD_NORMAL, GENERATOR_VANILLA));
    int bedrock = 0, deepslate = 0, stone = 0, water = 0, grass = 0, logs = 0, chunks = 0;
    bool biomeSeen[256] = {};
    int biomes = 0;
    uint64_t t0 = plat::micros();
    for (int cz = -3; cz < 3; cz++)
        for (int cx = -3; cx < 3; cx++) {
            Chunk c(cx, cz);
            g.generate(c);
            chunks++;
            for (int y = c.minY(); y <= c.maxY(); y++)
                for (int z = 0; z < 16; z++)
                    for (int x = 0; x < 16; x++) {
                        uint16_t st = c.get(x, y, z);
                        uint16_t id = blockIdOf(st);
                        if (y == c.minY()) bedrock += id == blk::Bedrock;
                        if (y < -8 && id == blk::Deepslate) deepslate++;
                        if (y > 8 && id == blk::Stone) stone++;
                        if (id == blk::Water) water++;
                        if (id == blk::GrassBlock) grass++;
                        if (strstr(BLOCKS[id].name, "_log")) logs++;
                    }
            for (int z = 0; z < 16; z += 4)
                for (int x = 0; x < 16; x += 4) {
                    uint8_t b = c.biome(x, z);
                    if (!biomeSeen[b]) { biomeSeen[b] = true; biomes++; }
                }
        }
    double ms = (plat::micros() - t0) / 1000.0 / chunks;
    printf("    36 chunks: %.1f ms a chunk on the PC; bedrock %d, deepslate %d, stone %d, water %d, grass %d, logs %d, %d biomes\n",
           ms, bedrock, deepslate, stone, water, grass, logs, biomes);
    CHECK_EQ(bedrock, 36 * 256);   // the floor is whole
    CHECK(deepslate > 36 * 256 * 20);
    CHECK(stone > 36 * 256 * 10);
    CHECK(water > 0 && grass > 0 && logs > 0);
    CHECK(biomes >= 2);
    int x, y, z;
    g.findSpawn(x, y, z);
    printf("    spawn %d %d %d\n", x, y, z);
    CHECK(y > 62 && y < 120);
}

// Version 4: vanilla's biomes from the climate. Over chunks spread across 3,000 blocks:
// most of the overworld's biomes, oceans and land in plausible shares, cave biomes only
// underground (lush and dripstone caves, the deep dark far down: vanilla places it by the
// router's depth, so it is higher under mountains), a column's 2D biome the one at its surface.
TEST(vanilla_generator_biomes) {
    Generator g;
    CHECK(g.init(42, WORLD_NORMAL, GENERATOR_VANILLA_BIOMES));
    int count[256] = {}, cells = 0, ocean = 0, sea = 0, caveCells = 0, caveHigh = 0, deepDarkHigh = 0;
    const int N = 12;
    uint64_t t0 = plat::micros();
    for (int i = 0; i < N * N; i++) {
        Chunk c((i % N) * 61 - 360, (i / N) * 61 - 360);
        g.generate(c);
        for (int q = 0; q < 16; q++) {
            uint8_t b = c.biome((q & 3) * 4, (q >> 2) * 4);
            count[b]++;
            cells++;
            ocean += b == biome::Ocean || b == biome::DeepOcean || b == biome::ColdOcean || b == biome::DeepColdOcean ||
                     b == biome::LukewarmOcean || b == biome::DeepLukewarmOcean || b == biome::WarmOcean ||
                     b == biome::FrozenOcean || b == biome::DeepFrozenOcean;
            CHECK(b != biome::LushCaves && b != biome::DripstoneCaves && b != biome::DeepDark);
            int top = c.height((q & 3) * 4 + 2, (q >> 2) * 4 + 2);
            int floor = top;   // the highest block that is not water
            while (floor > c.minY() && blockIdOf(c.get((q & 3) * 4 + 2, floor - 1, (q >> 2) * 4 + 2)) == blk::Water) floor--;
            sea += floor < SEA_LEVEL - 1;
            for (int y = c.minY(); y <= c.maxY(); y += 4) {
                uint8_t b3 = c.biomeAt((q & 3) * 4, y, (q >> 2) * 4);
                bool cave = b3 == biome::LushCaves || b3 == biome::DripstoneCaves || b3 == biome::DeepDark;
                caveCells += cave;
                if (cave && y > top + 8) caveHigh++;
                if (b3 == biome::DeepDark && y > top - 48) deepDarkHigh++;   // by depth: lower under mountains, but deep
                if (cave) count[b3]++;
            }
        }
    }
    double ms = (plat::micros() - t0) / 1000.0 / (N * N);
    int kinds = 0;
    for (int b = 0; b < 256; b++) kinds += count[b] > 0;
    printf("    %d chunks: %.1f ms a chunk on the PC; %d biomes, oceans %d%% of the columns (under water: %d%%), cave cells %d\n    ",
           N * N, ms, kinds, 100 * ocean / cells, 100 * sea / cells, caveCells);
    for (int b = 0; b < 256; b++)
        if (count[b]) printf("%d:%d ", b, count[b]);   // biome id (biome_ids_gen.h): cells
    printf("\n");
    CHECK(kinds >= 25);
    CHECK(ocean * 100 / cells > 15 && ocean * 100 / cells < 60);
    CHECK(ocean <= sea);   // ocean biomes are under water (rivers and lakes are too)
    CHECK(caveCells > 0);
    CHECK_EQ(caveHigh, 0);       // cave biomes stay below the surface
    CHECK_EQ(deepDarkHigh, 0);   // the deep dark is far below the surface
}

// Version 5: vanilla's surface rule. Over chunks spread across 6,000 blocks, what the top
// of the land and of the sea floor is made of: grass most of all, sand on beaches and in
// deserts, terracotta in badlands, snow up high, gravel under the sea; little bare stone.
TEST(vanilla_generator_surface_rule) {
    Generator g;
    CHECK(g.init(42, WORLD_NORMAL, GENERATOR_VANILLA_SURFACE));
    int land = 0, under = 0;
    static int tops[NUM_BLOCKS], floors[NUM_BLOCKS];   // block id: count
    memset(tops, 0, sizeof(tops));
    memset(floors, 0, sizeof(floors));
    const int N = 12;
    uint64_t t0 = plat::micros();
    for (int i = 0; i < N * N; i++) {
        Chunk c((i % N) * 61 - 360, (i / N) * 61 - 360);
        g.generate(c);
        for (int z = 0; z < 16; z += 2)
            for (int x = 0; x < 16; x += 2) {
                int y = c.height(x, z) - 1;
                uint16_t id = blockIdOf(c.get(x, y, z));
                while (y > c.minY() && (id == blk::Water || id == blk::Snow || id == blk::ShortGrass || id == blk::TallGrass ||
                                        strstr(BLOCKS[id].name, "leaves") || strstr(BLOCKS[id].name, "_log") ||
                                        strstr(BLOCKS[id].name, "flower") || id == blk::Air)) {
                    bool water = id == blk::Water;
                    y--;
                    id = blockIdOf(c.get(x, y, z));
                    if (water && id != blk::Water) { floors[id]++; under++; goto next; }
                }
                tops[id]++;
                land++;
            next:;
            }
    }
    double ms = (plat::micros() - t0) / 1000.0 / (N * N);
    printf("    %d chunks: %.1f ms a chunk on the PC\n    land tops:", N * N, ms);
    for (int id = 0; id < NUM_BLOCKS; id++)
        if (tops[id] * 200 >= land) printf(" %s %d%%", BLOCKS[id].name, tops[id] * 100 / land);
    printf("\n    sea floors:");
    for (int id = 0; id < NUM_BLOCKS; id++)
        if (floors[id] * 200 >= under) printf(" %s %d%%", BLOCKS[id].name, floors[id] * 100 / under);
    printf("\n");
    CHECK(tops[blk::GrassBlock] * 100 / land >= 30);   // most land is green
    CHECK(tops[blk::Stone] * 100 / land <= 25);        // bare stone: peaks, shores, cliffs
    CHECK(tops[blk::Sand] > 0);
    CHECK(floors[blk::Gravel] + floors[blk::Sand] > under / 2);
}

// Version 6: vanilla's aquifers and ore veins. Under the terrain (below each column's top,
// y -54..62) the caves are mostly dry, with water and lava at their own levels; copper
// veins (with granite) in y 0..50 and iron veins (with tuff) in y -60..-8 (their raw
// blocks nowhere else).
TEST(vanilla_generator_aquifers_and_veins) {
    Generator g;
    CHECK(g.init(42, WORLD_NORMAL, GENERATOR_VANILLA_AQUIFERS));
    int air = 0, water = 0, lava = 0, waterAboveSea = 0, copper = 0, iron = 0, granite = 0, tuff = 0, misplaced = 0;
    const int N = 8;
    uint64_t t0 = plat::micros();
    for (int i = 0; i < N * N; i++) {
        Chunk c((i % N) * 61 - 240, (i / N) * 61 - 240);
        g.generate(c);
        for (int z = 0; z < 16; z++)
            for (int x = 0; x < 16; x++) {
                int top = -64;
                for (int y = c.maxY(); y >= c.minY(); y--) {
                    uint16_t id = blockIdOf(c.get(x, y, z));
                    if (id != blk::Air && id != blk::Water && !strstr(BLOCKS[id].name, "leaves") && !strstr(BLOCKS[id].name, "_log")) {
                        top = y;
                        break;
                    }
                }
                for (int y = -64; y < top; y++) {
                    uint16_t id = blockIdOf(c.get(x, y, z));
                    if (y >= -54 && y < 63) {
                        air += id == blk::Air || id == blk::CaveAir;
                        water += id == blk::Water;
                        lava += id == blk::Lava;
                    }
                    waterAboveSea += id == blk::Water && y >= 63;
                    bool cu = id == blk::CopperOre || id == blk::RawCopperBlock, fe = id == blk::DeepslateIronOre || id == blk::RawIronBlock;
                    copper += cu;
                    iron += fe;
                    granite += id == blk::Granite;
                    tuff += id == blk::Tuff;
                    // raw ore blocks come only from veins (the scattered ores are placed anywhere)
                    if ((id == blk::RawCopperBlock && (y < 0 || y > 50)) || (id == blk::RawIronBlock && (y < -60 || y > -8)))
                        misplaced++;
                }
            }
    }
    double ms = (plat::micros() - t0) / 1000.0 / (N * N);
    {
        const vanilla::AquiferSteps& as = vanilla::aquiferSteps;
        printf("    the last chunk: aquifer %u us (%u cave blocks, %u statuses, %u barriers), veins %u us (%u blocks)\n",
               (unsigned)as.blocksUs, (unsigned)as.cave, (unsigned)as.statuses, (unsigned)as.barriers, (unsigned)as.veinsUs,
               (unsigned)as.veinBlocks);
    }
    printf("    %d chunks: %.1f ms a chunk on the PC; caves y -54..62: air %d, water %d, lava %d; water above the sea %d\n"
           "    veins: copper %d (granite %d), iron %d (tuff %d)\n",
           N * N, ms, air, water, lava, waterAboveSea, copper, granite, iron, tuff);
    CHECK(air > water);     // dry caves: no longer flooded to the sea level
    CHECK(water > 0 && lava > 0);
    CHECK(copper > 0 && iron > 0 && granite > copper && tuff > iron);
    CHECK_EQ(misplaced, 0);
}
