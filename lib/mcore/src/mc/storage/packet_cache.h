// Ready-to-send chunk packets of chunks nobody changed, kept on a device of their own
// (on the ESP32 a second file on the SD card): streaming such a chunk again is two small
// reads instead of generating it, lighting it, encoding and compressing it.
//
// A cache, not storage: anything in it can be made again from the seed, so it holds one
// copy, overwrites its oldest entries when it is full, and a lost entry is a miss.
//
// Layout (all integers little-endian, offsets in 512-byte sectors):
//   0                header: magic, format, the world it belongs to (an epoch: entries
//                    of another world or compression threshold are ignored), its sizes,
//                    where the ring writes next
//   1 .. 1+I         index: I sectors of 32 entries {key, epoch, record sector, length},
//                    a key's entry in the sector its hash picks (full: one is replaced)
//   1+I .. end       ring of records, each from a sector boundary: {magic, length, key,
//                    epoch, crc} and the packet's bytes; overwritten records fail their
//                    key or crc check when read
// Only the storage I/O thread calls it.
#pragma once
#include <stdint.h>
#include "mc/storage/block_device.h"

namespace mc {

struct PacketCacheStats {
    uint32_t hits = 0, misses = 0, stale = 0, puts = 0, putErrors = 0, readErrors = 0;
    uint64_t bytesPut = 0, bytesRead = 0;
};

class PacketCache {
public:
    static uint64_t key(uint8_t dim, int cx, int cz) {
        return (uint64_t)dim << 56 | (uint64_t)((uint32_t)cx & 0xFFFFFFFu) << 28 | ((uint32_t)cz & 0xFFFFFFFu);
    }
    // a chunk's whole record (the world store's encoding: its chests, entities, ticks), for
    // making it resident again: {raw size u32, flags u16, the record's bytes}
    static uint64_t recordKey(uint8_t dim, int cx, int cz) { return key(dim, cx, cz) | 1ull << 62; }
    // world: the world and its settings the packets depend on (seed, generator version,
    // world type, compression threshold); another value starts a new epoch (everything
    // before is ignored, nothing is erased)
    bool open(BlockDevice* dev, uint64_t world);
    bool isOpen() const { return dev_ != nullptr; }
    // the packet of key: true and its bytes in buf (up to cap; len: its length), false if
    // it is not there (or too large for buf)
    bool get(uint64_t key, uint8_t* buf, uint32_t cap, uint32_t& len);
    bool put(uint64_t key, const uint8_t* data, uint32_t len);
    // whether key has an entry (its index only: the record itself is checked by get)
    bool has(uint64_t key);
    bool flush();   // the header's write position (entries written since are found anyway)
    const PacketCacheStats& stats() const { return stats_; }
    uint64_t capacityBytes() const { return (uint64_t)dataSectors_ * 512; }
    uint32_t entries() const { return indexSectors_ * 32; }
    void statusLine(char* buf, size_t cap) const;

    static const uint32_t MAX_PACKET = 64 * 1024;

private:
    struct Entry {   // 16 bytes
        uint64_t key;
        uint32_t sector;    // from the ring's start
        uint16_t sectors;   // the record's length in sectors
        uint16_t epoch;
    };
    bool writeHeader();
    uint32_t indexSector(uint64_t key) const;
    BlockDevice* dev_ = nullptr;
    uint32_t indexSectors_ = 0, dataSectors_ = 0, writePos_ = 0;
    uint16_t epoch_ = 1;
    uint64_t world_ = 0;
    uint32_t putsSinceHeader_ = 0;
    PacketCacheStats stats_;
    uint8_t sector_[512];   // an index sector being read or updated
};

}  // namespace mc
