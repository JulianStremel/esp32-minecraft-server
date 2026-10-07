// Deterministic, seeded terrain generator. Everything is a pure function of
// (seed, x, z), so chunks that were never modified do not need to be stored:
// they are simply regenerated when needed. Trees that cross chunk borders are
// derived from neighbour-chunk tree sites, again without loading neighbours.
#pragma once
#include <stdint.h>
#include "mc/world/chunk.h"
#include "mc/world/noise.h"

namespace mc {

enum WorldType : uint8_t { WORLD_NORMAL = 0, WORLD_FLAT = 1, WORLD_VOID = 2 };

struct ColumnInfo {
    int16_t height;   // y of the topmost terrain block (before decoration)
    uint8_t biome;
    bool river;
};

class Generator {
public:
    void init(uint64_t seed, WorldType type);
    uint64_t seed() const { return seed_; }
    WorldType type() const { return type_; }

    void generate(Chunk& c) const;
    ColumnInfo column(int x, int z) const;
    // Finds a dry land position near the origin. y is the first air block above ground.
    void findSpawn(int& x, int& y, int& z) const;

private:
    struct TreeSite { int x, z, y; uint8_t kind; };
    enum TreeKind : uint8_t { TREE_OAK, TREE_BIRCH, TREE_SPRUCE, TREE_JUNGLE, TREE_ACACIA, TREE_DARK_OAK };

    void generateFlat(Chunk& c) const;
    void fillColumns(Chunk& c, ColumnInfo* cols) const;
    void carveCaves(Chunk& c, const ColumnInfo* cols) const;
    void placeOres(Chunk& c) const;
    int treeSites(int cx, int cz, TreeSite* out, int max) const;
    void placeTree(Chunk& c, const TreeSite& t) const;
    void decorate(Chunk& c, const ColumnInfo* cols) const;

    uint64_t seed_ = 0;
    WorldType type_ = WORLD_NORMAL;
    Noise cont_, hill_, detail_, mount_, river_, temp_, humid_, cave1_, cave2_, cave3_;
};

}  // namespace mc
