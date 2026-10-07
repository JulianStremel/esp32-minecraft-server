// Server-level tests without clients: block updates, fluids, gravity.
#include <unistd.h>
#include "testing.h"
#include "mc/registry.h"
#include "mc/server/server.h"

using namespace mc;

static Server* makeServer() {
    static Server* s = nullptr;
    if (s) return s;
    s = new Server();
    ServerConfig cfg;
    cfg.port = 0;  // ephemeral
    cfg.worldType = WORLD_FLAT;
    cfg.spawnMobs = false;
    cfg.seed = 1;
    if (!s->begin(cfg, nullptr)) return nullptr;
    // keep the area around the origin resident
    for (int cx = -2; cx <= 2; cx++)
        for (int cz = -2; cz <= 2; cz++) s->world.load(cx, cz);
    return s;
}

static void runTicks(Server& s, int n) {
    uint32_t target = s.ticks + (uint32_t)n;
    while (s.ticks < target) {
        s.loop();
        usleep(2000);
    }
}

static void dumpRow(Server& s, int y, int z, int x0, int x1) {
    printf("    y=%d z=%d:", y, z);
    for (int x = x0; x <= x1; x++) {
        uint16_t st = s.blockAt(x, y, z);
        if (blockIdOf(st) == blk::Water) printf(" %d", getProp(st, "level"));
        else printf(" %c", stateIsAir(st) ? '.' : '#');
    }
    printf("\n");
}

TEST(water_spreads_and_drains) {
    Server* s = makeServer();
    CHECK(s != nullptr);
    if (!s) return;
    s->setBlock(6, 4, 6, bs::Water);
    runTicks(*s, 80);
    dumpRow(*s, 4, 6, 4, 15);
    CHECK_EQ(blockIdOf(s->blockAt(7, 4, 6)), blk::Water);
    CHECK_EQ(getProp(s->blockAt(7, 4, 6), "level"), 1);
    CHECK_EQ(getProp(s->blockAt(13, 4, 6), "level"), 7);
    CHECK(stateIsAir(s->blockAt(14, 4, 6)));
    s->setBlock(6, 4, 6, 0);
    for (int i = 0; i < 4; i++) {
        runTicks(*s, 5);
        if (getenv("FLUID_DEBUG")) { printf("   t+%d\n", (i + 1) * 5); for (int z = 0; z <= 12; z++) dumpRow(*s, 4, z, 0, 13); }
    }
    runTicks(*s, 200);
    if (getenv("FLUID_DEBUG")) {
        printf("   final\n");
        for (int z = 0; z <= 12; z++) dumpRow(*s, 4, z, 0, 13);
        for (int y = 2; y <= 6; y++)
            for (int x = -16; x < 32; x++)
                for (int z = -16; z < 32; z++) {
                    uint16_t st = s->blockAt(x, y, z);
                    if (blockIdOf(st) == blk::Water && (y != 4 || getProp(st, "level") == 0 || getProp(st, "level") >= 8))
                        printf("    odd water at %d %d %d level %d\n", x, y, z, getProp(st, "level"));
                    if (y == 3 && blockIdOf(st) != blk::GrassBlock) printf("    non-grass floor at %d %d %d: %s\n", x, y, z, blockOf(st).name);
                }
    }
    bool any = false;
    for (int x = 0; x < 16; x++)
        for (int z = 0; z < 16; z++)
            if (blockIdOf(s->blockAt(x, 4, z)) == blk::Water) any = true;
    CHECK(!any);
}

TEST(sand_falls_and_lands) {
    Server* s = makeServer();
    if (!s) return;
    s->setBlock(-5, 10, -5, bs::Sand);
    runTicks(*s, 40);
    CHECK(stateIsAir(s->blockAt(-5, 10, -5)));
    CHECK_EQ(s->blockAt(-5, 4, -5), bs::Sand);
}

TEST(plants_break_without_support) {
    Server* s = makeServer();
    if (!s) return;
    s->setBlock(3, 4, -3, bs::Poppy);
    CHECK_EQ(s->blockAt(3, 4, -3), bs::Poppy);
    s->setBlock(3, 3, -3, 0);   // remove the grass below
    CHECK(stateIsAir(s->blockAt(3, 4, -3)));
}
