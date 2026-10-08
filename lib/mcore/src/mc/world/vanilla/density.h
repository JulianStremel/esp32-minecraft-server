// A prototype of vanilla's (1.18+) terrain shape: the noise router's density functions,
// compiled from the 1.21.8 data pack (tools/worldgen/compile_router.js), evaluated in
// single-precision floats. Vanilla's algorithms (ImprovedNoise, PerlinNoise, NormalNoise,
// the legacy blended noise, cubic splines, cell interpolation), not its seeding: the
// shape and the scales are vanilla's, the exact world for a seed is not.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace mc {
namespace vanilla {

enum DfOp : uint8_t {
    DF_CONSTANT, DF_ADD, DF_MUL, DF_MIN, DF_MAX, DF_ABS, DF_SQUARE, DF_CUBE, DF_HALF_NEGATIVE, DF_QUARTER_NEGATIVE,
    DF_SQUEEZE, DF_CLAMP, DF_Y_CLAMPED_GRADIENT, DF_RANGE_CHOICE, DF_NOISE, DF_SHIFTED_NOISE, DF_SHIFT_A, DF_SHIFT_B,
    DF_SHIFT, DF_WEIRD_SCALED_SAMPLER, DF_OLD_BLENDED_NOISE, DF_SPLINE, DF_INTERPOLATED, DF_FLAT_CACHE, DF_CACHE_2D,
    DF_CACHE_ONCE, DF_CACHE_ALL_IN_CELL, DF_BLEND_ALPHA, DF_BLEND_OFFSET, DF_BLEND_DENSITY, DF_BEARDIFIER, DF_END_ISLANDS
};

struct DfNode {
    uint8_t op, aux;     // aux: weird_scaled_sampler's rarity mapper (1, 2)
    int16_t a, b, c;     // arguments (-1: none)
    uint16_t noise;      // noise index, or spline index for DF_SPLINE
    float p[5];          // parameters (scales, bounds, constants)
};
struct SplineDef { int16_t coord; uint16_t first, count; };
struct SplinePoint { float loc, der, value; int16_t sub; };   // sub: a nested spline (-1: value)
struct NoiseDef { int8_t first; uint8_t count; uint16_t start; };
struct RouterShape { int16_t minY, height; uint8_t cellW, cellH; int16_t seaLevel; };

extern const DfNode DF_NODES[];
extern const int NUM_DF_NODES;
extern const SplineDef DF_SPLINES[];
extern const SplinePoint DF_SPLINE_POINTS[];
extern const NoiseDef DF_NOISES[];
extern const int NUM_DF_NOISES;
extern const float DF_AMPLITUDES[];
extern const int16_t DF_ROOTS[];   // final_density, temperature, vegetation, continents, erosion, depth, ridges
extern const RouterShape DF_SHAPE;
enum { ROOT_FINAL_DENSITY, ROOT_TEMPERATURE, ROOT_VEGETATION, ROOT_CONTINENTS, ROOT_EROSION, ROOT_DEPTH, ROOT_RIDGES };

class ImprovedNoise {
  public:
    // shared: use one permutation (in internal RAM) for every octave, each with its own
    // offsets: no lookups in PSRAM (the noises stay distinct; not vanilla's values)
    void init(uint64_t& rng, const uint8_t* shared = nullptr);
    float noise(float x, float y, float z, float yScale = 0, float yMax = 0) const;
  private:
    const uint8_t* p_;
    uint8_t own_[256];
    float xo_, yo_, zo_;
};

class Router;
// The router compiled to C (router_gen.cpp, compile_router.js): one function per node.
typedef float (*GenFn)(Router& r, int x, int y, int z);
extern const GenFn DF_GEN[];

class Router {
  public:
    ~Router();
    // Approximations (0: exact): octaveCut drops octaves weighing less than this share of
    // their noise's largest; cellMargin fills a cell without per-block work when all its
    // corners are beyond +-cellMargin.
    bool init(uint64_t seed, float octaveCut = 0, float cellMargin = 0, bool sharedPermutation = false,
              bool generated = false);
    // One root at a block position, without interpolation (climate, debugging).
    float sample(int root, int x, int y, int z);
    // The terrain of a chunk: out[(y * 16 + z) * 16 + x] = 1 where final_density > 0
    // (stone), for y from DF_SHAPE.minY over DF_SHAPE.height. Cell corners first, then
    // every block with the interpolated parts lerped, as vanilla's NoiseChunk.
    bool fillChunk(int cx, int cz, uint8_t* out);
    uint32_t cornerUs = 0, blockUs = 0;   // the last fillChunk's two passes
    uint32_t cellsSkipped = 0, cellsTotal = 0;
    uint32_t octaveSamples = 0;   // ImprovedNoise samples of the last fillChunk

    // used by the generated code as well (router_gen.cpp)
    float eval(int n, int x, int y, int z);
    float noiseValue(int noise, float x, float y, float z) const;
    float blended(const DfNode& n, int x, int y, int z) const;
    float spline(int s, int x, int y, int z);
    float node(int n, int x, int y, int z) { return generated_ ? DF_GEN[n](*this, x, y, z) : eval(n, x, y, z); }
    bool generated_ = false;

    struct Perlin { int first = 0, count = 0, start = 0; ImprovedNoise* levels = nullptr; float inFactor = 1, valFactor = 1;
                    uint32_t skip = 0; };   // skip: octaves left out (bits)
    float cellMargin_ = 0;
    int blendedSkip_ = 0, mainSkip_ = 0;   // the blended noise's finest octaves left out (limits, main)
    float* cornerDensity_ = nullptr;
    // the block pass: the interpolated values at the current block, and the current cell's
    // corners of each (8 per interpolated node), in internal RAM
    float* interpNow_ = nullptr;
    float* cellCorners_ = nullptr;
    int8_t* cellFill_ = nullptr;
    uint8_t* perm_ = nullptr;   // the shared permutation (internal RAM)
    Perlin* perlin_ = nullptr;   // two per noise (NormalNoise)
    float* normalFactor_ = nullptr;
    ImprovedNoise* blendedNoise_ = nullptr;   // 16 min limit, 16 max limit, 8 main
    float* memo_ = nullptr;
    uint32_t* memoStamp_ = nullptr;
    int32_t* cacheKey_ = nullptr;   // flat_cache / cache_2d: the column of memo_
    uint32_t stamp_ = 1, stampCounter_ = 1;
    // interpolation: corner values of each interpolated node
    bool corners_ = false;   // evaluating at cell corners
    int16_t* interpSlot_ = nullptr;
    float* cornerValues_ = nullptr;
    int interpCount_ = 0, cornersX_ = 0, cornersY_ = 0;
    int cellX0_ = 0, cellZ0_ = 0;
};

}  // namespace vanilla
}  // namespace mc
