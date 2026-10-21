// The classic generator's own structures (generator version 100).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "testing.h"
#include "mc/registry.h"
#include "mc/world/chunk.h"
#include "mc/world/generator.h"
#include "mc/world/classic/classic_structures.h"

using namespace mc;

TEST(classic_dungeons_and_ruins) {
    Generator g;
    CHECK(g.init(42, WORLD_NORMAL, GENERATOR_CLASSIC_STRUCTURES));
    int spawners = 0, chests = 0, loot = 0, ruins = 0, crypts = 0;
    const uint16_t SPAWNER = blockIdOf(bs::Spawner), CHEST = blockIdOf(bs::Chest), WEB = blockIdOf(bs::Cobweb),
                   BRICKS = blockIdOf(bs::StoneBricks), SAND = blockIdOf(bs::ChiseledSandstone);
    int shown = 0, sx, sy, sz;
    g.findSpawn(sx, sy, sz);   // on land
    for (int cz = (sz >> 4) - 16; cz < (sz >> 4) + 16; cz++)
        for (int cx = (sx >> 4) - 16; cx < (sx >> 4) + 16; cx++) {
            Chunk c(cx, cz);
            g.generate(c);
            bool ruin = false;
            for (int y = c.minY(); y <= c.maxY(); y++)
                for (int z = 0; z < 16; z++)
                    for (int x = 0; x < 16; x++) {
                        uint16_t b = blockIdOf(c.get(x, y, z));
                        if (b == SPAWNER) {
                            spawners++;
                            if (shown++ < 4) printf("    a dungeon at %d %d %d\n", cx * 16 + x, y, cz * 16 + z);
                        }
                        if (b == CHEST) chests++;
                        if (b == WEB) crypts++;
                        if ((b == BRICKS || b == SAND) && y > SEA_LEVEL) ruin = true;
                    }
            for (const TileEntity* t = c.tiles(); t; t = t->next)
                if (t->type == TILE_CHEST)
                    for (const ItemStack& it : t->items) loot += !it.empty();
            ruins += ruin;
        }
    printf("    1024 chunks: %d spawners, %d chests (%d stacks of loot), %d cobwebs, %d chunks with ruins\n", spawners, chests, loot,
           crypts, ruins);
    CHECK(spawners > 10);
    CHECK(chests > spawners);
    CHECK(loot > chests);
    CHECK(ruins > 5);
}

// MC_RENDER=<dir>: the village's top view (tools/worldgen/render_structure.js)
TEST(classic_villages) {
    Generator g;
    CHECK(g.init(42, WORLD_NORMAL, GENERATOR_CLASSIC_STRUCTURES));
    int cx, cz;
    const char* from = getenv("MC_RENDER_FROM");   // "x,z": the search starts there
    int fx = 0, fz = 0;
    if (from) sscanf(from, "%d,%d", &fx, &fz);
    bool found = g.locateStructure("village", fx, fz, 6000, cx, cz);
    CHECK(found);
    if (!found) return;
    const uint16_t PATH = blockIdOf(bs::DirtPath), SANDPATH = blockIdOf(bs::SmoothSandstone), LANTERN = blockIdOf(bs::Lantern);
    int paths = 0, beds = 0, bells = 0, lamps = 0, villagers = 0, chests = 0;
    const int R = 6, W = (2 * R + 1) * 16;
    std::vector<uint16_t> top(W * W);
    for (int dz = -R; dz <= R; dz++)
        for (int dx = -R; dx <= R; dx++) {
            Chunk c(cx + dx, cz + dz);
            g.generate(c);
            for (int z = 0; z < 16; z++)
                for (int x = 0; x < 16; x++) {
                    int y = c.maxY();
                    while (y > c.minY() && stateIsAir(c.get(x, y, z))) y--;
                    top[((dz + R) * 16 + z) * W + (dx + R) * 16 + x] = c.get(x, y, z);
                    for (int yy = 50; yy < 130; yy++) {
                        uint16_t b = blockIdOf(c.get(x, yy, z));
                        if (b == PATH || b == SANDPATH) paths++;
                        else if (strstr(BLOCKS[b].name, "_bed")) beds++;
                        else if (!strcmp(BLOCKS[b].name, "bell")) bells++;
                        else if (b == LANTERN) lamps++;
                    }
                }
            for (const TileEntity* t = c.tiles(); t; t = t->next) chests += t->type == TILE_CHEST;
            for (int i = 0; i < c.entCount; i++) villagers += c.ents[i].type == (uint16_t)findEntityType("villager");
        }
    printf("    village at chunk %d %d: %d path blocks, %d bed blocks, %d bells, %d lanterns, %d chests, %d villagers\n", cx, cz, paths,
           beds, bells, lamps, chests, villagers);
    CHECK(paths > 100);
    CHECK(beds >= 2);
    CHECK(villagers >= 2);
    const char* dir = getenv("MC_RENDER");
    if (dir) {
        char path[512];
        snprintf(path, sizeof(path), "%s/classic_village.txt", dir);
        FILE* f = fopen(path, "w");
        if (f) {
            fprintf(f, "%d %d\n", W, W);
            for (int i = 0; i < W * W; i++) fprintf(f, "%s\n", blockOf(top[i]).name);
            fclose(f);
        }
    }
}

TEST(classic_strongholds) {
    Generator g;
    CHECK(g.init(42, WORLD_NORMAL, GENERATOR_CLASSIC_STRUCTURES));
    int cx, cz;
    bool found = g.locateStructure("stronghold", 0, 0, 6000, cx, cz);
    CHECK(found);
    if (!found) return;
    int frames = 0, spawners = 0, shelves = 0, bars = 0, ladders = 0, bricks = 0, lowest = 1 << 20;
    const int R = 9;
    for (int dz = -R; dz <= R; dz++)
        for (int dx = -R; dx <= R; dx++) {
            Chunk c(cx + dx, cz + dz);
            g.generate(c);
            for (int y = -60; y < 140; y++)
                for (int z = 0; z < 16; z++)
                    for (int x = 0; x < 16; x++) {
                        const char* n = blockOf(c.get(x, y, z)).name;
                        if (!strcmp(n, "end_portal_frame")) frames++;
                        else if (!strcmp(n, "spawner")) spawners++;
                        else if (!strcmp(n, "bookshelf")) shelves++;
                        else if (!strcmp(n, "iron_bars")) bars++;
                        else if (!strcmp(n, "ladder")) ladders++;
                        else if (!strcmp(n, "stone_bricks")) {
                            bricks++;
                            if (y < lowest) lowest = y;
                        }
                    }
        }
    printf("    stronghold at chunk %d %d: %d portal frames, %d spawners, %d bookshelves, %d iron bars, %d ladders, %d stone bricks "
           "(lowest y %d)\n",
           cx, cz, frames, spawners, shelves, bars, ladders, bricks, lowest);
    CHECK_EQ(frames, 12);
    CHECK(spawners >= 1);
    CHECK(ladders >= 3);
    CHECK(bricks > 1000);
}
