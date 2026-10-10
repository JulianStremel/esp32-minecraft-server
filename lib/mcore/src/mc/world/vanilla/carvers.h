// Vanilla's carvers for the overworld (configured_carver: cave, cave_extra_underground,
// canyon; CaveWorldCarver, CanyonWorldCarver): tunnels and canyons started by the chunks
// up to 8 away, carved into this chunk's block mask after the surface rule, the aquifer
// deciding what fills them. This generator's random numbers, vanilla's shapes.
#pragma once
#include <stdint.h>

namespace mc {
namespace vanilla {

struct AquiferHandle;

struct CarverSteps { uint32_t us, starts, ellipsoids, carved, steps, cells; };
extern CarverSteps carverSteps;   // the last chunk's (the benchmark)

// fastTrig: polynomial sin and cos (generator version 8 on; version 7 keeps newlib's)
void carveChunk(uint64_t seed, int cx, int cz, uint8_t* mask, AquiferHandle* aquifer, bool fastTrig);

}  // namespace vanilla
}  // namespace mc
