// The on-device world format, stored on any BlockDevice (typically an NBD export).
//
// Layout of format 4 (all offsets in bytes, integers big-endian):
//   0      superblock copy A (512)      \  alternating writes with a sequence
//   512    superblock copy B (512)      /  number; the valid newest one wins
//   1024   the world's extra state (portals, the dragon fight): two copies of 1536
//   4096   player table: playerSlots x 1024 (hashed by UUID, linear probing);
//          a 512-byte record plus two 256-byte descriptors pointing to
//          variable-size payloads (player fields and item NBT) in the data area.
//   region directory and data area (RegionIndex): a chunk gets a slot pair (2 x
//          slotSize) when it is first saved, so the world border does not depend on
//          the size of the export.
// Older formats (1 and 2 "dense", 3 without item NBT) are not converted: opening such a
// world logs it and starts a new world in its place.
//
// Chunks are written to the older of their two slots, so an interrupted write
// (power loss on the ESP32) never destroys the last good copy. Records carry a
// CRC32 and are zlib compressed. Chunks that were never modified are not stored
// at all: they are regenerated from the seed. Unused space stays sparse on the
// server side, so a file-backed export only consumes what the world uses.
#pragma once
#include <stdint.h>
#include "mc/storage/block_device.h"
#include "mc/storage/region_index.h"
#include "mc/storage/storage.h"

namespace mc {

struct StoreParams {
    int radius = 64;                 // world border radius in chunks
    uint32_t chunkSlotSize = 65536;  // bytes per chunk copy
    uint32_t playerSlots = 1024;
    bool compress = true;            // zlib-compress chunk records
};

class WorldStore : public Storage {
public:
    explicit WorldStore(BlockDevice* dev);

    // Reads the superblock; formats the device if it holds no world (and allowFormat),
    // or a world in an older format (always: worlds are not converted).
    // Returns false on I/O errors or when the device is too small.
    bool open(const StoreParams& params, bool allowFormat = true);
    bool resetWorld(const WorldMeta& fresh) override;
    bool isOpen() const { return open_; }
    int radius() const { return radius_; }
    BlockDevice* device() { return dev_; }

    // Storage
    bool loadMeta(WorldMeta& m) override;
    bool saveMeta(const WorldMeta& m) override;
    bool loadPlayer(const uint8_t uuid[16], PlayerData& out) override;
    LoadResult fetchPlayer(const uint8_t uuid[16], PlayerData& out) override;
    bool savePlayer(const PlayerData& p) override;
    bool flush() override;
    bool flushLater() override { return open_ && dev_->flushLater(); }
    void statusLine(char* buf, size_t cap) override;
    int worldRadius() const override { return open_ ? radius_ : -1; }
    int format() const { return format_; }   // 4
    const RegionIndex& index() const { return index_; }

    // ChunkStore
    LoadResult loadChunk(Chunk& c) override;
    bool saveChunk(Chunk& c) override;
    bool chunkInRange(int cx, int cz) const override;
    bool splitIo() const override { return open_; }
    LoadResult fetchChunk(uint8_t dim, int cx, int cz, ChunkRecord& rec) override;
    void fetchChunks(int n, const uint8_t* dims, const int32_t* cx, const int32_t* cz, ChunkRecord* const* recs,
                     LoadResult* res) override;
    bool decodeChunk(const ChunkRecord& rec, Chunk& c) const override;
    bool encodeChunk(const Chunk& c, ChunkRecord& rec, uint8_t* deflateWs) const override;
    bool writeChunk(Chunk& c, const ChunkRecord& rec) override;

    uint32_t chunksWritten() const { return chunksWritten_; }
    uint64_t requiredSize() const;

private:
    struct ChunkHeaderInfo {
        bool valid;
        uint32_t seq, stored, raw, crc;
        uint16_t flags;
    };
    int readHeaders(int cx, int cz, uint64_t base, ChunkHeaderInfo h[2], int order[2]);
    // Where (cx, cz)'s slot pair is: `base` is 0 when it was never saved. false on I/O errors.
    bool findChunk(uint8_t dim, int cx, int cz, uint64_t& base);
    // Where to write it (allocating its slot pair); `commit`: RegionIndex::commit after.
    bool chunkForWrite(uint8_t dim, int cx, int cz, uint64_t& base, bool& commit);
    bool formatRegions(const StoreParams& params);
    bool zeroPlayerTable();
    StoreParams params_;
    int parseHeaders(int cx, int cz, const uint8_t* hb0, const uint8_t* hb1, ChunkHeaderInfo h[2], int order[2]) const;
    bool readSuper(uint64_t& allocHint, int& oldFormat);
    bool writeSuper();

    BlockDevice* dev_;
    bool open_ = false;
    bool compress_ = true;
    bool haveWorld_ = false;
    int radius_ = 0;
    uint32_t slotSize_ = 65536;
    uint32_t playerSlots_ = 1024;
    uint64_t playerOff_ = 4096;
    uint32_t playerStride_ = 1024;
    int format_ = 5;
    RegionIndex::Layout layout_;
    RegionIndex index_;
    uint32_t superSeq_ = 0;
    uint32_t extraSeq_ = 0;
    uint32_t extraCrc_ = 0;   // of the copy last written or read (skip writing it again)
    bool readExtra(WorldMeta& m);
    bool writeExtra(const WorldMeta& m);
    WorldMeta meta_;
    // where recently seen players live in the player table (saves then need no lookup)
    struct SlotCacheEntry {
        uint8_t uuid[16];
        int32_t slot;
    };
    static const int SLOT_CACHE = 16;
    SlotCacheEntry slotCache_[SLOT_CACHE];
    int slotCacheN_ = 0, slotCacheNext_ = 0;
    int cachedSlot(const uint8_t uuid[16]) const;
    void cacheSlot(const uint8_t uuid[16], int slot);
    uint32_t chunksWritten_ = 0, chunksRead_ = 0, playersWritten_ = 0;
};

}  // namespace mc
