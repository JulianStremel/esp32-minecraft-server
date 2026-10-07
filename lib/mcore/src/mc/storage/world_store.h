// The on-device world format, stored on any BlockDevice (typically an NBD export).
//
// Layout (all offsets in bytes, integers big-endian):
//   0      superblock copy A (512)      \  alternating writes with a sequence
//   512    superblock copy B (512)      /  number; the valid newest one wins
//   4096   player table: playerSlots x 512 (hashed by UUID, linear probing)
//   chunk area (64 KiB aligned): one slot pair per chunk inside the world border,
//          index = (cz + R) * 2R + (cx + R), each pair = 2 x slotSize
//
// Chunks are written to the older of their two slots, so an interrupted write
// (power loss on the ESP32) never destroys the last good copy. Records carry a
// CRC32 and are zlib compressed. Chunks that were never modified are not stored
// at all: they are regenerated from the seed. Unused space stays sparse on the
// server side, so a file-backed export only consumes what the world uses.
#pragma once
#include <stdint.h>
#include "mc/storage/block_device.h"
#include "mc/storage/storage.h"

namespace mc {

struct StoreParams {
    int radius = 64;                 // world border radius in chunks (shrunk to fit the device)
    uint32_t chunkSlotSize = 65536;  // bytes per chunk copy
    uint32_t playerSlots = 1024;
    bool compress = true;            // zlib-compress chunk records
};

class WorldStore : public Storage {
public:
    explicit WorldStore(BlockDevice* dev);

    // Reads the superblock; formats the device if it holds no world (and allowFormat).
    // Returns false on I/O errors or when the device is too small.
    bool open(const StoreParams& params, bool allowFormat = true);
    bool isOpen() const { return open_; }
    int radius() const { return radius_; }
    BlockDevice* device() { return dev_; }

    // Storage
    bool loadMeta(WorldMeta& m) override;
    bool saveMeta(const WorldMeta& m) override;
    bool loadPlayer(const uint8_t uuid[16], PlayerData& out) override;
    bool savePlayer(const PlayerData& p) override;
    bool flush() override;
    void statusLine(char* buf, size_t cap) override;
    int worldRadius() const override { return open_ ? radius_ : -1; }

    // ChunkStore
    LoadResult loadChunk(Chunk& c) override;
    bool saveChunk(Chunk& c) override;
    bool chunkInRange(int cx, int cz) const override;

    uint32_t chunksWritten() const { return chunksWritten_; }
    uint64_t requiredSize() const;

private:
    bool readSuper();
    bool writeSuper();
    uint64_t chunkBase(int cx, int cz) const;

    BlockDevice* dev_;
    bool open_ = false;
    bool compress_ = true;
    bool haveWorld_ = false;
    int radius_ = 0;
    uint32_t slotSize_ = 65536;
    uint32_t playerSlots_ = 1024;
    uint64_t playerOff_ = 4096;
    uint64_t chunkOff_ = 0;
    uint32_t superSeq_ = 0;
    WorldMeta meta_;
    uint32_t chunksWritten_ = 0, chunksRead_ = 0, playersWritten_ = 0;
};

}  // namespace mc
