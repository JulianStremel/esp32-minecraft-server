#include "testing.h"
#include "mc/registry.h"
#include "mc/server/path.h"

using namespace mc;

// 3 x 3 chunks around (0, 0) with a stone floor at y = 63 (stand at y = 64).
struct Area {
    Chunk* c[9];
    Area() {
        for (int k = 0; k < 9; k++) {
            c[k] = new Chunk(k % 3 - 1, k / 3 - 1);
            for (int x = 0; x < 16; x++)
                for (int z = 0; z < 16; z++) c[k]->set(x, 63, z, bs::Stone);
        }
    }
    ~Area() {
        for (Chunk* p : c) delete p;
    }
    void set(int x, int y, int z, uint16_t st) { c[((z + 16) >> 4) * 3 + ((x + 16) >> 4)]->set((x + 16) & 15, y, (z + 16) & 15, st); }
    PathRequest request(PathPoint from, PathPoint to) const {
        PathRequest r;
        for (int k = 0; k < 9; k++) r.nine[k] = c[k];
        r.start = from;
        r.goal = to;
        return r;
    }
};

TEST(path_straight_on_flat_ground) {
    Area a;
    PathResult res;
    CHECK(findPath(a.request({0, 0, 64}, {10, 0, 64}), res));
    CHECK(res.reached);
    CHECK(res.n >= 9 && res.n <= 10);
}

TEST(path_goes_through_the_gap_in_a_wall) {
    Area a;
    // a wall at x = 5 from z = -10 to 10, 3 high, with a gap at z = 8
    for (int z = -10; z <= 10; z++)
        for (int y = 64; y <= 66; y++)
            if (z != 8) a.set(5, y, z, bs::Stone);
    PathResult res;
    CHECK(findPath(a.request({0, 0, 64}, {10, 0, 64}), res));
    CHECK(res.reached);
    bool viaGap = false;
    for (int i = 0; i < res.n; i++)
        if (res.points[i].x == 5) viaGap = res.points[i].z == 8;
    CHECK(viaGap);
    printf("    %d waypoints, %d nodes\n", res.n, res.nodes);
}

TEST(path_avoids_lava_and_deep_drops) {
    Area a;
    // a lava moat across x = 5 except a bridge at z = -6
    for (int z = -16; z < 32; z++) {
        a.set(5, 63, z, z == -6 ? bs::Stone : bs::Lava);
    }
    PathResult res;
    CHECK(findPath(a.request({0, 0, 64}, {10, 0, 64}), res));
    CHECK(res.reached);
    for (int i = 0; i < res.n; i++)
        if (res.points[i].x == 5) CHECK_EQ(res.points[i].z, -6);
    // a pit 5 deep is not walked into: the goal inside it is not reached
    Area b;
    for (int x = 4; x <= 6; x++)
        for (int z = -1; z <= 1; z++)
            for (int y = 59; y <= 63; y++) b.set(x, y, z, bs::Air);
    b.set(5, 58, 0, bs::Stone);
    PathResult pit;
    CHECK(findPath(b.request({0, 0, 64}, {5, 0, 59}), pit));
    CHECK(!pit.reached);
}

TEST(path_steps_up_one_block_but_not_two) {
    Area a;
    for (int z = -16; z < 32; z++) a.set(5, 64, z, bs::Stone);   // a step one block high
    PathResult res;
    CHECK(findPath(a.request({0, 0, 64}, {10, 0, 65}), res));
    CHECK(res.reached);
    Area b;
    for (int z = -16; z < 32; z++)
        for (int y = 64; y <= 65; y++) b.set(5, y, z, bs::Stone);   // two high: a wall
    PathResult wall;
    CHECK(findPath(b.request({0, 0, 64}, {10, 0, 64}), wall));
    CHECK(!wall.reached);
}
