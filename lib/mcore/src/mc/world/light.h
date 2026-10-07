// Per-chunk light computation used when a chunk is sent to clients.
// Sky light: straight-down full light through transparent blocks, then a
// flood fill (BFS) into shaded areas; block light: flood fill from emitters.
// Only sections up to one above the highest non-empty section get data; the
// client treats everything above as full sky light.
#pragma once
#include <stdint.h>
#include "mc/world/chunk.h"

namespace mc {

class World;

class ChunkLight {
public:
    ChunkLight() {}
    ~ChunkLight();
    // Computes light for chunk c. Neighbouring chunks (if resident in `world`) are used
    // to seed light that enters through the chunk borders. Returns false on OOM.
    bool compute(const Chunk& c, World* world);

    int sections() const { return numSections_; }      // sections 0 .. numSections_-1 have data
    const uint8_t* sky(int s) const { return sky_ + (size_t)s * 2048; }
    const uint8_t* block(int s) const { return block_ + (size_t)s * 2048; }
    bool blockSectionEmpty(int s) const { return !(blockNonZero_ & (1u << s)); }

private:
    uint8_t* sky_ = nullptr;
    uint8_t* block_ = nullptr;
    uint32_t* queue_ = nullptr;
    uint32_t qcap_ = 0;
    int numSections_ = 0;
    int capSections_ = 0;
    uint32_t blockNonZero_ = 0;
};

}  // namespace mc
