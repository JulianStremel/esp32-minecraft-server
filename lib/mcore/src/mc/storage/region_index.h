// Where chunks live in an unbounded world (storage format 3): space is allocated when a
// chunk is first saved, and found through a region directory and per-region slot maps.
//
//   directory  an append-only log of 32-byte entries (CRC each): "region (dim, rx, rz)
//              has its slot map at unit u" and "units below w may be in use"
//              (allocation watermark). Replayed into a hash table in RAM when opened.
//   data area  units of 2 x slotSize (128 KiB), handed out by a bump allocator: a
//              chunk's two copies, or a region's slot map (two 4 KiB copies, A/B with
//              a sequence number and CRC, 1024 entries for 32 x 32 chunks).
//
// Nothing is ever freed. Allocation never passes the persisted watermark (it is logged
// and flushed before units beyond it are used), so a power cut cannot hand the same
// unit out twice. A region missing from the directory, or an empty map entry, means
// "never saved": the chunk is generated without reading the device.
#pragma once
#include <stdint.h>
#include "mc/storage/block_device.h"

namespace mc {

class RegionIndex {
public:
    struct Layout {
        uint64_t dirOff = 0, dirCap = 0;   // directory log
        uint64_t dataOff = 0;              // data area (unit 0)
        uint64_t unit = 2 * 65536;         // bytes per unit (two chunk copies)
    };
    struct Stats {
        uint32_t regions = 0, mapHits = 0, mapMisses = 0, dirEntries = 0;
        uint64_t unitsUsed = 0;
        bool full = false;                 // the export or the directory is full
    };

    RegionIndex() {}
    ~RegionIndex();
    RegionIndex(const RegionIndex&) = delete;
    RegionIndex& operator=(const RegionIndex&) = delete;

    // A new, empty index (clears the start of the directory).
    bool format(BlockDevice* dev, const Layout& layout);
    // Replays the directory. allocHint: the watermark the superblock knew.
    bool open(BlockDevice* dev, const Layout& layout, uint64_t allocHint);
    bool isOpen() const { return dev_ != nullptr; }

    // Loads the slot maps of these chunks' regions that are not cached (one round trip).
    bool prefetch(int n, const uint8_t* dims, const int32_t* cx, const int32_t* cz);
    // Device offset of the chunk's unit, or 0 if it was never saved; false on I/O error.
    bool lookup(uint8_t dim, int32_t cx, int32_t cz, uint64_t& off);
    // The unit to write the chunk to, allocating it (and its region's map) the first
    // time; `isNew`: commit() must follow once the record is written (also when an
    // earlier commit of the region's map failed).
    bool unitForWrite(uint8_t dim, int32_t cx, int32_t cz, uint64_t& off, bool& isNew);
    // Writes the region's map (and its directory entry, the first time) after the
    // chunk's first record.
    bool commit(uint8_t dim, int32_t cx, int32_t cz);

    uint64_t watermark() const { return mark_; }   // units, for the superblock
    uint64_t end() const { return layout_.dataOff + next_ * layout_.unit; }
    const Stats& stats() const { return stats_; }

private:
    struct Dir {   // directory entry in RAM
        int32_t rx, rz;
        uint32_t unit;   // the region map's unit + 1 (0: free slot)
        uint8_t dim;
    };
    struct Map {   // cached slot map
        int32_t rx = 0, rz = 0;
        uint8_t dim = 0;
        bool valid = false;
        bool fresh = false;      // created here, not yet in the directory
        bool dirty = false;      // entries changed since it was last written
        uint32_t unit = 0;       // where it lives
        uint32_t seq = 0;        // of the newest copy on the device
        uint8_t copy = 0;        // which copy that is
        uint32_t lastUse = 0;
        uint32_t entries[1024];  // chunk unit + 1, 0 = never saved
    };
    static const int CACHE = 40;
    static const uint32_t MARK_STEP = 128;   // units per watermark entry (16 MiB)

    Dir* findDir(uint8_t dim, int32_t rx, int32_t rz);
    bool addDir(uint8_t dim, int32_t rx, int32_t rz, uint32_t unit);
    Map* cached(uint8_t dim, int32_t rx, int32_t rz);
    Map* slotFor(uint8_t dim, int32_t rx, int32_t rz);   // an empty or least recently used one
    bool decodeMap(const uint8_t* b, Map& m) const;
    void encodeMap(uint8_t* b, const Map& m, uint32_t seq) const;
    bool appendDir(uint8_t type, uint8_t dim, int32_t rx, int32_t rz, uint64_t value);
    bool alloc(uint32_t& unit);

    BlockDevice* dev_ = nullptr;
    Layout layout_;
    Dir* dir_ = nullptr;
    uint32_t dirCap_ = 0, dirCount_ = 0;
    uint64_t dirLen_ = 0;    // bytes of the log in use
    uint32_t dirSeq_ = 0;
    Map* maps_ = nullptr;    // CACHE entries (PSRAM)
    uint32_t clock_ = 0;
    uint64_t next_ = 0, mark_ = 0;   // next free unit, persisted watermark
    Stats stats_;
};

}  // namespace mc
