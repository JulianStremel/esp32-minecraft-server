// Rails: shapes from the neighbours (RailState), slopes, the junction that redstone
// switches, power along powered rails, support.
#include <memory>
#include "testing.h"
#include "mc/registry.h"
#include "mc/server/rails.h"
#include "mc/server/server.h"

using namespace mc;

namespace {
struct RailWorld {
    std::unique_ptr<Server> s{new Server};
    RailWorld() {
        ServerConfig cfg;
        cfg.port = 0;
        cfg.worldType = WORLD_FLAT;
        cfg.spawnMobs = false;
        cfg.seed = 1;
        cfg.workerThreads = 0;
        CHECK(s->begin(cfg, nullptr));
        for (int z = -6; z <= 6; ++z)
            for (int x = -6; x <= 20; ++x) {
                s->world.setBlock(0, x, 79, z, bs::Stone, false);
                for (int y = 80; y <= 83; y++) s->world.setBlock(0, x, y, z, bs::Air, false);
            }
    }
    void rail(int x, int z, uint16_t st = bs::Rail, int y = 80) { s->setBlock(x, y, z, st); }
    const char* shape(int x, int z, int y = 80) {
        uint16_t st = s->blockAt(x, y, z);
        return rails::isRail(st) ? getPropStr(st, "shape") : "none";
    }
};
}  // namespace

TEST(rails_join_in_lines_and_curves) {
    RailWorld w;
    w.rail(0, 0);
    w.rail(1, 0);
    w.rail(2, 0);
    CHECK_STR(w.shape(0, 0), "east_west");
    CHECK_STR(w.shape(1, 0), "east_west");
    CHECK_STR(w.shape(2, 0), "east_west");
    // a rail south of the end: the end turns to it
    w.rail(2, 1);
    CHECK_STR(w.shape(2, 0), "south_west");
    CHECK_STR(w.shape(2, 1), "north_south");
    // a rail already joined at both ends keeps its shape
    w.rail(1, -1);
    CHECK_STR(w.shape(1, 0), "east_west");
    CHECK_STR(w.shape(1, -1), "north_south");
    // powered rails do not curve
    w.rail(5, 0, bs::PoweredRail);
    w.rail(6, 0, bs::PoweredRail);
    w.rail(6, 1, bs::PoweredRail);
    CHECK_STR(w.shape(6, 0), "east_west");
}

TEST(rails_slope_up_to_a_rail_one_block_higher) {
    RailWorld w;
    w.s->setBlock(1, 80, 0, bs::Stone);
    w.rail(1, 0, bs::Rail, 81);
    w.rail(0, 0);
    CHECK_STR(w.shape(0, 0), "ascending_east");
    // without the block it rises into, a slope breaks
    w.s->setBlock(1, 81, 0, bs::Air);
    w.s->setBlock(1, 80, 0, bs::Air);
    CHECK_STR(w.shape(0, 0), "none");
}

TEST(rail_junction_switches_with_redstone) {
    RailWorld w;
    w.rail(-1, 0);
    w.rail(1, 0);
    w.rail(0, 1);
    w.rail(0, 0);   // west, east and south: unpowered it turns south-east
    CHECK_STR(w.shape(0, 0), "south_east");
    w.s->setBlock(0, 79, 0, bs::RedstoneBlock);   // powered from below: south-west
    CHECK_STR(w.shape(0, 0), "south_west");
    w.s->setBlock(0, 79, 0, bs::Stone);
    CHECK_STR(w.shape(0, 0), "south_east");
}

TEST(powered_rails_pass_power_along_eight_rails) {
    RailWorld w;
    for (int x = 0; x <= 10; x++) w.rail(x, 0, bs::PoweredRail);
    for (int x = 0; x <= 10; x++) CHECK(!getBool(w.s->blockAt(x, 80, 0), "powered"));
    w.s->setBlock(0, 80, -1, bs::RedstoneBlock);   // beside the first
    int on = 0;
    for (int x = 0; x <= 10; x++) on += getBool(w.s->blockAt(x, 80, 0), "powered");
    printf("    %d of 11 powered rails on\n", on);
    CHECK_EQ(on, 9);   // the powered one and 8 more
    CHECK(!getBool(w.s->blockAt(9, 80, 0), "powered"));
    w.s->setBlock(0, 80, -1, bs::Air);
    on = 0;
    for (int x = 0; x <= 10; x++) on += getBool(w.s->blockAt(x, 80, 0), "powered");
    CHECK_EQ(on, 0);
}

TEST(rails_need_a_block_below) {
    RailWorld w;
    w.rail(3, 3);
    CHECK_STR(w.shape(3, 3), "north_south");
    w.s->setBlock(3, 79, 3, bs::Air);
    CHECK_STR(w.shape(3, 3), "none");
}
