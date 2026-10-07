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

// Generator versions. A world keeps the version it was created with (stored with the
// world): chunks nobody changed are regenerated, so their terrain must never change.
//  1: the original generator. Noise coordinates are float world coordinates, so the
//     terrain loses detail far from spawn (at 29 million blocks neighbouring columns
//     get the same noise input) and repeats every 256 noise cells.
//  2: noise on exact lattice coordinates computed with integer arithmetic, and hashed
//     gradients that never repeat: the same detail everywhere up to the world border.
// Both produce the same blocks on the ESP32 and the PC (no fused multiply-add, see
// platformio.ini and host/Makefile; checked by generatorFingerprint()).
constexpr uint8_t GENERATOR_LATEST = 2;

// A checksum over a fixed set of generated chunks (blocks, biomes, heightmaps), near
// spawn, 1 million and 29.9 million blocks out. Every platform must get the same value;
// the unit tests and the device benchmark compare it with GENERATOR_GOLDEN.
uint32_t generatorFingerprint(uint64_t seed, uint8_t version);
struct GeneratorGolden {
    uint64_t seed;
    uint8_t version;
    uint32_t fingerprint;
};
extern const GeneratorGolden GENERATOR_GOLDEN[];
extern const int NUM_GENERATOR_GOLDEN;

class Generator {
public:
    void init(uint64_t seed, WorldType type, uint8_t version = GENERATOR_LATEST);
    uint64_t seed() const { return seed_; }
    WorldType type() const { return type_; }
    uint8_t version() const { return version_; }

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

    // one noise source in both versions' forms
    struct Layer {
        Noise v1;
        LatticeNoise v2;
        void init(uint64_t s) {
            v1.init(s);
            v2.init(s);
        }
    };
    float fbm(const Layer& l, int x, int z, const Freq& f, int octaves) const;
    float ridged(const Layer& l, int x, int z, const Freq& f, int octaves) const;
    float noise3(const Layer& l, int x, int y, int z, const Freq& fxz, const Freq& fy) const;

    uint64_t seed_ = 0;
    WorldType type_ = WORLD_NORMAL;
    uint8_t version_ = GENERATOR_LATEST;
    Layer cont_, hill_, detail_, mount_, river_, temp_, humid_, cave1_, cave2_, cave3_;
};

}  // namespace mc
