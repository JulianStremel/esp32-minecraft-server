// Generator version 3 (vanilla's terrain shape, vanilla_gen.cpp): plausible chunks, what a
// chunk costs, and the default staying version 2.
#include <string.h>
#include "testing.h"
#include "mc/platform.h"
#include "mc/registry.h"
#include "mc/world/chunk.h"
#include "mc/world/generator.h"
#include "mc/world/vanilla/aquifer.h"
#include "mc/world/vanilla/carvers.h"
#include "mc/world/vanilla/features.h"

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

// Version 7: vanilla's carvers. The same chunks with and without them: more air under the
// terrain (tunnels, rooms, canyons), the bedrock floor whole, nothing carved in the sky.
TEST(vanilla_generator_carvers) {
    Generator v6, v7;
    CHECK(v6.init(42, WORLD_NORMAL, GENERATOR_VANILLA_AQUIFERS));
    CHECK(v7.init(42, WORLD_NORMAL, GENERATOR_VANILLA_CARVERS));
    long under6 = 0, under7 = 0, bedrock = 0, carvedSky = 0, starts = 0, carved = 0;
    const int N = 6;
    uint64_t us = 0;
    for (int i = 0; i < N * N; i++) {
        int cx = (i % N) * 3 - 9, cz = (i / N) * 3 - 9;
        Chunk a(cx, cz), b(cx, cz);
        v6.generate(a);
        uint64_t t = plat::micros();
        v7.generate(b);
        us += plat::micros() - t;
        starts += vanilla::carverSteps.starts;
        carved += vanilla::carverSteps.carved;
        for (int z = 0; z < 16; z++)
            for (int x = 0; x < 16; x++) {
                bedrock += blockIdOf(b.get(x, -64, z)) == blk::Bedrock;
                for (int y = -60; y < 40; y++) {
                    under6 += stateIsAir(a.get(x, y, z));
                    under7 += stateIsAir(b.get(x, y, z));
                }
                for (int y = 300; y < 320; y++) carvedSky += !stateIsAir(b.get(x, y, z));
            }
    }
    printf("    %d chunks: %.1f ms a chunk on the PC; air in y -60..39: %ld without carvers, %ld with; "
           "starts around a chunk %ld, blocks carved a chunk %ld\n",
           N * N, us / 1000.0 / (N * N), under6, under7, starts / (N * N), carved / (N * N));
    CHECK(under7 > under6);
    CHECK_EQ(bedrock, (long)N * N * 256);
    CHECK_EQ(carvedSky, 0L);
}

// Version 8: vanilla's features. Over chunks spread across 6,000 blocks: trees, grass and
// flowers on the land, ores in the stone, seagrass and kelp in the sea, sugar cane by water;
// every chunk the same whatever the order (no feature reaches into another chunk).
TEST(vanilla_generator_features) {
    Generator g;
    CHECK(g.init(42, WORLD_NORMAL, GENERATOR_VANILLA_FEATURES));
    long logs = 0, leaves = 0, grass = 0, flowers = 0, coal = 0, iron = 0, diamond = 0, seagrass = 0, kelp = 0, cane = 0,
         snow = 0, skipped = 0, placed = 0;
    const int N = 10;
    uint64_t t0 = plat::micros();
    for (int i = 0; i < N * N; i++) {
        Chunk c((i % N) * 61 - 300, (i / N) * 61 - 300);
        g.generate(c);
        skipped += vanilla::featureSteps.skipped;
        placed += vanilla::featureSteps.placed;
        for (int y = c.minY(); y <= c.maxY(); y++)
            for (int z = 0; z < 16; z++)
                for (int x = 0; x < 16; x++) {
                    uint16_t id = blockIdOf(c.get(x, y, z));
                    const char* n = BLOCKS[id].name;
                    logs += strstr(n, "_log") != nullptr;
                    leaves += strstr(n, "leaves") != nullptr;
                    grass += id == blk::ShortGrass || id == blk::TallGrass || id == blk::Fern;
                    flowers += id == blk::Dandelion || id == blk::Poppy || id == blk::Cornflower || id == blk::OxeyeDaisy ||
                               id == blk::AzureBluet || strstr(n, "tulip");
                    coal += id == blk::CoalOre || id == blk::DeepslateCoalOre;
                    iron += id == blk::IronOre || id == blk::DeepslateIronOre;
                    diamond += id == blk::DiamondOre || id == blk::DeepslateDiamondOre;
                    seagrass += id == blk::Seagrass || id == blk::TallSeagrass;
                    kelp += id == blk::Kelp || id == blk::KelpPlant;
                    cane += id == blk::SugarCane;
                    snow += id == blk::Snow;
                }
    }
    double ms = (plat::micros() - t0) / 1000.0 / (N * N);
    printf("    %d chunks: %.1f ms a chunk on the PC; logs %ld, leaves %ld, grass %ld, flowers %ld, coal %ld, iron %ld, "
           "diamond %ld, seagrass %ld, kelp %ld, sugar cane %ld, snow %ld; placed features a chunk %ld (types not done: %ld)\n",
           N * N, ms, logs, leaves, grass, flowers, coal, iron, diamond, seagrass, kelp, cane, snow, placed / (N * N), skipped);
    CHECK(logs > 0 && leaves > logs && grass > 0 && flowers > 0);
    CHECK(coal > diamond && iron > diamond && diamond > 0);   // coal and iron about as common (vanilla since 1.18)
    CHECK(seagrass > 0 && kelp > 0);
    // the same chunk alone and after its neighbours
    Generator h;
    CHECK(h.init(42, WORLD_NORMAL, GENERATOR_VANILLA_FEATURES));
    Chunk a(5, 7);
    h.generate(a);
    for (int k = 0; k < 4; k++) {
        Chunk n(4 + (k & 1) * 2, 6 + (k >> 1) * 2);
        h.generate(n);
    }
    Chunk b(5, 7);
    h.generate(b);
    bool same = true;
    for (int y = a.minY(); y <= a.maxY() && same; y++)
        for (int z = 0; z < 16; z++)
            for (int x = 0; x < 16; x++) same &= a.get(x, y, z) == b.get(x, y, z);
    CHECK(same);
}

// Version 9: the Nether and the End from vanilla's routers. The Nether: bedrock at its
// floor and roof, a lava sea, its five biomes with their surfaces (soul sand, nylium,
// basalt), glowstone, quartz and fungi. The End: the main island with its spikes, the
// void beyond, outer islands with chorus plants and their biomes.
TEST(vanilla_generator_nether_and_end) {
    Generator n, e;
    CHECK(n.init(42, WORLD_NORMAL, GENERATOR_VANILLA_DIMS, DIM_NETHER));
    CHECK(e.init(42, WORLD_NORMAL, GENERATOR_VANILLA_DIMS, DIM_END));
    long floorBedrock = 0, roofBedrock = 0, lava = 0, netherrack = 0, soul = 0, nylium = 0, basalt = 0, glowstone = 0,
         quartz = 0, fungi = 0;
    bool biomes[256] = {};
    const int N = 8;
    uint64_t t0 = plat::micros();
    for (int i = 0; i < N * N; i++) {
        Chunk c((i % N) * 97 - 380, (i / N) * 97 - 380, DIM_NETHER);   // its biomes are large: 12,000 blocks
        n.generate(c);
        biomes[c.biome(0, 0)] = biomes[c.biome(8, 8)] = true;
        for (int y = c.minY(); y <= c.maxY(); y++)
            for (int z = 0; z < 16; z++)
                for (int x = 0; x < 16; x++) {
                    uint16_t id = blockIdOf(c.get(x, y, z));
                    floorBedrock += y == 0 && id == blk::Bedrock;
                    roofBedrock += y == 127 && id == blk::Bedrock;
                    lava += id == blk::Lava;
                    netherrack += id == blk::Netherrack;
                    soul += id == blk::SoulSand || id == blk::SoulSoil;
                    nylium += id == blk::CrimsonNylium || id == blk::WarpedNylium;
                    basalt += id == blk::Basalt || id == blk::Blackstone;
                    glowstone += id == blk::Glowstone;
                    quartz += id == blk::NetherQuartzOre;
                    fungi += id == blk::CrimsonStem || id == blk::WarpedStem || id == blk::CrimsonFungus || id == blk::WarpedFungus ||
                             id == blk::CrimsonRoots || id == blk::WarpedRoots;
                }
    }
    double nms = (plat::micros() - t0) / 1000.0 / (N * N);
    int kinds = 0;
    for (bool b : biomes) kinds += b;
    printf("    the Nether, %d chunks: %.1f ms a chunk on the PC; %d biomes; netherrack %ld, lava %ld, soul sand/soil %ld, "
           "nylium %ld, basalt/blackstone %ld, glowstone %ld, quartz %ld, fungi and roots %ld\n",
           N * N, nms, kinds, netherrack, lava, soul, nylium, basalt, glowstone, quartz, fungi);
    CHECK_EQ(floorBedrock, (long)N * N * 256);
    CHECK_EQ(roofBedrock, (long)N * N * 256);
    CHECK(lava > 0 && netherrack > lava && glowstone > 0 && quartz > 0);
    CHECK(kinds >= 3);
    CHECK(soul + nylium + basalt > 0);
    // the End
    long island = 0, obsidian = 0, voidBlocks = 0, outer = 0, chorus = 0;
    bool endBiomes[256] = {};
    t0 = plat::micros();
    for (int cx = -3; cx <= 2; cx++)
        for (int cz = -3; cz <= 2; cz++) {
            Chunk c(cx, cz, DIM_END);
            e.generate(c);
            for (int y = c.minY(); y <= c.maxY(); y++)
                for (int z = 0; z < 16; z++)
                    for (int x = 0; x < 16; x++) {
                        uint16_t id = blockIdOf(c.get(x, y, z));
                        island += id == blk::EndStone;
                        obsidian += id == blk::Obsidian;
                    }
        }
    for (int i = 0; i < 40; i++) {
        Chunk c(i % 2 == 0 ? 25 : 80 + i * 3, i % 2 == 0 ? (i % 8) - 4 : i * 2, DIM_END);   // 400 blocks out: the void
        e.generate(c);
        endBiomes[c.biome(0, 0)] = true;
        for (int y = c.minY(); y <= c.maxY(); y++)
            for (int z = 0; z < 16; z++)
                for (int x = 0; x < 16; x++) {
                    uint16_t id = blockIdOf(c.get(x, y, z));
                    if (c.cx == 25) voidBlocks += id != blk::Air;
                    else outer += id == blk::EndStone;
                    chorus += id == blk::ChorusPlant || id == blk::ChorusFlower;
                }
    }
    double ems = (plat::micros() - t0) / 1000.0 / 76;
    int endKinds = 0;
    for (bool b : endBiomes) endKinds += b;
    printf("    the End: %.1f ms a chunk on the PC; main island end stone %ld, spikes' obsidian %ld; 400 blocks out %ld blocks; "
           "outer islands %ld, chorus %ld, %d biomes; exit portal at y %d\n",
           ems, island, obsidian, voidBlocks, outer, chorus, endKinds, e.endSurfaceY(0, 0));
    CHECK(island > 10000 && obsidian > 1000);
    CHECK_EQ(voidBlocks, 0L);
    CHECK(outer > 0 && endKinds >= 2);   // (chorus grows in the end highlands only: printed)
    CHECK(e.endSurfaceY(0, 0) > 40);
}
