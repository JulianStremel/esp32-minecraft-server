// The Nether and the End: their terrain, and the world, storage and server keeping the
// three dimensions apart.
#include <string.h>
#include "testing.h"
#include "mc/registry.h"
#include "mc/server/server.h"
#include "mc/storage/block_device.h"
#include "mc/storage/world_store.h"
#include "mc/world/generator.h"
#include "mc/world/light.h"

using namespace mc;

TEST(dimension_generators_match_golden_fingerprints) {
    for (int i = 0; i < NUM_GENERATOR_DIM_GOLDEN; i++) {
        const GeneratorDimGolden& g = GENERATOR_DIM_GOLDEN[i];
        uint32_t nether = generatorDimFingerprint(g.seed, DIM_NETHER), end = generatorDimFingerprint(g.seed, DIM_END);
        if (nether != g.nether || end != g.end)
            printf("    seed %llu: nether %08x end %08x, expected %08x %08x\n", (unsigned long long)g.seed,
                   (unsigned)nether, (unsigned)end, (unsigned)g.nether, (unsigned)g.end);
        CHECK_EQ(nether, g.nether);
        CHECK_EQ(end, g.end);
    }
    // the overworld's fingerprint is not affected by the other dimensions
    CHECK_EQ(generatorFingerprint(42, 2), GENERATOR_GOLDEN[3].blocks);
}

TEST(nether_terrain_is_plausible) {
    Generator g;
    CHECK(g.init(42, WORLD_NORMAL, GENERATOR_LATEST, DIM_NETHER));
    const int R = 3;   // 7 x 7 chunks
    long solid[8] = {}, cells[8] = {};
    long lava = 0, glow = 0, quartz = 0, gold = 0, soul = 0, wrongBedrock = 0, above = 0, lavaHigh = 0;
    for (int cz = -R; cz <= R; cz++)
        for (int cx = -R; cx <= R; cx++) {
            Chunk* c = new Chunk(cx, cz, DIM_NETHER);
            g.generate(*c);
            CHECK_EQ(c->biome(0, 0), biome::NetherWastes);
            for (int z = 0; z < 16; z++)
                for (int x = 0; x < 16; x++) {
                    if (c->get(x, 0, z) != bs::Bedrock || c->get(x, 127, z) != bs::Bedrock) wrongBedrock++;
                    for (int y = 128; y < WORLD_HEIGHT; y++) above += c->get(x, y, z) != 0;
                    for (int y = 1; y < 127; y++) {
                        uint16_t b = c->get(x, y, z);
                        cells[y / 16]++;
                        if (b == bs::Lava) {
                            lava++;
                            if (y > 31) lavaHigh++;
                        } else if (b != bs::Air) {
                            solid[y / 16]++;
                        }
                        glow += b == bs::Glowstone;
                        quartz += b == bs::NetherQuartzOre;
                        gold += b == bs::NetherGoldOre;
                        soul += b == bs::SoulSand;
                    }
                }
            delete c;
        }
    printf("    solid by 16-block band:");
    for (int i = 0; i < 8; i++) printf(" %d%%", (int)(solid[i] * 100 / cells[i]));
    printf("\n    lava %ld glowstone %ld quartz %ld gold %ld soul sand %ld\n", lava, glow, quartz, gold, soul);
    CHECK_EQ(wrongBedrock, 0);
    CHECK_EQ(above, 0);       // a ceiling at 127, nothing above
    CHECK_EQ(lavaHigh, 0);    // the sea ends at 31 (no lava flows yet)
    CHECK(lava > 0);
    CHECK(glow > 0);
    CHECK(quartz > 0);
    CHECK(gold > 0);
    CHECK(soul > 0);
    // a floor and a ceiling that are mostly solid, caves in between
    CHECK(solid[0] * 100 / cells[0] > 40);
    CHECK(solid[7] * 100 / cells[7] > 60);
    for (int i = 2; i <= 5; i++) {
        CHECK(solid[i] * 100 / cells[i] > 15);
        CHECK(solid[i] * 100 / cells[i] < 85);
    }
}

TEST(end_is_an_island_in_the_void) {
    Generator g;
    CHECK(g.init(42, WORLD_NORMAL, GENERATOR_LATEST, DIM_END));
    Chunk* c = new Chunk(0, 0, DIM_END);
    g.generate(*c);
    CHECK_EQ(c->biome(0, 0), biome::TheEnd);
    int top = c->height(0, 0) - 1;
    CHECK(top >= 55 && top <= 70);
    CHECK_EQ(c->get(0, top, 0), bs::EndStone);
    CHECK_EQ(c->get(0, top - 30, 0), bs::EndStone);   // deep under the centre
    delete c;
    // the vanilla spawn platform at (100, 48, 0) is over the void
    Chunk* p = new Chunk(6, 0, DIM_END);
    g.generate(*p);
    for (int y = 0; y < WORLD_HEIGHT; y++) CHECK_EQ(p->get(100 & 15, y, 0), 0);
    delete p;
    Chunk* far = new Chunk(40, -40, DIM_END);
    g.generate(*far);
    CHECK_EQ(far->highestSection(), -1);
    delete far;
}

TEST(the_overworld_is_unchanged_by_dimension_support) {
    Generator a, b;
    CHECK(a.init(7, WORLD_NORMAL));
    CHECK(b.init(7, WORLD_NORMAL, GENERATOR_LATEST, DIM_OVERWORLD));
    Chunk x(3, -2), y(3, -2);
    a.generate(x);
    b.generate(y);
    for (int k = 0; k < 16 * 16 * WORLD_HEIGHT; k++) CHECK_EQ(x.get(k & 15, k >> 8, (k >> 4) & 15), y.get(k & 15, k >> 8, (k >> 4) & 15));
}

TEST(nether_and_end_light_has_no_sky) {
    Generator g;
    CHECK(g.init(42, WORLD_NORMAL, GENERATOR_LATEST, DIM_NETHER));
    Chunk* c = new Chunk(0, 0, DIM_NETHER);
    g.generate(*c);
    ChunkLight L;
    NeighbourEdges none;
    CHECK(L.compute(*c, none));
    CHECK(!L.hasSky());
    int maxSky = 0, lit = 0;
    for (int y = 0; y < 140; y++)
        for (int z = 0; z < 16; z++)
            for (int x = 0; x < 16; x++) {
                int s = L.skyAt(x, y, z);
                if (s > maxSky) maxSky = s;
                lit += L.blockAt(x, y, z) > 0;
            }
    CHECK_EQ(maxSky, 0);
    CHECK(lit > 0);   // the lava sea and glowstone
    delete c;
}

TEST(world_keeps_dimensions_apart) {
    Generator o, n, e;
    CHECK(o.init(42, WORLD_FLAT));
    CHECK(n.init(42, WORLD_FLAT, GENERATOR_LATEST, DIM_NETHER));
    CHECK(e.init(42, WORLD_FLAT, GENERATOR_LATEST, DIM_END));
    Generator* const gens[NUM_DIMS] = {&o, &n, &e};
    World w;
    w.init(gens, nullptr, 64, 100);
    // the same coordinates are three different chunks
    Chunk* a = w.load(DIM_OVERWORLD, 0, 0);
    Chunk* b = w.load(DIM_NETHER, 0, 0);
    Chunk* c = w.load(DIM_END, 0, 0);
    CHECK(a && b && c && a != b && b != c && a != c);
    CHECK_EQ(a->dim, DIM_OVERWORLD);
    CHECK_EQ(b->dim, DIM_NETHER);
    CHECK_EQ(c->dim, DIM_END);
    CHECK_EQ(w.getBlock(DIM_OVERWORLD, 0, 0, 0), bs::Bedrock);
    CHECK_EQ(w.getBlock(DIM_NETHER, 0, 0, 0), bs::Bedrock);
    CHECK_EQ(w.getBlock(DIM_NETHER, 0, 3, 0) != bs::GrassBlock, true);   // a flat world's Nether is still the Nether
    w.setBlock(DIM_NETHER, 1, 100, 1, bs::Glowstone);
    CHECK_EQ(w.getBlock(DIM_NETHER, 1, 100, 1), bs::Glowstone);
    CHECK_EQ(w.getBlock(DIM_OVERWORLD, 1, 100, 1), 0);
    CHECK_EQ(w.getBlock(DIM_END, 1, 100, 1), 0);
    CHECK(w.isResident(DIM_NETHER, 0, 0));
    CHECK(!w.isResident(DIM_NETHER, 5, 5));
    CHECK(w.isResident(DIM_OVERWORLD, 0, 0));
}

TEST(players_keep_their_dimension_in_storage) {
    MemDevice dev(64u << 20);
    WorldStore ws(&dev);
    StoreParams sp;
    sp.radius = 64;
    CHECK(ws.open(sp, true));
    PlayerData p;
    for (int i = 0; i < 16; i++) p.uuid[i] = (uint8_t)(i * 7 + 1);
    snprintf(p.name, sizeof(p.name), "Traveller");
    p.x = 12.5; p.y = 70; p.z = -3.5;
    p.dim = DIM_NETHER;
    CHECK(ws.savePlayer(p));
    PlayerData q;
    CHECK(ws.loadPlayer(p.uuid, q));
    CHECK_EQ(q.dim, DIM_NETHER);
    CHECK(q.x == 12.5 && q.y == 70 && q.z == -3.5);
    p.dim = DIM_END;
    CHECK(ws.savePlayer(p));
    CHECK(ws.loadPlayer(p.uuid, q));
    CHECK_EQ(q.dim, DIM_END);
}

TEST(nether_chunks_are_stored_apart_from_the_overworld) {
    MemDevice dev(64u << 20);
    StoreParams sp;
    sp.radius = 64;
    {
        WorldStore ws(&dev);
        CHECK(ws.open(sp, true));
        Chunk o(2, 3, DIM_OVERWORLD), n(2, 3, DIM_NETHER);
        o.set(1, 10, 1, bs::Stone);
        n.set(1, 10, 1, bs::Netherrack);
        CHECK(ws.saveChunk(o));
        CHECK(ws.saveChunk(n));
        CHECK(ws.flush());
    }
    WorldStore ws(&dev);
    CHECK(ws.open(sp, false));
    Chunk o(2, 3, DIM_OVERWORLD), n(2, 3, DIM_NETHER), e(2, 3, DIM_END);
    CHECK_EQ(ws.loadChunk(o), LOAD_OK);
    CHECK_EQ(ws.loadChunk(n), LOAD_OK);
    CHECK_EQ(ws.loadChunk(e), LOAD_ABSENT);
    CHECK_EQ(o.get(1, 10, 1), bs::Stone);
    CHECK_EQ(n.get(1, 10, 1), bs::Netherrack);
}

TEST(dimension_names_parse) {
    CHECK_EQ(parseDimension("overworld"), DIM_OVERWORLD);
    CHECK_EQ(parseDimension("minecraft:the_nether"), DIM_NETHER);
    CHECK_EQ(parseDimension("nether"), DIM_NETHER);
    CHECK_EQ(parseDimension("the_end"), DIM_END);
    CHECK_EQ(parseDimension("end"), DIM_END);
    CHECK_EQ(parseDimension("moon"), -1);
    CHECK(!strcmp(dimensionName(DIM_NETHER), "the_nether"));
    CHECK(!strcmp(DIMENSION_NAME[DIM_END], "minecraft:the_end"));
    for (int d = 0; d < NUM_DIMS; d++) CHECK(DIMENSION_NBT_LEN[d] > 100);
}
