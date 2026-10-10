#include "mc/world/vanilla/surface.h"
#include <math.h>
#include <string.h>
#include "mc/platform.h"
#include "mc/registry.h"
#include "mc/world/vanilla/aquifer.h"
#include "mc/world/vanilla/density.h"

namespace mc {
namespace vanilla {

namespace {
constexpr uint16_t SR_NONE = 0xFFFF;
constexpr int SEA = 63;   // water up to y 62
constexpr int32_t NO_WATER = INT32_MIN;

uint64_t mix(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// SurfaceRules.Context: the column, then the block
struct Ctx {
    Router* r;
    uint64_t seed;
    int x, z, col;
    const int* heights;   // WORLD_SURFACE_WG + 1 per column of the chunk
    int surfaceDepth;
    float secondary;
    bool secondaryKnown;
    float noise[64];
    uint64_t noiseKnown;
    int minSurface;
    int8_t steep;   // -1 not known yet
    int y, stoneAbove, stoneBelow, waterHeight;
    uint8_t biome;
    bool bandsMade;
    uint8_t bands[192];   // indices into SR_STATES
};

float noiseAt(Ctx& c, int n) {
    if (!(c.noiseKnown >> n & 1)) {
        c.noise[n] = c.r->noiseValue(n, (float)c.x, 0, (float)c.z);
        c.noiseKnown |= 1ull << n;
    }
    return c.noise[n];
}

// SurfaceSystem#generateBands, with this generator's random numbers
void makeBands(Ctx& c) {
    uint64_t s = c.seed ^ 0xC1A7BA4D5ull;
    auto next = [&](int n) { return (int)(mix(s += 0x9E3779B97F4A7C15ull) % (uint64_t)n); };
    auto between = [&](int lo, int hi) { return lo + next(hi - lo + 1); };
    for (int i = 0; i < 192; i++) c.bands[i] = SR_BAND_STATES[0];   // terracotta
    for (int i = 0; i < 192; i++) {
        i += next(5) + 1;
        if (i < 192) c.bands[i] = SR_BAND_STATES[1];   // orange
    }
    auto band = [&](int minWidth, uint8_t st) {
        int n = between(6, 15);
        for (int j = 0; j < n; j++) {
            int w = minWidth + next(3), at = next(192);
            for (int m = 0; at + m < 192 && m < w; m++) c.bands[at + m] = st;
        }
    };
    band(1, SR_BAND_STATES[2]);   // yellow
    band(2, SR_BAND_STATES[3]);   // brown
    band(1, SR_BAND_STATES[4]);   // red
    int whites = between(9, 15), k = 0;
    for (int l = 0; k < whites && l < 192; l += next(16) + 4) {
        c.bands[l] = SR_BAND_STATES[5];
        if (l - 1 > 0 && next(2)) c.bands[l - 1] = SR_BAND_STATES[6];
        if (l + 1 < 192 && next(2)) c.bands[l + 1] = SR_BAND_STATES[6];
        k++;
    }
    c.bandsMade = true;
}

bool test(Ctx& c, int ci) {
    const SrCond& k = SR_CONDS[ci];
    switch (k.type) {
        case SC_BIOME: return (k.biomes[c.biome >> 6] >> (c.biome & 63)) & 1;
        case SC_NOISE_THRESHOLD: {
            float v = noiseAt(c, k.noise);
            return v >= k.lo && v <= k.hi;
        }
        case SC_STONE_DEPTH: {
            int depth = (k.flags & 1) ? c.stoneBelow : c.stoneAbove;
            int add = (k.flags & 2) ? c.surfaceDepth : 0, sec = 0;
            if (k.b) {   // Mth.map(secondary, -1, 1, 0, range)
                if (!c.secondaryKnown) {
                    c.secondary = noiseAt(c, SR_NOISE_SECONDARY);
                    c.secondaryKnown = true;
                }
                sec = (int)((c.secondary + 1.0f) / 2.0f * (float)k.b);
            }
            return depth <= 1 + k.a + add + sec;
        }
        case SC_Y_ABOVE: return c.y + ((k.flags & 1) ? c.stoneAbove : 0) >= k.a + c.surfaceDepth * k.b;
        case SC_WATER:
            return c.waterHeight == NO_WATER ||
                   c.y + ((k.flags & 1) ? c.stoneAbove : 0) >= c.waterHeight + k.a + c.surfaceDepth * k.b;
        case SC_NOT: return !test(c, k.inner);
        case SC_HOLE: return c.surfaceDepth <= 0;
        case SC_STEEP:
            if (c.steep < 0) {   // SurfaceRules.Steep: the neighbours' heights within the chunk
                int lx = c.col & 15, lz = c.col >> 4;
                int zn = lz > 0 ? lz - 1 : 0, zs = lz < 15 ? lz + 1 : 15, xw = lx > 0 ? lx - 1 : 0, xe = lx < 15 ? lx + 1 : 15;
                c.steep = c.heights[zs * 16 + lx] >= c.heights[zn * 16 + lx] + 4 ||
                          c.heights[lz * 16 + xw] >= c.heights[lz * 16 + xe] + 4;
            }
            return c.steep;
        case SC_TEMPERATURE: {   // Biome#coldEnoughToSnow, colder with height above y 80
            float t = BIOME_TEMPERATURE[c.biome];
            if (c.y > SEA + 17) t -= (float)(c.y - (SEA + 17)) * 0.05f / 40.0f;
            return t < 0.15f;
        }
        case SC_ABOVE_PRELIMINARY_SURFACE: return c.y >= c.minSurface;
        default: return false;
    }
}

uint16_t apply(Ctx& c, int ni) {
    const SrNode& n = SR_NODES[ni];
    switch (n.op) {
        case SR_SEQ:
            for (int i = 0; i < n.b; i++) {
                uint16_t v = apply(c, SR_CHILDREN[n.a + i]);
                if (v != SR_NONE) return v;
            }
            return SR_NONE;
        case SR_COND: return test(c, n.a) ? apply(c, n.b) : SR_NONE;
        case SR_BLOCK: return (uint16_t)n.a;
        case SR_BANDS: {   // SurfaceSystem#getBand
            if (!c.bandsMade) makeBands(c);
            int off = (int)floorf(noiseAt(c, SR_NOISE_CLAY_BANDS) * 4.0f + 0.5f);
            return c.bands[((c.y + off) % 192 + 192 * 2) % 192];
        }
        default: return SR_NONE;
    }
}
}  // namespace

SurfaceSteps surfaceSteps;

uint32_t buildSurface(Router& r, uint64_t seed, int cx, int cz, uint8_t* mask, const int* tops, const uint8_t* biomes) {
    uint64_t t0 = plat::micros(), colUs = 0;
    const int minY = DF_SHAPE.minY, H = DF_SHAPE.height, cw = DF_SHAPE.cellW;
    // NoiseChunk#computePreliminarySurfaceLevel at the 5 x 5 corner columns: the highest
    // cell y where initial_density_without_jaggedness exceeds 0.390625
    double prelim[25];
    for (int k = 0; k < 25; k++) prelim[k] = r.preliminarySurface(cx * 16 + (k % 5) * cw, cz * 16 + (k / 5) * cw);
    uint64_t t1 = plat::micros();
    int heights[256];   // WORLD_SURFACE_WG: the highest block that is not air, + 1
    for (int col = 0; col < 256; col++) heights[col] = (tops[col] < SEA - 1 ? SEA - 1 : tops[col]) + 1;
    Ctx c;   // ~550 bytes
    memset(&c, 0, sizeof(c));
    c.r = &r;
    c.seed = seed;
    c.heights = heights;
    uint32_t evaluated = 0;
    const int cap = SR_MAX_CEILING_DEPTH + 1;   // stone below counted this far (no condition looks further)
    for (int col = 0; col < 256; col++) {
        uint64_t tc = plat::micros();
        int lx = col & 15, lz = col >> 4;
        c.x = cx * 16 + lx;
        c.z = cz * 16 + lz;
        c.col = col;
        c.noiseKnown = 0;
        c.secondaryKnown = false;
        c.steep = -1;
        // SurfaceSystem#getSurfaceDepth
        double rnd = (double)(mix(seed ^ mix((uint64_t)(uint32_t)c.x << 32 | (uint32_t)c.z)) >> 11) * (1.0 / 9007199254740992.0);
        c.surfaceDepth = (int)(noiseAt(c, SR_NOISE_SURFACE) * 2.75 + 3.0 + rnd * 0.25);
        // Context#getMinSurfaceLevel: the corners' preliminary levels lerped, + depth - 8
        int qx = lx >> 2, qz = lz >> 2;
        double fx = (lx & 3) / 4.0, fz = (lz & 3) / 4.0;
        double p00 = prelim[qz * 5 + qx], p10 = prelim[qz * 5 + qx + 1], p01 = prelim[(qz + 1) * 5 + qx],
               p11 = prelim[(qz + 1) * 5 + qx + 1];
        double lerp2 = (p00 + fx * (p10 - p00)) + fz * ((p01 + fx * (p11 - p01)) - (p00 + fx * (p10 - p00)));
        double minSurface = floor(lerp2) + c.surfaceDepth - 8;
        colUs += plat::micros() - tc;
        if (minSurface > (double)(minY + H)) continue;   // no preliminary surface: the rule never applies
        c.minSurface = (int)minSurface;
        // SurfaceSystem#buildSurface from the column's top down to the preliminary surface
        c.stoneAbove = 0;
        c.waterHeight = NO_WATER;
        for (int y = heights[col] - 1; y >= minY && y >= c.minSurface; y--) {
            int i = y - minY;
            uint8_t* at = mask + (size_t)i * 256 + col;
            // what the block is: 0 the fluid by y (or air), 1 stone, from 2 a state the aquifer or
            // the ore veins put there (air, water, lava; ores count as stone, the rule leaves them)
            uint16_t st = *at < 2 ? 1 : maskState(*at);
            bool fluid = *at == 0 ? y < SEA : st == bs::Water || st == bs::Lava;
            if (*at == 0 || st == 0 || fluid) {
                if (fluid) {   // water (or lava): the first one's top
                    if (c.waterHeight == NO_WATER) c.waterHeight = y + 1;
                } else {
                    c.stoneAbove = 0;
                    c.waterHeight = NO_WATER;
                }
                continue;
            }
            c.stoneAbove++;
            if (*at >= 2) continue;   // an ore vein's block
            c.biome = biomes[(i >> 2) * 16 + (lz >> 2) * 4 + (lx >> 2)];
            // the biome's own rule, and only as deep as any of it reaches at this surface depth
            int rule = SR_BIOME_ROOT[c.biome];
            if (rule < 0) continue;
            int sd = c.surfaceDepth;
            int bound = sd < SR_DEPTH_SD_MIN || sd > SR_DEPTH_SD_MAX
                            ? 255 : SR_DEPTH_BOUND[c.biome * (SR_DEPTH_SD_MAX - SR_DEPTH_SD_MIN + 1) + sd - SR_DEPTH_SD_MIN];
            if (bound != 255 && c.stoneAbove > bound) continue;
            c.stoneBelow = 1;   // this block and the stone right under it, up to cap
            while (c.stoneBelow < cap && i - c.stoneBelow >= 0 && mask[(size_t)(i - c.stoneBelow) * 256 + col])
                c.stoneBelow++;
            c.y = y;
            evaluated++;
            uint16_t v = apply(c, rule);
            if (v != SR_NONE) *at = (uint8_t)(2 + v);
        }
    }
    uint64_t t2 = plat::micros();
    surfaceSteps = {(uint32_t)(t1 - t0), (uint32_t)colUs, (uint32_t)(t2 - t1 - colUs), evaluated};
    return evaluated;
}

}  // namespace vanilla
}  // namespace mc
