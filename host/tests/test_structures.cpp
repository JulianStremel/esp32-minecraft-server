// Generator version 10: vanilla's structures (mc/world/vanilla/structures.cpp): where
// they stand, that each is whole across its chunks in whatever order they are generated,
// and their chests.
#include <string.h>
#include "testing.h"
#include "mc/platform.h"
#include "mc/registry.h"
#include "mc/world/chunk.h"
#include "mc/world/generator.h"
#include "mc/world/vanilla/java_random.h"
#include "mc/world/vanilla/structures.h"
#include "mc/storage/block_device.h"
#include "mc/storage/world_store.h"

using namespace mc;

namespace {
int countBlock(const Chunk& c, uint16_t blockId) {
    int n = 0;
    for (int y = c.minY(); y <= c.maxY(); y++)
        for (int z = 0; z < 16; z++)
            for (int x = 0; x < 16; x++) n += blockIdOf(c.get(x, y, z)) == blockId;
    return n;
}
int lootItems(const Chunk& c, uint8_t type) {
    int n = 0;
    for (const TileEntity* t = c.tiles(); t; t = t->next)
        if (t->type == type)
            for (const ItemStack& s : t->items) n += !s.empty();
    return n;
}
int tilesOf(const Chunk& c, uint8_t type) {
    int n = 0;
    for (const TileEntity* t = c.tiles(); t; t = t->next) n += t->type == type;
    return n;
}
// the seeds' first structure of a kind within the radius, searched from the origin
bool find(Generator& g, const char* name, int radius, int& cx, int& cz) { return g.locateStructure(name, 0, 0, radius, cx, cz); }
}  // namespace

TEST(java_random_matches_java) {
    // values of new java.util.Random(42)
    vanilla::JavaRandom r(42);
    CHECK_EQ(r.nextInt(), -1170105035);
    vanilla::JavaRandom r10(42);
    CHECK_EQ(r10.nextInt(10), 0);
    vanilla::JavaRandom l(0);
    CHECK_EQ(l.nextLong(), -4962768465676381896LL);
    vanilla::JavaRandom d(1);
    double v = d.nextDouble();
    CHECK(v > 0.73087819 && v < 0.73087820);   // 0.7308781907032909
}

TEST(structures_placement_is_vanillas_grid) {
    // a desert pyramid's candidate chunk in each 32 x 32 region: in its first 24 chunks
    Generator g;
    CHECK(g.init(42, WORLD_NORMAL, GENERATOR_VANILLA_STRUCTURES));
    const vanilla::StructureSetDef* pyramids = nullptr;
    for (int s = 0; s < vanilla::SD_NUM_SETS; s++)
        if (vanilla::SD_SETS[s].salt == 14357617) pyramids = &vanilla::SD_SETS[s];
    CHECK(pyramids != nullptr);
    CHECK_EQ(pyramids->spacing, 32);
    CHECK_EQ(pyramids->separation, 8);
}

TEST(structures_desert_pyramid_is_whole) {
    Generator g;
    CHECK(g.init(42, WORLD_NORMAL, GENERATOR_VANILLA_STRUCTURES));
    int cx = 0, cz = 0;
    uint64_t t0 = plat::micros();
    bool found = find(g, "desert_pyramid", 6000, cx, cz);
    printf("    desert pyramid at chunk %d %d (%s, %.0f ms)\n", cx, cz, found ? "found" : "none", (plat::micros() - t0) / 1000.0);
    CHECK(found);
    if (!found) return;
    // its 21 x 21 blocks reach 2 x 2 chunks: one blue terracotta in the middle, 4 chests below
    int blue = 0, chests = 0, items = 0, tnt = 0;
    Chunk* first[4];
    for (int k = 0; k < 4; k++) {
        Chunk* c = new Chunk(cx + (k & 1), cz + (k >> 1));
        g.generate(*c);
        blue += countBlock(*c, (uint16_t)findBlock("blue_terracotta"));
        tnt += countBlock(*c, (uint16_t)findBlock("tnt"));
        chests += tilesOf(*c, TILE_CHEST);
        items += lootItems(*c, TILE_CHEST);
        first[k] = c;
    }
    printf("    blue terracotta %d, tnt %d, chests %d with %d stacks\n", blue, tnt, chests, items);
    CHECK_EQ(blue, 1);
    CHECK_EQ(tnt, 9);   // vanilla's 3 x 3 under the plate
    CHECK_EQ(chests, 4);
    CHECK(items >= 8);
    // the same chunks from a fresh generator, in the other order: the same blocks
    Generator h;
    CHECK(h.init(42, WORLD_NORMAL, GENERATOR_VANILLA_STRUCTURES));
    for (int k = 3; k >= 0; k--) {
        Chunk c(cx + (k & 1), cz + (k >> 1));
        h.generate(c);
        int diff = 0;
        for (int y = c.minY(); y <= c.maxY(); y++)
            for (int z = 0; z < 16; z++)
                for (int x = 0; x < 16; x++) diff += c.get(x, y, z) != first[k]->get(x, y, z);
        CHECK_EQ(diff, 0);
    }
    for (Chunk* c : first) delete c;
}

TEST(structures_temples_huts_and_treasure) {
    struct Want { const char* name; const char* block; int radius; };
    const Want wants[] = {
        {"jungle_pyramid", "tripwire_hook", 12000},
        {"swamp_hut", "cauldron", 12000},
        {"buried_treasure", "chest", 3000},
    };
    for (const Want& w : wants) {
        bool ok = false;
        for (uint64_t seed = 1; seed <= 6 && !ok; seed++) {
            Generator g;
            CHECK(g.init(seed, WORLD_NORMAL, GENERATOR_VANILLA_STRUCTURES));
            int cx, cz;
            if (!find(g, w.name, w.radius, cx, cz)) continue;
            int n = 0, items = 0;
            for (int k = 0; k < 4; k++) {
                Chunk c(cx + (k & 1), cz + (k >> 1));
                g.generate(c);
                n += countBlock(c, (uint16_t)findBlock(w.block));
                items += lootItems(c, TILE_CHEST);
            }
            printf("    %s: seed %llu chunk %d %d, %d %s, %d chest stacks\n", w.name, (unsigned long long)seed, cx, cz, n,
                   w.block, items);
            CHECK(n > 0);
            if (!strcmp(w.name, "swamp_hut")) CHECK_EQ(n, 1);
            if (!strcmp(w.name, "jungle_pyramid")) CHECK(items > 0);
            if (!strcmp(w.name, "buried_treasure")) CHECK(items > 0);
            ok = true;
        }
        CHECK(ok);
    }
}

TEST(structures_leave_older_versions_alone) {
    Generator a, b;
    CHECK(a.init(42, WORLD_NORMAL, GENERATOR_VANILLA_DIMS));
    CHECK(b.init(42, WORLD_NORMAL, GENERATOR_VANILLA_STRUCTURES));
    int cx = 0, cz = 0;
    CHECK(!a.locateStructure("desert_pyramid", 0, 0, 6000, cx, cz));
}

TEST(structures_cost_per_chunk) {
    // version 9 and 10 over the same 64 chunks: what finding and placing structures adds
    double ms[2] = {};
    int starts = 0, evaluated = 0, pieces = 0;
    for (int v = 0; v < 2; v++) {
        Generator g;
        CHECK(g.init(42, WORLD_NORMAL, v ? GENERATOR_VANILLA_STRUCTURES : GENERATOR_VANILLA_DIMS));
        uint64_t t0 = plat::micros();
        for (int i = 0; i < 64; i++) {
            Chunk c(-80 + (i & 7), -58 + (i >> 3));
            g.generate(c);
            if (v) {
                starts += vanilla::structureStats.starts;
                evaluated += vanilla::structureStats.evaluated;
                pieces += vanilla::structureStats.pieces;
            }
        }
        ms[v] = (plat::micros() - t0) / 1000.0 / 64;
    }
    printf("    version 9 %.2f ms, version 10 %.2f ms a chunk; %d candidate starts, %d evaluated, %d pieces placed\n", ms[0],
           ms[1], starts, evaluated, pieces);
    CHECK(pieces > 0);
}

TEST(structures_stronghold_has_its_portal_room) {
    Generator g;
    CHECK(g.init(42, WORLD_NORMAL, GENERATOR_VANILLA_STRONGHOLDS));
    int cx = 0, cz = 0;
    uint64_t t0 = plat::micros();
    bool found = g.locateStructure("stronghold", 0, 0, 4000, cx, cz);
    printf("    stronghold at chunk %d %d (%s, %.0f ms with the biome search)\n", cx, cz, found ? "found" : "none",
           (plat::micros() - t0) / 1000.0);
    CHECK(found);
    if (!found) return;
    // its pieces reach up to 112 blocks from the start: 17 x 17 chunks around it
    int frames = 0, bricks = 0, chests = 0, spawners = 0, books = 0;
    t0 = plat::micros();
    const uint16_t FRAME = (uint16_t)findBlock("end_portal_frame"), SB = (uint16_t)findBlock("stone_bricks"),
                   SP = (uint16_t)findBlock("spawner"), BOOK = (uint16_t)findBlock("bookshelf");
    for (int dz = -8; dz <= 8; dz++)
        for (int dx = -8; dx <= 8; dx++) {
            Chunk c(cx + dx, cz + dz);
            g.generate(c);
            for (int y = -40; y <= 70; y++)
                for (int z = 0; z < 16; z++)
                    for (int x = 0; x < 16; x++) {
                        uint16_t b = blockIdOf(c.get(x, y, z));
                        frames += b == FRAME;
                        bricks += b == SB;
                        spawners += b == SP;
                        books += b == BOOK;
                    }
            chests += tilesOf(c, TILE_CHEST);
        }
    printf("    %d portal frames, %d stone bricks, %d chests, %d spawners, %d bookshelves (%.0f ms for 289 chunks)\n", frames,
           bricks, chests, spawners, books, (plat::micros() - t0) / 1000.0);
    CHECK_EQ(frames, 12);
    CHECK_EQ(spawners, 1);
    CHECK(bricks > 1000);
}

TEST(structures_spawner_keeps_its_mob_in_storage) {
    MemDevice dev(64u << 20);
    StoreParams sp;
    sp.radius = 64;
    {
        WorldStore ws(&dev);
        CHECK(ws.open(sp, true));
        Chunk c(1, 2);
        c.set(3, 20, 4, (uint16_t)BLOCKS[findBlock("spawner")].defState);
        TileEntity* t = c.addTile(TILE_SPAWNER, 3, 20, 4);
        CHECK(t != nullptr);
        t->spawnEntity = (uint16_t)findEntityType("blaze");
        t->spawnDelay = 123;
        CHECK_EQ(c.spawners(), 1);
        CHECK(ws.saveChunk(c));
        CHECK(ws.flush());
    }
    WorldStore ws(&dev);
    CHECK(ws.open(sp, false));
    Chunk c(1, 2);
    CHECK_EQ(ws.loadChunk(c), LOAD_OK);
    const TileEntity* t = c.tileAt(3, 20, 4);
    CHECK(t && t->type == TILE_SPAWNER);
    CHECK(t && t->spawnEntity == (uint16_t)findEntityType("blaze"));
    CHECK(t && t->spawnDelay == 123);
    CHECK_EQ(c.spawners(), 1);
}

TEST(structures_nether_fortress) {
    Generator g;
    CHECK(g.init(42, WORLD_NORMAL, GENERATOR_VANILLA_FORTRESSES, DIM_NETHER));
    int cx = 0, cz = 0;
    bool found = g.locateStructure("fortress", 0, 0, 3000, cx, cz);
    printf("    fortress at chunk %d %d (%s)\n", cx, cz, found ? "found" : "none");
    CHECK(found);
    if (!found) return;
    int bricks = 0, spawners = 0, wart = 0, chests = 0, blazes = 0;
    uint64_t t0 = plat::micros();
    const uint16_t NB = (uint16_t)findBlock("nether_bricks"), SP = (uint16_t)findBlock("spawner"),
                   WART = (uint16_t)findBlock("nether_wart");
    const int blaze = findEntityType("blaze");
    for (int dz = -8; dz <= 8; dz++)
        for (int dx = -8; dx <= 8; dx++) {
            Chunk c(cx + dx, cz + dz, DIM_NETHER);
            g.generate(c);
            for (int y = 0; y < 128; y++)
                for (int z = 0; z < 16; z++)
                    for (int x = 0; x < 16; x++) {
                        uint16_t b = blockIdOf(c.get(x, y, z));
                        bricks += b == NB;
                        spawners += b == SP;
                        wart += b == WART;
                    }
            chests += tilesOf(c, TILE_CHEST);
            for (const TileEntity* t = c.tiles(); t; t = t->next) blazes += t->type == TILE_SPAWNER && t->spawnEntity == blaze;
        }
    printf("    %d nether bricks, %d spawners (%d of blazes), %d nether wart, %d chests (%.0f ms for 289 chunks)\n", bricks,
           spawners, blazes, wart, chests, (plat::micros() - t0) / 1000.0);
    CHECK(bricks > 3000);
    CHECK_EQ(spawners, blazes);
    // the overworld at the same seed has none
    Generator o;
    CHECK(o.init(42, WORLD_NORMAL, GENERATOR_VANILLA_FORTRESSES));
    CHECK(!o.locateStructure("fortress", 0, 0, 3000, cx, cz));
}
