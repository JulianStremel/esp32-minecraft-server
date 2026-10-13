#include "mc/world/vanilla/biomes.h"

namespace mc {
namespace vanilla {

namespace {
// Climate.Parameter#distance: how far v is outside [lo, hi]
inline int64_t outside(int32_t v, int32_t lo, int32_t hi) {
    int64_t d = v < lo ? (int64_t)lo - v : v > hi ? (int64_t)v - hi : 0;
    return d * d;
}
}  // namespace

void columnClimate(const float climate[5], ColumnClimate& out) {
    int idx = 0;
    int64_t dist = 0;
    for (int i = 0; i < 5; i++) {
        const BiomeAxis& a = BIOME_AXES[i];
        int32_t v = quantizeClimate(climate[i]);
        out.p[i] = v;
        int seg = 0;
        // the grid's segments tile [first bound, last bound]; outside, the nearest edge
        // segment, at the distance vanilla would find to it
        dist += outside(v, a.bounds[0], a.bounds[a.count - 1]);
        while (seg < a.count - 2 && v > a.bounds[seg + 1]) seg++;
        idx = idx * (a.count - 1) + seg;
    }
    out.surface = BIOME_GRID[idx];
    out.dist = dist;
}

uint8_t biomeAtDepth(const ColumnClimate& c, int32_t depth) {
    // the surface entries are at depth 0 and 1
    int64_t d0 = outside(depth, 0, 0), d1 = outside(depth, 10000, 10000);
    int64_t best = c.dist + (d0 < d1 ? d0 : d1);
    uint8_t biome = c.surface;
    for (int k = 0; k < NUM_CAVE_BIOMES; k++) {
        const CaveBiome& e = CAVE_BIOMES[k];
        const int32_t v[6] = {c.p[0], c.p[1], c.p[2], c.p[3], depth, c.p[4]};
        int64_t d = 0;
        for (int i = 0; i < 6 && d < best; i++) d += outside(v[i], e.lo[i], e.hi[i]);
        if (d < best) {
            best = d;
            biome = e.biome;
        }
    }
    return biome;
}

}  // namespace vanilla
}  // namespace mc
