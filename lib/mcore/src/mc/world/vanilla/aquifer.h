// Vanilla's aquifers (Aquifer.NoiseBasedAquifer: underground water and lava at their own
// levels, dry caves, barriers between them) and ore veins (OreVeinifier: copper with
// granite, iron with tuff), on the block mask the router makes.
#pragma once
#include <stdint.h>

namespace mc {
namespace vanilla {

class Router;

// The block mask's codes: 0 the global fluid by y (water below the sea, lava below y -54,
// air above), 1 the default block (stone, deepslate, bedrock by y), from 2 a state:
// 2 + an index into SR_STATES (the surface rule's), then these.
enum MaskExtra { AQ_WATER, AQ_LAVA, AQ_AIR, VEIN_COPPER_ORE, VEIN_RAW_COPPER, VEIN_GRANITE, VEIN_IRON_ORE,
                 VEIN_RAW_IRON, VEIN_TUFF, NUM_MASK_EXTRA };
extern const uint16_t MASK_EXTRA_STATES[NUM_MASK_EXTRA];
int maskExtraCode(int extra);        // its mask code
uint16_t maskState(uint8_t code);    // the state of a code from 2

struct AquiferSteps {
    uint32_t statusUs, blocksUs, veinsUs, toggleUs, cave, statuses, barriers, veinBlocks, veinCells, veinTested, ridgeCorners;
};
extern AquiferSteps aquiferSteps;   // the last chunk's (the benchmark)

// Underground (below each column's top) where the router left no block: water, lava, air
// or a barrier of stone, as vanilla's aquifer decides; then the ore veins in the stone.
void fillAquifersAndVeins(Router& r, uint64_t seed, int cx, int cz, uint8_t* mask, const int* tops);

}  // namespace vanilla
}  // namespace mc
