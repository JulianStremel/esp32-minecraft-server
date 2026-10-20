// A prototype of vanilla's (1.18+) terrain shape: the noise router's density functions,
// compiled from the 1.21.8 data pack (tools/worldgen/compile_router.js), evaluated in
// single-precision floats. Vanilla's algorithms (ImprovedNoise, PerlinNoise, NormalNoise,
// the legacy blended noise, cubic splines, cell interpolation), not its seeding: the
// shape and the scales are vanilla's, the exact world for a seed is not.
#pragma once
#include <limits.h>
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
extern const char* const DF_NOISE_NAMES[];   // without "minecraft:"
int noiseIndex(const char* name);            // -1 if the router has no such noise
extern const float DF_AMPLITUDES[];
extern const int16_t DF_ROOTS[];   // final_density, temperature, vegetation, continents, erosion, depth, ridges,
                                   // initial_density_without_jaggedness
extern const RouterShape DF_SHAPE;
enum { ROOT_FINAL_DENSITY, ROOT_TEMPERATURE, ROOT_VEGETATION, ROOT_CONTINENTS, ROOT_EROSION, ROOT_DEPTH, ROOT_RIDGES,
       ROOT_INITIAL_DENSITY };

class ImprovedNoise {
  public:
    // shared: use one permutation (in internal RAM) for every octave, each with its own
    // offsets: no lookups in PSRAM (the noises stay distinct; not vanilla's values);
    // otherwise own: 256 bytes for this octave's permutation
    void init(uint64_t& rng, const uint8_t* shared, uint8_t* own);
    float noise(float x, float y, float z, float yScale = 0, float yMax = 0) const;
  private:
    const uint8_t* p_;   // 16 bytes in all: the octaves of a router stay in a few cache lines
    float xo_, yo_, zo_;
};

class Router;
// The router compiled to C (router_gen.cpp, compile_router.js): one function per node.
typedef float (*GenFn)(Router& r, int x, int y, int z);
extern const GenFn DF_GEN[];
// final_density in the block pass, inlined (interpolated nodes through Router::interpAt)
extern const GenFn DF_BLOCK_DENSITY;

// A dimension's router: its tables and code (compile_router.js writes one per
// noise_settings preset: the overworld's names above, the Nether's and the End's in their
// own namespaces).
struct RouterDef {
    const DfNode* nodes;
    int numNodes;
    const SplineDef* splines;
    const SplinePoint* points;
    const NoiseDef* noises;
    int numNoises;
    const char* const* noiseNames;
    const float* amplitudes;
    const int16_t* roots;
    RouterShape shape;
    const GenFn* gen;
    GenFn block;
};
// The hot path's arithmetic without library calls, with the same results as the calls
// on every platform (the generated router and the interpreter use them):
//  - fmin_ / fmax_: newlib's fminf / fmaxf (a NaN gives the other operand), inline (newlib
//    calls __fpclassifyf twice for each)
//  - fdiv: a / b; on the ESP32-S3, the ROM's __divsf3 sequence (the FPU's correctly rounded
//    division steps) inline, without the call through the ROM's trampoline, the register
//    window and the moves between the integer and the float registers
inline float fmin_(float x, float y) { return (x < y || y != y) ? x : y; }
inline float fmax_(float x, float y) { return (x > y || y != y) ? x : y; }
inline float fdiv(float a, float b) {
#if defined(__XTENSA__) && defined(MC_XTENSA_FP_DIV)
    float q, d0, e, t, r, n, m, na;
    __asm__("div0.s     %[d0], %[b]\n\t"
            "nexp01.s   %[e], %[b]\n\t"
            "const.s    %[t], 1\n\t"
            "maddn.s    %[t], %[e], %[d0]\n\t"
            "mov.s      %[r], %[d0]\n\t"
            "mov.s      %[m], %[b]\n\t"
            "nexp01.s   %[n], %[a]\n\t"
            "maddn.s    %[r], %[t], %[r]\n\t"
            "const.s    %[t], 1\n\t"
            "const.s    %[q], 0\n\t"
            "neg.s      %[na], %[n]\n\t"
            "maddn.s    %[t], %[e], %[r]\n\t"
            "maddn.s    %[q], %[na], %[d0]\n\t"
            "mkdadj.s   %[m], %[a]\n\t"
            "maddn.s    %[r], %[t], %[r]\n\t"
            "maddn.s    %[na], %[e], %[q]\n\t"
            "const.s    %[d0], 1\n\t"
            "maddn.s    %[d0], %[e], %[r]\n\t"
            "maddn.s    %[q], %[na], %[r]\n\t"
            "neg.s      %[n], %[n]\n\t"
            "maddn.s    %[r], %[d0], %[r]\n\t"
            "maddn.s    %[n], %[e], %[q]\n\t"
            "addexpm.s  %[q], %[m]\n\t"
            "addexp.s   %[r], %[m]\n\t"
            "divn.s     %[q], %[n], %[r]"
            : [q] "=&f"(q), [d0] "=&f"(d0), [e] "=&f"(e), [t] "=&f"(t), [r] "=&f"(r), [n] "=&f"(n), [m] "=&f"(m),
              [na] "=&f"(na)
            : [a] "f"(a), [b] "f"(b));
    return q;
#else
    return a / b;
#endif
}

extern const RouterDef OVERWORLD_ROUTER;
namespace nether { extern const RouterDef ROUTER; }
namespace end { extern const RouterDef ROUTER; }

// Vanilla's Beardifier: what structures add to final_density around them, per block. A
// rigid piece (its box, the y its ground is at) with its structure's terrain adaptation, or
// a jigsaw junction (x0, y0 = its ground, z0).
enum BeardKind : uint8_t { BEARD_NONE, BEARD_THIN, BEARD_BOX, BEARD_BURY, BEARD_ENCAPSULATE, BEARD_JUNCTION };
struct BeardPiece { int32_t x0, y0, z0, x1, y1, z1, groundY; uint8_t kind; };

class Router {
  public:
    ~Router();
    // Approximations (0: exact): octaveCut drops octaves weighing less than this share of
    // their noise's largest; cellMargin fills a cell without per-block work when all its
    // corners are beyond +-cellMargin.
    bool init(uint64_t seed, float octaveCut = 0, float cellMargin = 0, bool sharedPermutation = false,
              bool generated = false, const RouterDef* def = nullptr);   // def: the overworld's by default
    const RouterDef& def() const { return *d_; }
    int noiseIndex(const char* name) const;   // in this router, -1 if none
    // end_islands (vanilla's EndIslandDensityFunction, this generator's simplex noise)
    float endIslands(int x, int z);
    const RouterDef* d_ = nullptr;
    // One root at a block position, without interpolation (climate, debugging).
    float sample(int root, int x, int y, int z);
    // The terrain of a chunk: out[(y * 16 + z) * 16 + x] = 1 where final_density > 0
    // (stone), for y from DF_SHAPE.minY over DF_SHAPE.height. Cell corners first, then
    // every block with the interpolated parts lerped, as vanilla's NoiseChunk.
    bool fillChunk(int cx, int cz, uint8_t* out);
    // The structures shaping the next fillChunk's terrain (nullptr: none; kept by the caller)
    void setBeard(const BeardPiece* p, int n) { beard_ = p; beardN_ = n; }
    float beard(int x, int y, int z) const;   // Beardifier#compute
    uint32_t beardBlocks = 0;                  // the last fillChunk's blocks with structures near
    uint32_t cornerUs = 0, blockUs = 0;   // the last fillChunk's two passes
    uint32_t boundUs = 0;                 // of cornerUs: the 2D table and the air bound
    uint32_t signUs = 0, fullUs = 0;      // of cornerUs: the sign slot, the other interpolated nodes
    uint32_t cellsSkipped = 0, cellsTotal = 0;
    uint32_t cellsAbove = 0;              // cells left out as air above the bound
    uint32_t cornersEvaluated = 0;
    uint32_t blocksEvaluated = 0, blocksSignAir = 0;   // the block pass: final_density, or air by the sign slot
    uint32_t cellsProven = 0;             // filled whole by the interval over the cell
    // Leave out the cells that are air for certain: an interval bound of final_density
    // over the cell and everything above it (noises at their largest possible value,
    // the 2D parts exact). Exact: the same blocks with or without.
    bool airBound = true;
    uint32_t octaveSamples = 0;   // ImprovedNoise samples of the last fillChunk

    // NoiseChunk#preliminarySurfaceLevel at (x, z) (quart-aligned): the highest cell y where
    // initial_density_without_jaggedness exceeds 0.390625, INT32_MAX if none; cached
    int preliminarySurface(int x, int z);
    // An upper bound of final_density at a block of the last chunk (lx, lz in it), from the
    // sign slot's corners (final_density is the min of that and the noodle caves): what the
    // aquifer's barriers compare with. -1 above the air bound (air for certain).
    float densityBound(int lx, int y, int lz);

    // used by the generated code as well (router_gen.cpp)
    float eval(int n, int x, int y, int z);
    float noiseValue(int noise, float x, float y, float z) const;
    float blended(const DfNode& n, int x, int y, int z) const;
    float spline(int s, int x, int y, int z);
    float node(int n, int x, int y, int z) { return generated_ ? d_->gen[n](*this, x, y, z) : eval(n, x, y, z); }
    bool generated_ = false;
    // an interpolated node in the block pass: lerped from the cell's corners when first read
    // at this block (interpNow_ holds it, or a value set beforehand: interpStamp_)
    float interpAt(int sl) {
        if (interpStamp_[sl] == stamp_) return interpNow_[sl];
        float v = interpNow_[sl];
        if (blockZ_) {   // the cell's x and z lerps done already (the same arithmetic)
            const float* z = blockZ_ + sl * 32;
            v = z[bdx_] + bfy_ * (z[16 + bdx_] - z[bdx_]);
        } else if (blockCorners_) {
            const float* c = blockCorners_ + sl * 8;
            float x = bfx_, z = bfz_;
            auto lerpf = [](float t, float a, float b) { return a + t * (b - a); };
            v = lerpf(bfy_, lerpf(z, lerpf(x, c[0], c[1]), lerpf(x, c[2], c[3])), lerpf(z, lerpf(x, c[4], c[5]), lerpf(x, c[6], c[7])));
        }
        interpNow_[sl] = v;
        interpStamp_[sl] = stamp_;
        return v;
    }
    const float* blockCorners_ = nullptr;   // the current cell's corners (8 per interpolated node)
    float* blockZ_ = nullptr;               // or per node and x in the cell: the lerps at the block's z (32 floats)
    float* zLerps_ = nullptr;
    float* xLerps_ = nullptr;
    int bdx_ = 0;
    float bfx_ = 0, bfy_ = 0, bfz_ = 0;
    uint32_t* interpStamp_ = nullptr;
    // flat_cache at one of the last chunk's corner columns: from the table fillChunk made
    bool flatTable(int n, int cx, int cz, float& v) const {
        if (!colFlatReady_ || d_->shape.cellW != 4) return false;
        int ix = (cx - cellX0_) >> 2, iz = (cz - cellZ0_) >> 2;
        if ((unsigned)ix >= (unsigned)cornersX_ || (unsigned)iz >= (unsigned)cornersX_) return false;
        v = colFlat_[(iz * cornersX_ + ix) * numFlat_ + flatSlot_[n]];
        return true;
    }

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
    ImprovedNoise* octaves_ = nullptr;   // every octave of every noise, one block
    bool octavesInternal_ = false;
    uint8_t* ownPerms_ = nullptr;        // without a shared permutation: 256 bytes per octave
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
    // the flat_cache values at the chunk's corner columns (flatSlot_: a node's column in it)
    int16_t* flatSlot_ = nullptr;
    int numFlat_ = 0;
    float* colFlat_ = nullptr;
    bool colFlatReady_ = false;

  private:
    struct Iv { float lo, hi; };
    Iv interval(int n);
    bool cellAir(int cell, int cy);
    float* noiseBound_ = nullptr;   // per noise: the largest |value| it can have
    float blendedBound_ = 0;
    Iv* iv_ = nullptr;              // interval memo, per node
    uint32_t* ivStamp_ = nullptr;
    uint32_t ivStampN_ = 0;
    int ivCol_ = -1;                // a corner column (2D parts exact), or -1: the inside of a cell
    float ivY0_ = 0, ivY1_ = 0;
    const Iv* ivInterp_ = nullptr;  // inside a cell: the interpolated nodes' ranges
    Iv* ivCell_ = nullptr;
    uint8_t* cellTop_ = nullptr;    // per cell column: the first cell layer that is air for certain
    uint8_t* cornerTop_ = nullptr;  // per corner column: the highest corner needed
    // final_density is not above 0 where this interpolated node is not (a chain of
    // sign-keeping functions over it, or the min of one): such cells and blocks are air
    // without the other interpolated nodes (-1: none)
    uint8_t islandPerm_[512];
    int32_t islandKey_ = INT32_MIN;
    float islandValue_ = 0;
    int signSlot_ = -1;
    int signNode_ = -1;   // the root of the sign-keeping chain (final_density, or one side of its min)
    struct PrelimEntry { int32_t key; int32_t level; };
    PrelimEntry* prelim_ = nullptr;   // a small hash of preliminary surface levels
    int chainSlot(int n) const;
    int8_t* cellState_ = nullptr;   // per cell (all layers): 1 solid, 0 air, -1 per block
    const BeardPiece* beard_ = nullptr;
    int beardN_ = 0;
    float* beardCell_ = nullptr;    // per cell (all layers): how much the beard can add, and take, there (0: none)
    int16_t beardSel_[256];         // the pieces reaching the cell being filled
    int beardSelN_ = -1;            // -1: all of them
    // a piece's reach: its box and 12 blocks around (Beardifier's kernel)
    static void beardReach(const BeardPiece& p, int& x0, int& y0, int& z0, int& x1, int& y1, int& z1);
    // the most a piece adds, and takes, anywhere in a block box (its kernel falls with the
    // distance; a beard adds below its ground and takes above it, a bury only adds)
    static void beardMost(const BeardPiece& p, int x0, int y0, int z0, int x1, int y1, int z1, float& add, float& take);
    uint8_t* cornerFull_ = nullptr; // per corner: what is needed (fillChunk)
};

}  // namespace vanilla
}  // namespace mc
