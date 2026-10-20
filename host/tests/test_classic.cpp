// The classic generator's own structures (generator version 100).
#include <stdio.h>
#include <string.h>
#include "testing.h"
#include "mc/registry.h"
#include "mc/world/chunk.h"
#include "mc/world/generator.h"

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
#include "mc/world/vanilla/templates.h"
TEST(zz_houses) {
    using namespace mc::vanilla;
    int shown = 0;
    for (int i = 0; i < TD_NUM_TEMPLATES && shown < 40; i++) {
        const char* n = TD_STRINGS[TD_TEMPLATES[i].name];
        if (!strstr(n, "village/plains/houses/") && !strstr(n, "village/desert/houses/") && !strstr(n, "town_centers")) continue;
        if (strstr(n, "zombie")) continue;
        const TdTemplate& t = TD_TEMPLATES[i];
        printf("    %s %dx%dx%d jigsaws %d:", n, t.sx, t.sy, t.sz, t.jigCount);
        for (int k = 0; k < t.jigCount && k < 4; k++) {
            const TdJigsaw& j = TD_JIGSAWS[t.jigFirst + k];
            printf(" [%d %d %d o%d %s->%s]", j.x, j.y, j.z, j.orient, TD_STRINGS[j.name], TD_STRINGS[j.target]);
        }
        printf("\n");
        shown++;
    }
}
