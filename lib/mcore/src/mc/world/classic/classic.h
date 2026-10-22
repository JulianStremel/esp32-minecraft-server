// The classic generator's own structures (generator version 100 and up): not vanilla's,
// freely interpreted, with vanilla's templates among their parts. Each a pure function of
// the world seed and the chunk, the same on every platform.
#pragma once
#include <stdint.h>
#include "mc/world/chunk.h"

namespace mc {
struct ColumnInfo;
namespace classic {

// Dungeons: rooms under ground where 1 to 5 openings lead into caves (vanilla's monster
// room rule), a spawner and chests; one in five a crypt of stone bricks and cobwebs.
// After the caves and the ores; inside the chunk.
void placeDungeons(Chunk& c, uint64_t seed, const ColumnInfo* cols);
// Ruins on the surface: a tower's stump, walls or a ring of stones in the biome's
// material, rubble around, sometimes a chest under the floor. Before the trees.
void placeRuins(Chunk& c, uint64_t seed, const ColumnInfo* cols);

}  // namespace classic
}  // namespace mc
