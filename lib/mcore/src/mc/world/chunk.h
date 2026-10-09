// A 16x16 column of sections (24 in the overworld, 16 in the Nether and the End), plus
// heightmap and biome data.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "mc/platform.h"
#include "mc/item.h"
#include "mc/world/section.h"

namespace mc {

constexpr int SEA_LEVEL = 63;

// Dimensions: the world a chunk belongs to (also part of the storage's region key).
enum : uint8_t { DIM_OVERWORLD = 0, DIM_NETHER = 1, DIM_END = 2, NUM_DIMS = 3 };

// The build height per dimension, as vanilla 1.18+: the overworld from -64 to 319,
// the Nether and the End from 0 to 255. Block y values are world y everywhere.
constexpr int MAX_SECTIONS = 24;
constexpr int MIN_WORLD_Y = -64;   // the lowest of any dimension
constexpr int MAX_WORLD_Y = 319;   // the highest of any dimension
constexpr int dimMinY(uint8_t dim) { return dim == DIM_OVERWORLD ? -64 : 0; }
constexpr int dimHeight(uint8_t dim) { return dim == DIM_OVERWORLD ? 384 : 256; }
constexpr int dimMaxY(uint8_t dim) { return dimMinY(dim) + dimHeight(dim) - 1; }   // the highest block y
constexpr int dimSections(uint8_t dim) { return dimHeight(dim) >> 4; }
constexpr bool dimHasY(uint8_t dim, int y) { return y >= dimMinY(dim) && y <= dimMaxY(dim); }
// below this, entities take void damage or are removed (vanilla: 64 under the bottom)
constexpr int dimVoidY(uint8_t dim) { return dimMinY(dim) - 64; }

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
    int16_t y = 0;
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
    uint8_t lx = 0, lz = 0;
    int16_t y = 0;
    int8_t prio = 0;
    uint16_t block = 0;
    int32_t delay = 0;    // ticks after the time it was saved
};

// An entity stored with its chunk: mobs and dropped items (vanilla keeps them with their
// chunk too). The kind is the server's EntityKind; the server fills and reads these.
struct SavedEntity {
    uint8_t kind = 0;
    uint16_t type = 0;          // entity type registry id
    uint8_t variant = 0;        // sheep colour etc.
    uint8_t size = 1;           // magma cubes
    double x = 0, y = 0, z = 0;
    float vx = 0, vy = 0, vz = 0;
    float yaw = 0, pitch = 0;
    float health = 0;
    int16_t fireTicks = 0;
    int16_t pickupDelay = 0;
    uint32_t age = 0;
    ItemStack item;             // dropped items
    static void* operator new[](size_t n) noexcept { return plat::bigAlloc(n); }
    static void operator delete[](void* p) { plat::bigFree(p); }
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

    // Entities kept with the chunk while nobody is near (stashed: not in the game) and
    // stored with it; clone() copies them. hadEntities: the stored copy holds entities
    // (it must be saved again once they are gone).
    SavedEntity* ents = nullptr;
    uint16_t entCount = 0;
    bool hadEntities = false;
    bool addEntity(const SavedEntity& e);   // false: out of memory
    void clearEntities();
    // Entities that are in the game, copied in right before saving (like the ticks) and
    // stored after the stashed ones; cleared after the save.
    SavedEntity* liveEnts = nullptr;
    uint16_t liveCount = 0;
    bool setLiveEntities(const SavedEntity* e, int n);
    void clearLiveEntities();

    // the dimension's build height (see dimMinY)
    int minY() const { return minY_; }
    int maxY() const { return minY_ + (numSections_ << 4) - 1; }
    int numSections() const { return numSections_; }
    bool hasY(int y) const { return y >= minY_ && y <= maxY(); }
    int sectionIndex(int y) const { return (y - minY_) >> 4; }   // y must be in range
    int sectionY(int i) const { return minY_ + (i << 4); }       // the lowest y of section i

    uint16_t get(int lx, int y, int lz) const {
        if (!hasY(y)) return 0;
        const Section* s = sec_[(y - minY_) >> 4];
        return s ? s->get(lx, y & 15, lz) : 0;
    }
    // Returns previous state. Updates heightmap. Does not mark dirty (World does that).
    uint16_t set(int lx, int y, int lz, uint16_t state);

    // Deep copy (blocks, heightmap, biomes, block entities) for background jobs, which
    // must never read a chunk the game loop may modify. nullptr when out of memory.
    Chunk* clone() const;

    // section i covers y from sectionY(i) to sectionY(i) + 15 (index 0 is the lowest)
    Section* section(int i) { return sec_[i]; }
    const Section* section(int i) const { return sec_[i]; }
    Section* ensureSection(int i);
    void dropEmptySections();

    // the highest motion-blocking block's y + 1 (minY() for an empty column)
    int height(int lx, int lz) const { return height_[lx + lz * 16]; }
    void recomputeHeightmap();
    int highestSection() const;  // index of the highest non-empty section, -1 if none

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
    int16_t minY_;
    uint8_t numSections_;
    Section* sec_[MAX_SECTIONS];
    int16_t height_[256];
    uint8_t biome_[16];
};

}  // namespace mc
