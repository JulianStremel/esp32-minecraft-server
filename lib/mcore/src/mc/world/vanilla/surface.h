// Vanilla's surface rule (SurfaceRules, SurfaceSystem#buildSurface) for the overworld,
// compiled by tools/worldgen/compile_surface.js (surface_rules.cpp) and run on the block
// mask the router makes, from each column's top down to its preliminary surface: below it
// vanilla's rule changes nothing but bedrock and deepslate, which the generator places.
#pragma once
#include <stdint.h>

namespace mc {
namespace vanilla {

class Router;

enum SrOp : uint8_t { SR_SEQ, SR_COND, SR_BLOCK, SR_BANDS };
enum SrCondType : uint8_t {
    SC_BIOME, SC_NOISE_THRESHOLD, SC_STONE_DEPTH, SC_Y_ABOVE, SC_WATER, SC_NOT, SC_HOLE, SC_STEEP, SC_TEMPERATURE,
    SC_ABOVE_PRELIMINARY_SURFACE
};
struct SrNode { uint8_t op; int16_t a, b; };   // seq: children a.., count b; cond: condition a, then b; block: state a
struct SrCond {
    uint8_t type, flags;   // stone_depth: 1 ceiling, 2 add the surface depth; y_above, water: 1 add the stone depth
    int16_t a, b;          // offset or anchor y; secondary depth range or surface depth multiplier
    float lo, hi;          // noise threshold
    uint64_t biomes[2];
    int8_t noise;          // router noise index
    int16_t inner;         // not: the condition inverted
};
extern const SrNode SR_NODES[];
extern const int16_t SR_CHILDREN[];
extern const SrCond SR_CONDS[];
extern const uint16_t SR_STATES[];
extern const int SR_NUM_STATES;
extern const uint8_t SR_BAND_STATES[];
extern const int SR_ROOT, SR_MAX_CEILING_DEPTH;
extern const int16_t SR_BIOME_ROOT[];   // per biome: the rule with its biome conditions decided (-1: none)
extern const int SR_DEPTH_SD_MIN, SR_DEPTH_SD_MAX;
extern const uint8_t SR_DEPTH_BOUND[];   // per biome and surface depth: the deepest stoneAbove a rule reaches (255: any)
extern const int SR_NOISE_SURFACE, SR_NOISE_SECONDARY, SR_NOISE_CLAY_BANDS;
extern const float BIOME_TEMPERATURE[];

// A dimension's compiled rule (compile_surface.js: the overworld's names above, the
// Nether's in vanilla::nether).
struct SurfaceDef {
    const SrNode* nodes;
    const int16_t* children;
    const SrCond* conds;
    const uint16_t* states;
    int numStates;
    const uint8_t* bandStates;
    const int16_t* biomeRoot;
    int sdMin, sdMax;
    const uint8_t* depthBound;   // per biome and surface depth: the deepest stoneAbove a rule reaches (255: any)
    const uint8_t* ceilBound;    // the same for stoneBelow
    int maxCeiling;
    int noiseSurface, noiseSecondary, noiseClayBands;
};
extern const SurfaceDef OVERWORLD_SURFACE;
namespace nether { extern const SurfaceDef SURFACE; }

// The surface of one chunk. mask: the router's blocks (fillChunk's layout, 1 solid), on
// return a solid block the rule replaced holds 2 + its index in SR_STATES. tops: each
// column's highest solid block. biomes: per 4 x 4 x 4 cell, ((y - minY) >> 2) * 16 +
// (z >> 2) * 4 + (x >> 2). Returns the blocks the rule evaluated (the cost).
uint32_t buildSurface(Router& r, uint64_t seed, int cx, int cz, uint8_t* mask, const int* tops, const uint8_t* biomes);
// The Nether's: every column top to bottom (floors and ceilings of the caverns), the
// lava sea below y 32 a fluid; biomes per 4 x 4 column. Bedrock and the roof's top five
// blocks are the generator's.
uint32_t buildSurfaceNether(Router& r, uint64_t seed, int cx, int cz, uint8_t* mask, const uint8_t* columnBiomes);
// the last buildSurface's steps in microseconds (the benchmark)
struct SurfaceSteps { uint32_t prelimUs, columnsUs, rulesUs, evaluated; };
extern SurfaceSteps surfaceSteps;

}  // namespace vanilla
}  // namespace mc
