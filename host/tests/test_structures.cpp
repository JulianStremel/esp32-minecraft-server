// Generator version 10: vanilla's structures (mc/world/vanilla/structures.cpp): where
// they stand, that each is whole across its chunks in whatever order they are generated,
// and their chests.
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "testing.h"
#include "mc/platform.h"
#include "mc/registry.h"
#include "mc/world/chunk.h"
#include "mc/world/generator.h"
#include "mc/world/vanilla/java_random.h"
#include "mc/world/vanilla/density.h"
#include "mc/world/vanilla_probes.h"
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

TEST(structures_mineshaft) {
    Generator g;
    CHECK(g.init(42, WORLD_NORMAL, GENERATOR_VANILLA_MINESHAFTS));
    int cx = 0, cz = 0;
    uint64_t t0 = plat::micros();
    bool found = g.locateStructure("mineshaft", 0, 0, 1000, cx, cz);
    printf("    mineshaft at chunk %d %d (%s, %.0f ms)\n", cx, cz, found ? "found" : "none", (plat::micros() - t0) / 1000.0);
    CHECK(found);
    if (!found) return;
    int rails = 0, fences = 0, webs = 0, carts = 0, spawners = 0;
    const uint16_t RAIL = (uint16_t)findBlock("rail"), FENCE = (uint16_t)findBlock("oak_fence"),
                   WEB = (uint16_t)findBlock("cobweb"), SP = (uint16_t)findBlock("spawner");
    t0 = plat::micros();
    for (int dz = -6; dz <= 6; dz++)
        for (int dx = -6; dx <= 6; dx++) {
            Chunk c(cx + dx, cz + dz);
            g.generate(c);
            for (int y = -64; y <= 80; y++)
                for (int z = 0; z < 16; z++)
                    for (int x = 0; x < 16; x++) {
                        uint16_t b = blockIdOf(c.get(x, y, z));
                        rails += b == RAIL;
                        fences += b == FENCE;
                        webs += b == WEB;
                        spawners += b == SP;
                    }
            carts += c.entCount;
        }
    printf("    %d rails, %d oak fences, %d cobwebs, %d spawners, %d chest minecarts (%.0f ms for 169 chunks)\n", rails, fences,
           webs, spawners, carts, (plat::micros() - t0) / 1000.0);
    CHECK(fences > 20);
    CHECK(rails > 10);
}

TEST(structures_villages_and_outposts) {
    struct Want { const char* name; const char* block; };
    const Want wants[] = {{"pillager_outpost", "dark_oak_log"}, {"village_plains", "bell"}, {"village_desert", "bell"},
                          {"village_savanna", "bell"}, {"village_taiga", "bell"}, {"village_snowy", "bell"}};
    for (const Want& w : wants) {
        bool ok = false;
        for (uint64_t seed = 1; seed <= 8 && !ok; seed++) {
            Generator g;
            CHECK(g.init(seed, WORLD_NORMAL, GENERATOR_VANILLA_VILLAGES));
            int cx, cz;
            uint64_t t0 = plat::micros();
            if (!g.locateStructure(w.name, 0, 0, 4000, cx, cz)) continue;
            uint32_t locUs = (uint32_t)(plat::micros() - t0);
            int n = 0, blocks = 0, chests = 0, mobs = 0;
            const uint16_t B = (uint16_t)findBlock(w.block);
            t0 = plat::micros();
            for (int dz = -5; dz <= 5; dz++)
                for (int dx = -5; dx <= 5; dx++) {
                    Chunk c(cx + dx, cz + dz);
                    g.generate(c);
                    n += countBlock(c, B);
                    blocks += vanilla::structureStats.blocks;
                    chests += tilesOf(c, TILE_CHEST) + tilesOf(c, TILE_BARREL);
                    mobs += c.entCount;
                }
            printf("    %s: seed %llu chunk %d %d (located in %u ms), %d %s, %d structure blocks, %d containers, %d mobs (%.0f ms for 121 chunks)\n",
                   w.name, (unsigned long long)seed, cx, cz, locUs / 1000, n, w.block, blocks, chests, mobs, (plat::micros() - t0) / 1000.0);
            CHECK(n > 0);
            ok = true;
        }
        CHECK(ok);
    }
}

// MC_RENDER=<dir>: a plains village's (MC_RENDER_KIND: another structure's, MC_RENDER_VERSION: the
// generator's) top view and vertical cuts as block names, for
// tools/worldgen/render_structure.js (how the houses sit on the terrain)
TEST(structures_render_village) {
    const char* dir = getenv("MC_RENDER");
    if (!dir) return;
    const char* vEnv = getenv("MC_RENDER_VERSION");
    int version = vEnv ? atoi(vEnv) : GENERATOR_VANILLA_VILLAGES;
    for (int v = version; v <= version; v++) {
        Generator g;
        const char* seedEnv = getenv("MC_RENDER_SEED");
        const char* kind = getenv("MC_RENDER_KIND");
        CHECK(g.init(seedEnv ? strtoull(seedEnv, nullptr, 10) : 1, WORLD_NORMAL, v));
        int cx, cz;
        if (!g.locateStructure(kind ? kind : "village_plains", 0, 0, 4000, cx, cz)) return;
        const int R = 4, W = (2 * R + 1) * 16;
        std::vector<uint16_t> top(W * W), cut(W * 64 * 5);
        for (int dz = -R; dz <= R; dz++)
            for (int dx = -R; dx <= R; dx++) {
                Chunk c(cx + dx, cz + dz);
                g.generate(c);
                for (int z = 0; z < 16; z++)
                    for (int x = 0; x < 16; x++) {
                        int px = (dx + R) * 16 + x, pz = (dz + R) * 16 + z, y = c.maxY();
                        while (y > c.minY() && stateIsAir(c.get(x, y, z))) y--;
                        top[pz * W + px] = c.get(x, y, z);
                        for (int k = 0; k < 5; k++)
                            if (pz == W / 2 - 24 + k * 12)
                                for (int yy = 0; yy < 64; yy++) cut[(k * 64 + yy) * W + px] = c.get(x, 50 + yy, z);
                    }
            }
        char path[512];
        snprintf(path, sizeof(path), "%s/village_v%d.txt", dir, v);
        FILE* f = fopen(path, "w");
        if (!f) return;
        fprintf(f, "%d %d\n", W, W);
        for (int i = 0; i < W * W; i++) fprintf(f, "%s\n", blockOf(top[i]).name);
        fprintf(f, "%d %d\n", W, 64 * 5);
        for (int i = 0; i < W * 64 * 5; i++) fprintf(f, "%s\n", blockOf(cut[i]).name);
        fclose(f);
    }
}

TEST(structures_beard_cost) {
    // the village's start chunk with and without the structures shaping the terrain
    Generator a, b;
    CHECK(a.init(42, WORLD_NORMAL, GENERATOR_VANILLA_MINESHAFTS));
    CHECK(b.init(42, WORLD_NORMAL, GENERATOR_VANILLA_VILLAGES));
    int cx, cz;
    CHECK(b.locateStructure("village_plains", 0, 0, 3000, cx, cz));
    for (int k = 0; k < 2; k++) {
        Chunk c1(cx, cz), c2(cx, cz);
        a.generate(c1);
        uint32_t s1 = a.vanillaSteps.shape;
        b.generate(c2);
        uint32_t s2 = b.vanillaSteps.shape;
        if (k) printf("    the village's start chunk %d %d: terrain %u us without structures, %u us with\n", cx, cz, (unsigned)s1, (unsigned)s2);
    }
}

TEST(structures_bastions_ruins_cities_chambers) {
    struct Want { const char* name; uint8_t dim; const char* block; int radius; };
    const Want wants[] = {{"bastion_remnant", DIM_NETHER, "polished_blackstone_bricks", 3000},
                          {"trail_ruins", DIM_OVERWORLD, "mud_bricks", 6000},
                          {"ancient_city", DIM_OVERWORLD, "reinforced_deepslate", 6000},
                          {"trial_chambers", DIM_OVERWORLD, "trial_spawner", 6000}};
    for (const Want& w : wants) {
        bool ok = false;
        for (uint64_t seed = 1; seed <= 6 && !ok; seed++) {
            Generator g;
            CHECK(g.init(seed, WORLD_NORMAL, GENERATOR_VANILLA_JIGSAWS, w.dim));
            int cx, cz;
            uint64_t t0 = plat::micros();
            if (!g.locateStructure(w.name, 0, 0, w.radius, cx, cz)) continue;
            uint32_t locMs = (uint32_t)((plat::micros() - t0) / 1000);
            int n = 0, chests = 0;
            const uint16_t B = (uint16_t)findBlock(w.block);
            t0 = plat::micros();
            for (int dz = -6; dz <= 6; dz++)
                for (int dx = -6; dx <= 6; dx++) {
                    Chunk c(cx + dx, cz + dz, w.dim);
                    g.generate(c);
                    n += countBlock(c, B);
                    chests += tilesOf(c, TILE_CHEST) + tilesOf(c, TILE_BARREL);
                }
            printf("    %s: seed %llu chunk %d %d (located in %u ms), %d %s, %d containers (%.0f ms for 169 chunks)\n", w.name,
                   (unsigned long long)seed, cx, cz, locMs, n, w.block, chests, (plat::micros() - t0) / 1000.0);
            CHECK(n > 0);
            ok = true;
        }
        CHECK(ok);
    }
}

TEST(structures_igloos_shipwrecks_ruins_fossils) {
    struct Want { const char* name; uint8_t dim; const char* block; };
    const Want wants[] = {{"igloo", DIM_OVERWORLD, "snow_block"}, {"shipwreck", DIM_OVERWORLD, "oak_trapdoor"},
                          {"shipwreck_beached", DIM_OVERWORLD, "oak_trapdoor"}, {"ocean_ruin_cold", DIM_OVERWORLD, "stone_bricks"},
                          {"ocean_ruin_warm", DIM_OVERWORLD, "sandstone"}, {"nether_fossil", DIM_NETHER, "bone_block"}};
    for (const Want& w : wants) {
        bool ok = false;
        for (uint64_t seed = 1; seed <= 6 && !ok; seed++) {
            Generator g;
            CHECK(g.init(seed, WORLD_NORMAL, GENERATOR_VANILLA_TEMPLATES, w.dim));
            int cx, cz;
            if (!g.locateStructure(w.name, 0, 0, 4000, cx, cz)) continue;
            int n = 0, chests = 0, mobs = 0;
            const uint16_t B = (uint16_t)findBlock(w.block);
            for (int dz = -3; dz <= 3; dz++)
                for (int dx = -3; dx <= 3; dx++) {
                    Chunk c(cx + dx, cz + dz, w.dim);
                    g.generate(c);
                    n += countBlock(c, B);
                    chests += lootItems(c, TILE_CHEST) > 0 ? tilesOf(c, TILE_CHEST) : 0;
                    mobs += c.entCount;
                }
            printf("    %s: seed %llu chunk %d %d, %d %s, %d chests with loot, %d mobs\n", w.name, (unsigned long long)seed, cx, cz, n,
                   w.block, chests, mobs);
            CHECK(n > 0);
            ok = true;
        }
        CHECK(ok);
    }
}


TEST(structures_ruined_portals) {
    const char* const names[] = {"ruined_portal", "ruined_portal_desert", "ruined_portal_jungle", "ruined_portal_swamp",
                                 "ruined_portal_mountain", "ruined_portal_ocean", "ruined_portal_nether"};
    const uint16_t OBS = (uint16_t)findBlock("obsidian"), CRY = (uint16_t)findBlock("crying_obsidian"),
                   NR = (uint16_t)findBlock("netherrack"), BS = (uint16_t)findBlock("polished_blackstone_bricks");
    for (const char* name : names) {
        bool nether = !strcmp(name, "ruined_portal_nether");
        bool ok = false;
        for (uint64_t seed = 1; seed <= 8 && !ok; seed++) {
            Generator g;
            CHECK(g.init(seed, WORLD_NORMAL, GENERATOR_VANILLA_PORTALS, nether ? DIM_NETHER : DIM_OVERWORLD));
            int cx, cz;
            if (!g.locateStructure(name, 0, 0, 6000, cx, cz)) continue;
            int obs = 0, nr = 0, bs = 0, chests = 0;
            for (int dz = -2; dz <= 2; dz++)
                for (int dx = -2; dx <= 2; dx++) {
                    Chunk c(cx + dx, cz + dz, nether ? DIM_NETHER : DIM_OVERWORLD);
                    g.generate(c);
                    obs += countBlock(c, OBS) + countBlock(c, CRY);
                    nr += countBlock(c, NR);
                    bs += countBlock(c, BS);
                    chests += tilesOf(c, TILE_CHEST);
                }
            printf("    %s: seed %llu chunk %d %d, %d obsidian, %d netherrack, %d blackstone bricks, %d chests\n", name,
                   (unsigned long long)seed, cx, cz, obs, nr, bs, chests);
            CHECK(obs > 0);
            ok = true;
        }
        CHECK(ok);
    }
}

TEST(structures_end_cities) {
    bool ok = false;
    for (uint64_t seed = 1; seed <= 6 && !ok; seed++) {
        Generator g;
        CHECK(g.init(seed, WORLD_NORMAL, GENERATOR_VANILLA_END_CITIES, DIM_END));
        int cx, cz;
        uint64_t t0 = plat::micros();
        if (!g.locateStructure("end_city", 1500, 0, 3000, cx, cz)) continue;
        uint32_t locMs = (uint32_t)((plat::micros() - t0) / 1000);
        const uint16_t PUR = (uint16_t)findBlock("purpur_block"), ROD = (uint16_t)findBlock("end_rod");
        int purpur = 0, rods = 0, chests = 0, shulkers = 0;
        t0 = plat::micros();
        for (int dz = -7; dz <= 7; dz++)
            for (int dx = -7; dx <= 7; dx++) {
                Chunk c(cx + dx, cz + dz, DIM_END);
                g.generate(c);
                purpur += countBlock(c, PUR);
                rods += countBlock(c, ROD);
                chests += lootItems(c, TILE_CHEST) > 0 ? tilesOf(c, TILE_CHEST) : 0;
                shulkers += c.entCount;
            }
        printf("    end city: seed %llu chunk %d %d (located in %u ms), %d purpur blocks, %d end rods, %d chests with loot, %d shulkers "
               "(%.0f ms for 225 chunks)\n",
               (unsigned long long)seed, cx, cz, locMs, purpur, rods, chests, shulkers, (plat::micros() - t0) / 1000.0);
        CHECK(purpur > 200);
        ok = true;
    }
    CHECK(ok);
    // the fingerprint's search (generatorDimFingerprint) finds one for its seeds
    for (uint64_t seed : {42ull, 1ull}) {
        Generator g;
        CHECK(g.init(seed, WORLD_NORMAL, GENERATOR_VANILLA_END_CITIES, DIM_END));
        int cx, cz;
        CHECK(g.locateStructure("end_city", 0, 0, 3000, cx, cz));
    }
}

TEST(structures_woodland_mansions) {
    Generator g;
    CHECK(g.init(42, WORLD_NORMAL, GENERATOR_VANILLA_MANSIONS));
    int cx, cz;
    uint64_t t0 = plat::micros();
    bool found = g.locateStructure("mansion", 0, 0, 12000, cx, cz);
    uint32_t locMs = (uint32_t)((plat::micros() - t0) / 1000);
    CHECK(found);
    if (!found) return;
    const uint16_t DARK = (uint16_t)findBlock("dark_oak_planks"), COBBLE = (uint16_t)findBlock("cobblestone");
    int planks = 0, cobble = 0, chests = 0, mobs = 0;
    t0 = plat::micros();
    for (int dz = -6; dz <= 6; dz++)
        for (int dx = -6; dx <= 6; dx++) {
            Chunk c(cx + dx, cz + dz);
            g.generate(c);
            planks += countBlock(c, DARK);
            cobble += countBlock(c, COBBLE);
            chests += lootItems(c, TILE_CHEST) > 0 ? tilesOf(c, TILE_CHEST) : 0;
            mobs += c.entCount;
        }
    printf("    mansion: seed 42 chunk %d %d (located in %u ms), %d dark oak planks, %d cobblestone, %d chests with loot, %d mobs "
           "(%.0f ms for 169 chunks)\n",
           cx, cz, locMs, planks, cobble, chests, mobs, (plat::micros() - t0) / 1000.0);
    CHECK(planks > 1000);
    CHECK(chests >= 1);
    CHECK(mobs >= 2);
    // the fingerprint's search (generatorFingerprint) finds one for seed 1 too
    Generator g1;
    CHECK(g1.init(1, WORLD_NORMAL, GENERATOR_VANILLA_MANSIONS));
    CHECK(g1.locateStructure("mansion", 0, 0, 8000, cx, cz));
}
