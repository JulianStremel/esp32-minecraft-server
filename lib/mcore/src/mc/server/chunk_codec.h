// Chunk Data and Update Light packet encoders. They only read the chunk (and the
// computed light), so they can run on any thread that owns a stable chunk.
#pragma once
#include "mc/io.h"
#include "mc/world/chunk.h"
#include "mc/world/light.h"

namespace mc {

// Writes the whole Chunk Data packet (id included), with its light.
void writeChunkPacket(Writer& w, const Chunk& c, const ChunkLight& L);
// Writes the Update Light packet (id included). full: also send fully lit data for
// the sections above the computed range, which overwrites stale client data after
// the terrain got lower.
// A sign's block entity NBT (front text, the back empty), as in chunks and updates.
void writeSignNbt(Writer& w, const TileEntity& t);
void writeLightPacket(Writer& w, const Chunk& c, const ChunkLight& L, bool full);

}  // namespace mc
