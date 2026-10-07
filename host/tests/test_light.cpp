#include "testing.h"
#include "mc/registry.h"
#include "mc/world/chunk.h"
#include "mc/world/light.h"

using namespace mc;

static int skyAt(const ChunkLight& L, int x, int y, int z) {
    int s = y >> 4;
    if (s >= L.sections()) return 15;
    uint32_t i = ((uint32_t)(y & 15) << 8) | ((uint32_t)z << 4) | (uint32_t)x;
    return (L.sky(s)[i >> 1] >> ((i & 1) * 4)) & 15;
}
static int blockAtL(const ChunkLight& L, int x, int y, int z) {
    int s = y >> 4;
    if (s >= L.sections()) return 0;
    uint32_t i = ((uint32_t)(y & 15) << 8) | ((uint32_t)z << 4) | (uint32_t)x;
    return (L.block(s)[i >> 1] >> ((i & 1) * 4)) & 15;
}

static void floorChunk(Chunk& c) {
    for (int x = 0; x < 16; x++)
        for (int z = 0; z < 16; z++)
            for (int y = 0; y <= 3; y++) c.set(x, y, z, bs::Stone);
}

TEST(light_open_sky_is_full) {
    Chunk c(0, 0);
    floorChunk(c);
    ChunkLight L;
    CHECK(L.compute(c, nullptr));
    CHECK_EQ(L.sections(), 2);
    CHECK_EQ(skyAt(L, 5, 4, 5), 15);
    CHECK_EQ(skyAt(L, 5, 20, 5), 15);
    CHECK_EQ(skyAt(L, 5, 2, 5), 0);   // inside the stone
}

TEST(light_under_a_roof_fades_with_distance) {
    Chunk c(0, 0);
    floorChunk(c);
    for (int x = 4; x <= 11; x++)
        for (int z = 4; z <= 11; z++) c.set(x, 8, z, bs::Stone);
    ChunkLight L;
    CHECK(L.compute(c, nullptr));
    CHECK_EQ(skyAt(L, 3, 5, 8), 15);  // open column next to the roof
    CHECK_EQ(skyAt(L, 4, 5, 8), 14);  // one step under the roof
    CHECK_EQ(skyAt(L, 5, 5, 8), 13);
    CHECK_EQ(skyAt(L, 7, 5, 7), 11);  // 4 steps from the nearest open side
    CHECK_EQ(skyAt(L, 4, 7, 4), 14);  // corner
    CHECK_EQ(skyAt(L, 8, 9, 8), 15);  // on top of the roof
}

TEST(light_torch_in_a_closed_box) {
    Chunk c(0, 0);
    // stone box from 2..12 with air inside 3..11
    for (int x = 2; x <= 12; x++)
        for (int y = 2; y <= 12; y++)
            for (int z = 2; z <= 12; z++) {
                bool shell = x == 2 || x == 12 || y == 2 || y == 12 || z == 2 || z == 12;
                c.set(x, y, z, shell ? bs::Stone : bs::Air);
            }
    c.set(7, 3, 7, bs::Torch);
    ChunkLight L;
    CHECK(L.compute(c, nullptr));
    CHECK_EQ(skyAt(L, 7, 6, 7), 0);       // sealed from the sky
    CHECK_EQ(blockAtL(L, 7, 3, 7), 14);   // torch
    CHECK_EQ(blockAtL(L, 8, 3, 7), 13);
    CHECK_EQ(blockAtL(L, 7, 5, 7), 12);
    CHECK_EQ(blockAtL(L, 11, 3, 11), 6);  // manhattan distance 8
    CHECK_EQ(blockAtL(L, 7, 3, 1), 0);    // outside the box
    CHECK(!L.blockSectionEmpty(0));
}

TEST(light_leaves_and_water_dim_sky_light) {
    Chunk c(0, 0);
    floorChunk(c);
    c.set(6, 10, 6, bs::OakLeaves);
    c.set(9, 4, 9, bs::Water);
    c.set(9, 5, 9, bs::Water);
    ChunkLight L;
    CHECK(L.compute(c, nullptr));
    CHECK(skyAt(L, 6, 9, 6) < 15);        // shaded by leaves
    CHECK(skyAt(L, 6, 9, 6) >= 13);
    CHECK(skyAt(L, 9, 4, 9) < 15);        // under water
    printf("    under leaves %d, 2 deep in water %d (filter: leaves %d, water %d)\n", skyAt(L, 6, 9, 6), skyAt(L, 9, 4, 9),
           BLOCKS[blk::OakLeaves].filterLight, BLOCKS[blk::Water].filterLight);
}
