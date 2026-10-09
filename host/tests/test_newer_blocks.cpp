// Blocks of 1.17 to 1.21 (newer_blocks.cpp): copper oxidation and amethyst growth by
// random ticks, amethyst drops and support.
#include <memory>
#include "testing.h"
#include "mc/registry.h"
#include "mc/server/server.h"

using namespace mc;

namespace {
struct TestWorld {
    std::unique_ptr<Server> s{new Server};
    TestWorld() {
        ServerConfig cfg;
        cfg.port = 0;
        cfg.worldType = WORLD_FLAT;
        cfg.spawnMobs = false;
        cfg.seed = 1;
        cfg.workerThreads = 0;
        CHECK(s->begin(cfg, nullptr));
    }
    uint16_t state(const char* name) {
        int b = findBlock(name);
        CHECK(b >= 0);
        return b >= 0 ? BLOCKS[b].defState : 0;
    }
    const char* nameAt(int x, int y, int z) { return BLOCKS[blockIdOf(s->blockAt(x, y, z))].name; }
};
}  // namespace

TEST(copper_oxidizes_by_random_ticks_one_stage_at_a_time) {
    TestWorld w;
    w.s->setBlock(0, 40, 0, w.state("cut_copper_stairs"));
    w.s->setBlock(0, 40, 0, setPropStr(w.s->blockAt(0, 40, 0), "facing", "east"));
    int ticks = 0;
    while (strcmp(w.nameAt(0, 40, 0), "exposed_cut_copper_stairs") && ticks < 20000) {
        w.s->randomTickNewerBlock(0, 40, 0, w.s->blockAt(0, 40, 0));
        ticks++;
    }
    CHECK_STR(w.nameAt(0, 40, 0), "exposed_cut_copper_stairs");
    CHECK_STR(getPropStr(w.s->blockAt(0, 40, 0), "facing"), "east");   // the properties carry over
    // the pace: a lone block changes with chance 0.0569 * 0.75 per random tick (vanilla), so
    // after 23.4 ticks on average
    long total = 0;
    const int N = 300;
    for (int k = 0; k < N; k++) {
        w.s->setBlock(10, 40, 10, w.state("copper_block"));
        int t = 0;
        while (blockIdOf(w.s->blockAt(10, 40, 10)) == blk::CopperBlock && t < 5000) {
            w.s->randomTickNewerBlock(10, 40, 10, w.s->blockAt(10, 40, 10));
            t++;
        }
        total += t;
    }
    double mean = (double)total / N;
    printf("    a lone copper block oxidizes after %.1f random ticks on average (vanilla 23.4)\n", mean);
    CHECK(mean > 18 && mean < 30);
    for (int k = 0; k < 20000; k++) w.s->randomTickNewerBlock(0, 40, 0, w.s->blockAt(0, 40, 0));
    CHECK_STR(w.nameAt(0, 40, 0), "oxidized_cut_copper_stairs");   // and stops at the last stage
}

TEST(copper_waits_for_less_oxidized_neighbours_and_wax_stops_it) {
    TestWorld w;
    w.s->setBlock(0, 40, 0, w.state("weathered_copper"));
    w.s->setBlock(3, 40, 0, w.state("copper_block"));   // less oxidized, 3 blocks away
    w.s->setBlock(0, 40, 5, w.state("waxed_copper_block"));
    for (int k = 0; k < 5000; k++) {
        w.s->randomTickNewerBlock(0, 40, 0, w.s->blockAt(0, 40, 0));
        w.s->randomTickNewerBlock(0, 40, 5, w.s->blockAt(0, 40, 5));
    }
    CHECK_STR(w.nameAt(0, 40, 0), "weathered_copper");
    CHECK_STR(w.nameAt(0, 40, 5), "waxed_copper_block");
    CHECK(newerRandomTicking(blk::CopperBlock));
    CHECK(!newerRandomTicking(blk::WaxedCopperBlock));
    CHECK(!newerRandomTicking(blk::OxidizedCopper));
}

TEST(budding_amethyst_grows_buds_into_clusters) {
    TestWorld w;
    for (int x = -1; x <= 1; x++)
        for (int y = 39; y <= 41; y++)
            for (int z = -1; z <= 1; z++) w.s->setBlock(x, y, z, bs::Stone);
    w.s->setBlock(0, 40, 0, bs::BuddingAmethyst);
    w.s->setBlock(0, 41, 0, 0);   // one free face: up
    for (int k = 0; k < 2000; k++) w.s->randomTickNewerBlock(0, 40, 0, w.s->blockAt(0, 40, 0));
    CHECK_STR(w.nameAt(0, 41, 0), "amethyst_cluster");
    CHECK_STR(getPropStr(w.s->blockAt(0, 41, 0), "facing"), "up");
    // without the block it grew from, the cluster falls off; it gives four shards to a pickaxe
    // only (here: no player, two)
    w.s->breakBlock(0, 40, 0, nullptr, true);
    w.s->updateNeighbors(0, 40, 0);
    CHECK(stateIsAir(w.s->blockAt(0, 41, 0)));
}
