// A 16x16 column of up to 16 sections, plus heightmap and biome data.
#pragma once
#include <stdint.h>
#include "mc/world/section.h"

namespace mc {

constexpr int WORLD_HEIGHT = 256;
constexpr int NUM_SECTIONS = 16;
constexpr int SEA_LEVEL = 63;

inline int floorDiv(int a, int b) { int q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }
inline int chunkCoord(int w) { return w >> 4; }

bool isMotionBlocking(uint16_t state);   // solid or fluid: counts for the heightmap

class Chunk {
public:
    Chunk(int32_t cx, int32_t cz);
    ~Chunk();
    Chunk(const Chunk&) = delete;
    Chunk& operator=(const Chunk&) = delete;

    const int32_t cx, cz;
    bool dirty = false;        // modified since last save
    bool lightDirty = true;    // light data must be recomputed before it is sent again
    uint32_t lastUse = 0;
    uint32_t version = 0;      // bumps on every block change (clients resend based on it)

    uint16_t get(int lx, int y, int lz) const {
        if (y < 0 || y >= WORLD_HEIGHT) return 0;
        const Section* s = sec_[y >> 4];
        return s ? s->get(lx, y, lz) : 0;
    }
    // Returns previous state. Updates heightmap. Does not mark dirty (World does that).
    uint16_t set(int lx, int y, int lz, uint16_t state);

    Section* section(int i) { return sec_[i]; }
    const Section* section(int i) const { return sec_[i]; }
    Section* ensureSection(int i);
    void dropEmptySections();

    int height(int lx, int lz) const { return height_[lx + lz * 16]; }  // highest motion-blocking y + 1
    void recomputeHeightmap();
    int highestSection() const;  // index of highest non-empty section, -1 if none

    uint8_t biome(int lx, int lz) const { return biome_[(lz >> 2) * 4 + (lx >> 2)]; }
    void setBiomeCell(int cx4, int cz4, uint8_t b) { biome_[cz4 * 4 + cx4] = b; }
    void setBiomeAll(uint8_t b) { for (int i = 0; i < 16; i++) biome_[i] = b; }
    const uint8_t* biomeCells() const { return biome_; }

    size_t memoryBytes() const;

private:
    Section* sec_[NUM_SECTIONS];
    uint16_t height_[256];
    uint8_t biome_[16];
};

}  // namespace mc
