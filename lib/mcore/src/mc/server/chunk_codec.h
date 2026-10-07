// Chunk Data and Update Light packet encoders. They only read the chunk (and the
// computed light), so they can run on any thread that owns a stable chunk.
#pragma once
#include "mc/io.h"
#include "mc/world/chunk.h"
#include "mc/world/light.h"

namespace mc {

// Writes the whole Chunk Data packet (id included).
void writeChunkPacket(Writer& w, const Chunk& c);
// Writes the Update Light packet (id included). full: also send fully lit data for
// the sections above the computed range, which overwrites stale client data after
// the terrain got lower.
void writeLightPacket(Writer& w, const Chunk& c, const ChunkLight& L, bool full);

}  // namespace mc
