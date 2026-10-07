// A 16x16 column of up to 16 sections, plus heightmap and biome data.
#pragma once
#include <stdint.h>
#include "mc/item.h"
#include "mc/world/section.h"

namespace mc {

constexpr int WORLD_HEIGHT = 256;
constexpr int NUM_SECTIONS = 16;
constexpr int SEA_LEVEL = 63;

inline int floorDiv(int a, int b) { int q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }
inline int chunkCoord(int w) { return w >> 4; }

bool isMotionBlocking(uint16_t state);   // solid or fluid: counts for the heightmap

enum TileType : uint8_t { TILE_NONE = 0, TILE_CHEST = 1, TILE_FURNACE = 2, TILE_SIGN = 3, TILE_BARREL = 4 };

// Block entity data the server keeps (container contents, furnace progress, sign text).
struct TileEntity {
    TileEntity* next = nullptr;
    uint8_t type = TILE_NONE;
    uint8_t lx = 0, lz = 0;
    uint8_t y = 0;
    ItemStack items[27];          // chest/barrel: 27 slots; furnace: 0 input, 1 fuel, 2 output
    int16_t burnTime = 0, burnTotal = 0, cookTime = 0;
    char text[4][64];             // sign lines (plain text)

    TileEntity() { for (auto& l : text) l[0] = 0; }
    int slotCount() const { return type == TILE_FURNACE ? 3 : (type == TILE_SIGN ? 0 : 27); }
};

class Chunk {
public:
    Chunk(int32_t cx, int32_t cz);
    ~Chunk();
    Chunk(const Chunk&) = delete;
    Chunk& operator=(const Chunk&) = delete;

    const int32_t cx, cz;
    bool dirty = false;        // modified since last save
    bool lightDirty = true;    // light data must be recomputed before it is sent again
    bool readOnly = false;     // storage failed to load it: never overwrite the stored copy
    uint32_t storeSeq = 0;     // sequence number of the newest stored copy (0 = never stored)
    int8_t storeSlot = -1;     // which of the two storage slots holds that copy
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

    // block entities
    TileEntity* tiles() const { return tiles_; }
    TileEntity* tileAt(int lx, int y, int lz) const;
    TileEntity* addTile(uint8_t type, int lx, int y, int lz);   // replaces an existing one
    void removeTile(int lx, int y, int lz);
    int tileCount() const;

private:
    TileEntity* tiles_ = nullptr;
    Section* sec_[NUM_SECTIONS];
    uint16_t height_[256];
    uint8_t biome_[16];
};

}  // namespace mc
