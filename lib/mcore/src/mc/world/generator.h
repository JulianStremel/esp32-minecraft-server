// Deterministic, seeded terrain generator. Everything is a pure function of
// (seed, x, z), so chunks that were never modified do not need to be stored:
// they are simply regenerated when needed. Trees that cross chunk borders are
// derived from neighbour-chunk tree sites, again without loading neighbours.
#pragma once
#include <stdint.h>
#include "mc/world/chunk.h"
#include "mc/world/noise.h"

namespace mc {

namespace vanilla { class Router; struct RouterDef; class Structures; struct BeardPiece; }

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
// lib/mcore/CMakeLists.txt and host/Makefile; checked by generatorFingerprint()).
constexpr uint8_t GENERATOR_LATEST = 2;   // what a new world gets unless it asks for another
// Vanilla's generation (vanilla_gen.cpp, docs/TERRAIN_GENERATION.md), for new worlds that
// choose it; slower (about 230 ms a chunk on the ESP32-S3 against 30-60 ms). A version
// per stage of the plan, so that a world keeps the terrain it was made with:
//  3: the terrain shape (the 1.21.8 noise router), provisional 2D biomes and surface
//  4: + vanilla's biomes from the climate (OverworldBiomeBuilder's table), cave biomes in 3D
//  5: + vanilla's surface rule
//  6: + vanilla's aquifers (dry caves, underground water and lava at their own levels) and ore veins
//  7: + vanilla's carvers (caves and canyons)
//  8: + vanilla's features (ores, trees, plants, disks, springs, ...), each inside its chunk
//  9: + the Nether and the End from vanilla's routers, biomes, surface rule and features
// 10: + vanilla's structures: desert pyramids, jungle temples, swamp huts, buried treasure;
//     the aquifer's locations right beyond 32,767 blocks (16-bit before)
// 11: + strongholds (vanilla's rings and pieces, the portal room)
// 12: + Nether fortresses
// 13: + mineshafts
// 14: + jigsaw structures: villages and pillager outposts (vanilla's templates)
constexpr uint8_t GENERATOR_VANILLA = 3;          // the first vanilla version
constexpr uint8_t GENERATOR_VANILLA_BIOMES = 4;
constexpr uint8_t GENERATOR_VANILLA_SURFACE = 5;
constexpr uint8_t GENERATOR_VANILLA_AQUIFERS = 6;
constexpr uint8_t GENERATOR_VANILLA_CARVERS = 7;
constexpr uint8_t GENERATOR_VANILLA_FEATURES = 8;
constexpr uint8_t GENERATOR_VANILLA_DIMS = 9;
constexpr uint8_t GENERATOR_VANILLA_STRUCTURES = 10;
constexpr uint8_t GENERATOR_VANILLA_STRONGHOLDS = 11;
constexpr uint8_t GENERATOR_VANILLA_FORTRESSES = 12;
constexpr uint8_t GENERATOR_VANILLA_MINESHAFTS = 13;
constexpr uint8_t GENERATOR_VANILLA_VILLAGES = 14;
constexpr uint8_t GENERATOR_VANILLA_LATEST = 14;   // what "vanilla" means for a new world
constexpr uint8_t GENERATOR_MAX = 14;
constexpr bool isVanillaGenerator(uint8_t version) { return version >= GENERATOR_VANILLA; }

// Checksums over a fixed set of generated chunks, near spawn, 1 million and 29.9 million
// blocks out. Every platform must get the same values; the unit tests and the device
// benchmark compare them with GENERATOR_GOLDEN.
//  - generatorFingerprint: the blocks, biomes and heightmaps. It stays the same as
//    long as the terrain does.
//  - generatorFloatFingerprint: the bit patterns of the noise values and of the
//    unrounded heights, temperatures and humidities at those chunks' columns. A
//    rounding difference (fused multiply-add, another libm) changes a block in only a
//    few chunks in a hundred, so the blocks of nine chunks can miss it; this cannot.
uint32_t generatorFingerprint(uint64_t seed, uint8_t version);
uint32_t generatorFloatFingerprint(uint64_t seed, uint8_t version);
struct GeneratorGolden {
    uint64_t seed;
    uint8_t version;
    uint32_t blocks, floats;
};
extern const GeneratorGolden GENERATOR_GOLDEN[];
extern const int NUM_GENERATOR_GOLDEN;
// The Nether's and the End's terrain: the same checksum over the same chunks. These
// dimensions have one generator version so far.
uint32_t generatorDimFingerprint(uint64_t seed, uint8_t dim, uint8_t version = GENERATOR_LATEST);
struct GeneratorDimGolden {
    uint64_t seed;
    uint32_t nether, end;
    uint8_t version;   // 0: GENERATOR_LATEST (the classic dimensions)
};
extern const GeneratorDimGolden GENERATOR_DIM_GOLDEN[];
extern const int NUM_GENERATOR_DIM_GOLDEN;
// false if generator.cpp or noise.cpp was compiled with fused multiply-add, which
// rounds differently from a separate multiply and add (build with -ffp-contract=off)
bool generatorArithmeticIsPortable();

// The End's 10 obsidian spikes for a world seed (vanilla SpikeFeature, positions and
// heights as vanilla's for the same seed).
struct EndSpike {
    int x, z, radius, height;
    bool guarded;   // caged in iron bars
};
void endSpikes(uint64_t worldSeed, EndSpike out[10]);
void placeEndSpikes(Chunk& c, const EndSpike* spikes);   // their blocks in chunk c

class Generator {
public:
    Generator() = default;
    ~Generator();
    Generator(const Generator&) = delete;
    Generator& operator=(const Generator&) = delete;
    // false for a version this build does not have (nothing is initialised then)
    // dim: the dimension this generator makes (the Nether and the End ignore type and version)
    bool init(uint64_t seed, WorldType type, uint8_t version = GENERATOR_LATEST, uint8_t dim = DIM_OVERWORLD);
    uint64_t seed() const { return seed_; }
    WorldType type() const { return type_; }
    uint8_t version() const { return version_; }
    uint8_t dim() const { return dim_; }

    void generate(Chunk& c) const;
    // raw (optional): the height, temperature and humidity before rounding
    ColumnInfo column(int x, int z, float* raw = nullptr) const;
    // mixes the bit patterns of every noise value and of column()'s raw values at a
    // column into an FNV-1a hash (generatorFloatFingerprint())
    uint32_t floatHash(int x, int z, uint32_t h) const;
    // Finds a dry land position near the origin. y is the first air block above ground.
    void findSpawn(int& x, int& y, int& z) const;
    // the End: the first air above the island as generated (0 over the void)
    int endSurfaceY(int x, int z) const;
    // version 10: the start chunk of the nearest structure of this name ("desert_pyramid")
    // within radius blocks of (x, z); false if none or this generator has no structures
    bool locateStructure(const char* name, int x, int z, int radius, int& cx, int& cz) const;

    // version 3: the last chunk's steps in microseconds (the benchmark; with several
    // workers, whichever finished last)
    struct VanillaSteps { uint32_t shape, write, biomes, surface, features; };
    mutable VanillaSteps vanillaSteps = {};

private:
    struct TreeSite { int x, z, y; uint8_t kind; };
    enum TreeKind : uint8_t { TREE_OAK, TREE_BIRCH, TREE_SPRUCE, TREE_JUNGLE, TREE_ACACIA, TREE_DARK_OAK };

    void generateFlat(Chunk& c) const;
    void generateNether(Chunk& c) const;   // dim_generators.cpp
    void generateEnd(Chunk& c) const;
    void fillColumns(Chunk& c, ColumnInfo* cols) const;
    void carveCaves(Chunk& c, const ColumnInfo* cols) const;
    void placeOres(Chunk& c) const;
    int treeSites(int cx, int cz, TreeSite* out, int max) const;
    void placeTree(Chunk& c, const TreeSite& t) const;
    static bool vanillaTree(void* ctx, Chunk& c, int lx, int y, int lz, int kind, int size, int height);
    void decorate(Chunk& c, const ColumnInfo* cols, bool trees = true) const;
    // version 3 (vanilla_gen.cpp)
    void generateVanilla(Chunk& c) const;
    // version 9's Nether and End (vanilla_dims.cpp)
    void generateVanillaNether(Chunk& c) const;
    void generateVanillaEnd(Chunk& c) const;
    int vanillaEndSurfaceY(int x, int z) const;
    const vanilla::RouterDef* routerDef() const;   // this dimension's (nullptr: the overworld's)
    void findVanillaSpawn(int& x, int& y, int& z) const;
    int takeRouter() const;
    void giveRouter(int i) const;
    static constexpr int POOL = 4;
    mutable vanilla::Router* pool_[POOL] = {};
    mutable uint8_t* poolBuf_[POOL] = {};
    mutable uint16_t* poolStates_[POOL] = {};
    mutable uint8_t* poolBiomes_[POOL] = {};   // 3D biomes of the chunk (16 x 96)
    static constexpr int BEARD_MAX = 256;
    mutable vanilla::BeardPiece* poolBeard_[POOL] = {};   // the structures shaping the chunk (version 14)
    mutable bool poolBusy_[POOL] = {};
    void* poolLock_ = nullptr;
    vanilla::Structures* structures_ = nullptr;   // version 10

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
    uint8_t dim_ = DIM_OVERWORLD;
    Layer cont_, hill_, detail_, mount_, river_, temp_, humid_, cave1_, cave2_, cave3_;
};

}  // namespace mc
