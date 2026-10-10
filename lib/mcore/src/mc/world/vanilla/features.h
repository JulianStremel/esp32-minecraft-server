// Vanilla's features for the overworld (biome "features", placed_feature, configured_feature)
// compiled by tools/worldgen/compile_features.js (feature_data.cpp) and run on a generated
// chunk in vanilla's decoration steps. Each feature stays inside its chunk (vanilla lets
// them reach into the neighbours, which are generated around it first; here a chunk is
// generated alone).
#pragma once
#include <stdint.h>

namespace mc {

class Chunk;

namespace vanilla {

enum FdProvType : uint8_t { FP_CONSTANT, FP_UNIFORM, FP_BIASED, FP_VERY_BIASED, FP_TRAPEZOID, FP_WEIGHTED, FP_CLAMPED };
enum FdStateType : uint8_t { FS_SIMPLE, FS_WEIGHTED, FS_NOISE_THRESHOLD, FS_NOISE, FS_RULES };
enum FdPredType : uint8_t { FB_TRUE, FB_BLOCKS, FB_FLUIDS, FB_REPLACEABLE, FB_SOLID, FB_SURVIVE, FB_NOT, FB_ALL, FB_ANY };
enum FdModType : uint8_t {
    FM_COUNT, FM_RARITY_FILTER, FM_IN_SQUARE, FM_BIOME, FM_HEIGHT_RANGE, FM_HEIGHTMAP, FM_BLOCK_PREDICATE_FILTER,
    FM_SURFACE_WATER_DEPTH_FILTER, FM_ENVIRONMENT_SCAN, FM_SURFACE_RELATIVE_THRESHOLD_FILTER, FM_NOISE_BASED_COUNT,
    FM_NOISE_THRESHOLD_COUNT, FM_RANDOM_OFFSET
};
enum FdCfgType : uint8_t {
    FC_SKIP, FC_RANDOM_PATCH, FC_SIMPLE_BLOCK, FC_ORE, FC_TREE, FC_RANDOM_SELECTOR, FC_SIMPLE_RANDOM_SELECTOR,
    FC_RANDOM_BOOLEAN_SELECTOR, FC_SEAGRASS, FC_KELP, FC_FREEZE_TOP_LAYER, FC_DISK, FC_SPRING_FEATURE, FC_BLOCK_COLUMN
};

struct FdSet { int16_t first, count; };
struct FdProv { uint8_t type; int16_t a, b, c; float fa, fb, fc; };
struct FdStateProv { uint8_t type; int16_t a, b, c; float f, g, h; };
struct FdPred { uint8_t type; int8_t dx, dy, dz; int16_t a, b; };
struct FdMod { uint8_t type; int32_t a, b, c; float f; };
struct FdCfg { uint8_t type; int32_t a, b, c, d, e; float f; };
struct FdPlaced { int16_t cfg, first; uint8_t count; };

extern const char* const FD_STATES[];
extern const int FD_NUM_STATES;
extern const char* const FD_SET_NAMES[];
extern const FdSet FD_SETS[];
extern const int FD_NUM_SETS;
extern const int32_t FD_LISTS[];
extern const FdProv FD_PROVS[];
extern const FdStateProv FD_SPROVS[];
extern const FdPred FD_PREDS[];
extern const FdMod FD_MODS[];
extern const FdCfg FD_CFGS[];
extern const int FD_NUM_CFGS;
extern const FdPlaced FD_PLACED[];
extern const int FD_NUM_PLACED;
extern const int16_t FD_BIOME_FIRST[], FD_BIOME_COUNT[], FD_BIOME_PAIRS[];

struct FeatureSteps { uint32_t us, placed, skipped, blocks, heightmapUs, typeUs[16], ores, oresUnder, spheres, voxels, oreUs[4]; };
extern FeatureSteps featureSteps;   // the last chunk's (the benchmark)

// Trees go through this (the generator's tree shapes): kind (Generator's TREE_*), the base,
// the trunk's height hint; false if it does not fit the chunk.
typedef bool (*TreeFn)(void* ctx, Chunk& c, int x, int y, int z, int kind, int size, int height);

// The tables' block names resolved: once, before any decorateChunk (Generator::init).
void featuresInit();
// The ores, on the block mask before the sections are written (vanilla::maskState's codes;
// tops: each column's top block; bedrock, deep: the generator's bits per column).
void decorateMask(Chunk& c, uint8_t* mask, uint64_t seed, const int* tops, const uint8_t* bedrock, const uint8_t* deep);
// Decorates a chunk whose blocks, heightmap and biomes are made. Thread-safe.
void decorateChunk(Chunk& c, uint64_t seed, TreeFn tree, void* treeCtx);

}  // namespace vanilla
}  // namespace mc
