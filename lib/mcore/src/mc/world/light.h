// Per-chunk light computation used when a chunk is sent to clients.
// Sky light: straight-down full light through transparent blocks, then a
// flood fill (BFS) into shaded areas; block light: flood fill from emitters.
// Only sections up to one above the highest non-empty section get data; the
// client treats everything above as full sky light.
//
// Two modes:
//  - per chunk (compute): the chunk alone, plus sky light entering from the open-sky
//    columns of the neighbours' borders (NeighbourEdges). Fast; light from the
//    neighbours' blocks (torches, overhangs) stops at the border.
//  - region (computeRegion): the chunk and the blocks of its 8 neighbours within 14
//    blocks of the border (light from further away is spent before it arrives), so light
//    crosses chunk borders exactly as in a whole-world computation.
// Both work on a byte grid decoded once per section (filter in the low nibble, light in
// the high nibble), not on per-block palette lookups. Sky light first falls straight down
// each column; a flood fill only spreads it sideways where that adds light.
#pragma once
#include <stdint.h>
#include "mc/world/chunk.h"

namespace mc {

class World;

// The only thing the per-chunk mode needs from the four neighbouring chunks: the
// heightmap along the shared border (sky light that enters sideways).
struct NeighbourEdges {
    bool present[4] = {false, false, false, false};   // -x, +x, -z, +z
    uint16_t height[4][16];                            // neighbour's border column heights
    // Snapshot from the resident neighbours of (cx, cz); does not touch the LRU order.
    void gather(const World& world, int cx, int cz);
};

class ChunkLight {
public:
    // How far light reaches into a neighbour: the margin of the region mode.
    static constexpr int MARGIN = 14;   // a cell 15 steps away cannot add any light
    static constexpr int REGION_W = 16 + 2 * MARGIN;

    ChunkLight() {}
    ~ChunkLight();
    ChunkLight(const ChunkLight&) = delete;
    ChunkLight& operator=(const ChunkLight&) = delete;
    // Computes light for chunk c. Neighbouring chunks (if resident in `world`) are used
    // to seed light that enters through the chunk borders. Returns false on OOM.
    bool compute(const Chunk& c, World* world);
    // Same with a snapshot of the neighbours' borders (safe on worker threads).
    bool compute(const Chunk& c, const NeighbourEdges& edges) { return computeChunk(c, &edges); }
    // Exact light of nine[4] from it and its 8 neighbours, nine[(dz + 1) * 3 + (dx + 1)]
    // (all present). Safe on worker threads with snapshots. false on OOM.
    bool computeRegion(const Chunk* const nine[9]);

    int sections() const { return numSections_; }      // sections 0 .. numSections_-1 have data
    const uint8_t* sky(int s) const { return sky_ + (size_t)s * 2048; }
    const uint8_t* block(int s) const { return block_ + (size_t)s * 2048; }
    bool blockSectionEmpty(int s) const { return !(blockNonZero_ & (1u << s)); }
    // Light of the computed chunk at local (x, y, z); above the computed sections the
    // sky is open and there is no block light.
    int skyAt(int x, int y, int z) const { return nibbleAt(sky_, x, y, z, 15); }
    int blockAt(int x, int y, int z) const { return nibbleAt(block_, x, y, z, 0); }

    // Time of the last computation's phases (for the benchmark): fill, sky, block, output.
    enum { PH_FILL, PH_SKY, PH_BLOCK, PH_OUT, PH_DIRECT, PHASES };   // PH_DIRECT: part of PH_SKY
    uint32_t phaseUs[PHASES] = {};
    uint32_t skyPushes = 0, blockPushes = 0;   // flood queue entries of the last computation

private:
    int nibbleAt(const uint8_t* a, int x, int y, int z, int above) const {
        if (y < 0) return 0;
        if (y >= numSections_ * 16) return above;
        uint32_t i = ((uint32_t)y << 8) | ((uint32_t)z << 4) | (uint32_t)x;
        return (a[i >> 1] >> ((i & 1) * 4)) & 15;
    }
    bool computeChunk(const Chunk& c, const NeighbourEdges* edges);
    // the shared engine: a W x W x H grid whose centre chunk starts at (off, off)
    bool reserve(int W, int H, int outSections);
    bool fillFrom(const Chunk& c, int ox, int oz, int x0, int x1, int z0, int z1, uint16_t skipSections);
    bool run(const NeighbourEdges* edges);   // block light, then sky light, into the outputs
    void findDirect();
    void skyPass(const NeighbourEdges* edges);
    void blockPass();
    void clearTouched();
    void copyOut(uint8_t* dst, bool sky);
    bool pushEmitter(uint32_t i, int level);

    uint8_t* sky_ = nullptr;      // output: centre chunk, nibbles per section
    uint8_t* block_ = nullptr;
    int numSections_ = 0;
    int capSections_ = 0;
    uint32_t blockNonZero_ = 0;

    // PSRAM: the grid and the flood queue
    uint8_t* cells_ = nullptr;    // grid: filter (low nibble) | light << 4
    size_t cellCap_ = 0;
    uint32_t* queue_ = nullptr;
    uint32_t qcap_ = 0;
    uint32_t* emit_ = nullptr;    // emitter cells: packed position << 4 | level
    uint32_t emitN_ = 0, emitCap_ = 0;
    uint32_t* touched_ = nullptr; // cells the block light pass lit (cleared before the sky pass)
    uint32_t touchedN_ = 0, touchedCap_ = 0;
    bool touchedOver_ = false;
    // internal RAM (small, hot)
    uint8_t* tmp_ = nullptr;      // one decoded section
    int16_t* direct_ = nullptr;   // per column: lowest y with direct sky light (H if none);
                                  // cells from there up are not stored, they are 15
    uint8_t* dist_ = nullptr;     // per column: steps to the centre chunk (0 inside it)
    uint8_t* fall_ = nullptr;     // per column: sky light falling down it (vertical pass)
    int W_ = 0, H_ = 0, off_ = 0;
};

}  // namespace mc
