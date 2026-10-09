// A 16x16 column of up to 16 sections, plus heightmap and biome data.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "mc/platform.h"
#include "mc/item.h"
#include "mc/world/section.h"

namespace mc {

constexpr int WORLD_HEIGHT = 256;
constexpr int NUM_SECTIONS = 16;
constexpr int SEA_LEVEL = 63;

// Dimensions: the world a chunk belongs to (also part of the storage's region key).
enum : uint8_t { DIM_OVERWORLD = 0, DIM_NETHER = 1, DIM_END = 2, NUM_DIMS = 3 };

inline int floorDiv(int a, int b) { int q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }
inline int chunkCoord(int w) { return w >> 4; }

bool isMotionBlocking(uint16_t state);   // solid or fluid: counts for the heightmap

enum TileType : uint8_t { TILE_NONE = 0, TILE_CHEST = 1, TILE_FURNACE = 2, TILE_SIGN = 3, TILE_BARREL = 4, TILE_COMPARATOR = 5, TILE_PISTON = 6,
    TILE_HOPPER = 7, TILE_DROPPER = 8, TILE_DISPENSER = 9, TILE_DAYLIGHT = 10, TILE_LECTERN = 11,
    TILE_BOOKSHELF = 12, TILE_CRAFTER = 13, TILE_SCULK = 14 };

// Block entity data the server keeps (container contents, furnace progress, sign text).
struct TileEntity {
    TileEntity* next = nullptr;
    uint8_t type = TILE_NONE;
    uint8_t lx = 0, lz = 0;
    uint8_t y = 0;
    ItemStack items[27];          // chest/barrel: 27 slots; furnace: 0 input, 1 fuel, 2 output
    int16_t burnTime = 0, burnTotal = 0, cookTime = 0;   // furnace state as of tick `updated`
    uint32_t updated = 0;         // furnace: world age its state was brought up to date (not saved)
    uint8_t signal = 0;          // comparator output (independent of powered property)
    int16_t transferCooldown = -1;
    uint16_t movedState = 0;
    uint8_t pistonFace = 0, pistonProgress = 0, pistonPrevious = 0; // progress in half-block steps
    bool pistonExtending = false, pistonSource = false;
    int32_t bookPage = 0;
    int8_t lastSlot = -1;          // chiseled bookshelf: the slot last used (comparator: + 1)
    uint16_t disabledSlots = 0;    // crafter: slot bits the player turned off
    bool craftPending = false;     // crafter: a craft is scheduled (else a tick ends the crafting look)
    uint8_t frequency = 0;         // sculk sensor: the last vibration's frequency (comparator)
    uint8_t pendingFrequency = 0;  // sculk sensor: a vibration on its way (0: none), its strength
    uint8_t pendingStrength = 0;
    uint64_t tickOrder = 0; // live block-entity insertion order, assigned again after loading
    uint64_t daylightVersions[9] = {}; // light-only chunk versions, including residency
    uint8_t daylightSky = 0;
    bool daylightValid = false;
    char text[4][64];             // sign lines (plain text)

    TileEntity() { for (auto& l : text) l[0] = 0; }
    static void* operator new(size_t n) noexcept { return plat::bigAlloc(n); }
    static void operator delete(void* p) { plat::bigFree(p); }
    int slotCount() const {
        if (type == TILE_LECTERN) return 1;
        if (type == TILE_FURNACE) return 3;
        if (type == TILE_HOPPER) return 5;
        if (type == TILE_DROPPER || type == TILE_DISPENSER || type == TILE_CRAFTER) return 9;
        if (type == TILE_BOOKSHELF) return 6;
        if (type == TILE_SIGN || type == TILE_COMPARATOR || type == TILE_PISTON || type == TILE_DAYLIGHT ||
            type == TILE_SCULK)
            return 0;
        return 27;
    }
};

// A scheduled block tick kept in a stored chunk (vanilla's TileTicks).
struct ChunkTick {
    uint8_t lx = 0, lz = 0, y = 0;
    int8_t prio = 0;
    uint16_t block = 0;
    int32_t delay = 0;    // ticks after the time it was saved
};

class Chunk;

// An immutable copy of a chunk shared by background jobs (light, spawning, path
// finding): jobs that need the same chunk in the same version share one copy. References
// are taken and released on the game loop only (jobs release theirs when they are
// deleted, after finish()); workers only read the copy.
struct ChunkSnap {
    Chunk* chunk = nullptr;   // the copy
    uint32_t version = 0;     // the live chunk's version when it was taken
    uint16_t refs = 0;
    ~ChunkSnap();
    void retain() { refs++; }
    void release() { if (--refs == 0) delete this; }
};

class Chunk {
public:
    Chunk(int32_t cx, int32_t cz, uint8_t dim = DIM_OVERWORLD);
    ~Chunk();
    Chunk(const Chunk&) = delete;
    // world data lives in PSRAM on the ESP32
    static void* operator new(size_t n) noexcept { return plat::bigAlloc(n); }
    static void operator delete(void* p) { plat::bigFree(p); }
    Chunk& operator=(const Chunk&) = delete;

    const int32_t cx, cz;
    const uint8_t dim;
    bool dirty = false;        // modified since last save
    bool lightDirty = true;    // light data must be recomputed before it is sent again
    bool readOnly = false;     // storage failed to load it: never overwrite the stored copy
    uint32_t storeSeq = 0;     // sequence number of the newest stored copy (0 = never stored)
    int8_t storeSlot = -1;     // which of the two storage slots holds that copy
    uint32_t lastUse = 0;
    uint32_t version = 0;      // bumps on every block change (clients resend based on it)
    uint32_t skyVersion = 0;   // changes only when skylight filtering changes
    uint32_t residency = 0;    // distinguishes reloaded chunks in derived-data caches
    uint8_t jobRefs = 0;       // background jobs working on a snapshot of it (never evicted then)
    bool saving = false;       // a background save is in flight
    bool lightPartial = false; // sent with per-chunk light near a player: resend when the neighbours are there
    ChunkSnap* snap = nullptr; // latest shared snapshot (World::snapshot), holds one reference

    // Scheduled block ticks travelling with a stored copy: attached right before saving
    // and filled by loading, then moved into the server's timer wheel. clone() does not
    // copy them.
    ChunkTick* ticks = nullptr;
    uint16_t tickCount = 0;
    bool setTicks(const ChunkTick* t, int n);   // false: out of memory
    void clearTicks();

    uint16_t get(int lx, int y, int lz) const {
        if (y < 0 || y >= WORLD_HEIGHT) return 0;
        const Section* s = sec_[y >> 4];
        return s ? s->get(lx, y, lz) : 0;
    }
    // Returns previous state. Updates heightmap. Does not mark dirty (World does that).
    uint16_t set(int lx, int y, int lz, uint16_t state);

    // Deep copy (blocks, heightmap, biomes, block entities) for background jobs, which
    // must never read a chunk the game loop may modify. nullptr when out of memory.
    Chunk* clone() const;

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
    int movingPistons() const { return movingPistons_; }
    int tickingBlockEntities() const { return movingPistons_ + hoppers_ + daylights_; }
    int sculkSensors() const { return sculks_; }

private:
    TileEntity* tiles_ = nullptr;
    int movingPistons_ = 0;
    int hoppers_ = 0;
    int sculks_ = 0;
    int daylights_ = 0;
    Section* sec_[NUM_SECTIONS];
    uint16_t height_[256];
    uint8_t biome_[16];
};

}  // namespace mc
