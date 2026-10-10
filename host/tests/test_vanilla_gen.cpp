// Generator version 3 (vanilla's terrain shape, vanilla_gen.cpp): plausible chunks, what a
// chunk costs, and the default staying version 2.
#include <string.h>
#include "testing.h"
#include "mc/platform.h"
#include "mc/registry.h"
#include "mc/world/chunk.h"
#include "mc/world/generator.h"

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
