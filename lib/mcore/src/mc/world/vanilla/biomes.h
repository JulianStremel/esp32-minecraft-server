// Vanilla's overworld biomes from the climate (Climate.ParameterList#findValue over
// OverworldBiomeBuilder's points), compiled by tools/worldgen/compile_biomes.js into a grid
// of the surface biomes (biome_table.cpp) and the three cave biomes.
#pragma once
#include <stdint.h>

namespace mc {
namespace vanilla {

struct BiomeAxis { const int32_t* bounds; int count; };   // sorted, quantized
struct CaveBiome { int32_t lo[6], hi[6]; uint8_t biome; };   // temperature, humidity, continentalness, erosion, depth, weirdness

extern const BiomeAxis BIOME_AXES[5];   // temperature, humidity, continentalness, erosion, weirdness
extern const uint8_t BIOME_GRID[];
extern const CaveBiome CAVE_BIOMES[];
extern const int NUM_CAVE_BIOMES;

// Climate.quantizeCoord
inline int32_t quantizeClimate(float v) { return (int32_t)(v * 10000.0f); }

// A column's climate without depth (it is the same at every y): its surface biome and the
// squared distance to it.
struct ColumnClimate {
    int32_t p[5];   // temperature, humidity, continentalness, erosion, weirdness (quantized)
    uint8_t surface;
    int64_t dist;
};
void columnClimate(const float climate[5], ColumnClimate& out);

// The biome at a depth (the router's depth, quantized) in that column: the surface biome
// (vanilla lists it at depths 0 and 1) or a cave biome if one is nearer.
uint8_t biomeAtDepth(const ColumnClimate& c, int32_t depth);

}  // namespace vanilla
}  // namespace mc
