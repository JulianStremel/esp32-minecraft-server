#include "mc/storage/packet_cache.h"
#include <stdio.h>
#include <string.h>

namespace mc {

namespace {
const uint32_t MAGIC = 0x4350434Du;          // "MCPC"
const uint32_t RECORD_MAGIC = 0x5250434Du;   // "MCPR"
const uint32_t FORMAT = 1;
const uint32_t RECORD_HEADER = 24;           // magic, length, key, epoch, pad, crc

void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
void put64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
uint32_t get32(const uint8_t* p) { uint32_t v = 0; for (int i = 3; i >= 0; i--) v = v << 8 | p[i]; return v; }
uint64_t get64(const uint8_t* p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v; }
uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

uint64_t mix(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
}  // namespace

bool PacketCache::open(BlockDevice* dev, uint64_t world) {
    dev_ = nullptr;
    if (!dev) return false;
    uint64_t total = dev->size() / 512;
    if (total < 1024) return false;   // under 512 KiB: not worth it
    if (total > 0xFFFFFFFFull) total = 0xFFFFFFFFull;
    if (!dev->read(0, sector_, 512)) return false;
    bool known = get32(sector_) == MAGIC && get32(sector_ + 4) == FORMAT &&
                 get32(sector_ + 24) == crc32(sector_, 24);
    if (known) {
        indexSectors_ = get32(sector_ + 8);
        dataSectors_ = get32(sector_ + 12);
        writePos_ = get32(sector_ + 16);
        epoch_ = get16(sector_ + 20);
        // the world it holds (written right after the header)
        world_ = get64(sector_ + 32);
        if ((uint64_t)1 + indexSectors_ + dataSectors_ > total || writePos_ >= dataSectors_) known = false;
    }
    if (!known) {   // a blank (or foreign) device: the layout, a first epoch
        // about one index entry for every 5 data sectors (a record takes 5 to 20)
        indexSectors_ = (uint32_t)((total - 1) / 161);
        if (indexSectors_ < 1) indexSectors_ = 1;
        dataSectors_ = (uint32_t)(total - 1 - indexSectors_);
        writePos_ = 0;
        epoch_ = 1;
        world_ = world;
        dev_ = dev;
        return writeHeader();
    }
    dev_ = dev;
    if (world_ != world) {   // another world (or settings): a new epoch, the old entries ignored
        epoch_ = (uint16_t)(epoch_ + 1 == 0 ? 1 : epoch_ + 1);
        world_ = world;
        writePos_ = 0;
        return writeHeader();
    }
    return true;
}

bool PacketCache::writeHeader() {
    memset(sector_, 0, 512);
    put32(sector_, MAGIC);
    put32(sector_ + 4, FORMAT);
    put32(sector_ + 8, indexSectors_);
    put32(sector_ + 12, dataSectors_);
    put32(sector_ + 16, writePos_);
    put16(sector_ + 20, epoch_);
    put32(sector_ + 24, crc32(sector_, 24));
    put64(sector_ + 32, world_);
    putsSinceHeader_ = 0;
    if (!dev_->write(0, sector_, 512)) {
        stats_.putErrors++;
        return false;
    }
    return true;
}

uint32_t PacketCache::indexSector(uint64_t key) const { return 1 + (uint32_t)(mix(key) % indexSectors_); }

bool PacketCache::get(uint64_t key, uint8_t* buf, uint32_t cap, uint32_t& len) {
    len = 0;
    if (!dev_) return false;
    if (!dev_->read((uint64_t)indexSector(key) * 512, sector_, 512)) {
        stats_.readErrors++;
        return false;
    }
    int found = -1;
    for (int i = 0; i < 32 && found < 0; i++)
        if (get64(sector_ + i * 16) == key && get16(sector_ + i * 16 + 14) == epoch_) found = i;
    if (found < 0) {
        stats_.misses++;
        return false;
    }
    uint32_t at = get32(sector_ + found * 16 + 8), sectors = get16(sector_ + found * 16 + 12);
    uint32_t bytes = sectors * 512;
    if (at + sectors > dataSectors_ || bytes > cap || bytes < RECORD_HEADER) {
        stats_.stale++;
        return false;
    }
    uint64_t off = ((uint64_t)1 + indexSectors_ + at) * 512;
    if (!dev_->read(off, buf, bytes)) {
        stats_.readErrors++;
        return false;
    }
    uint32_t n = get32(buf + 4);
    if (get32(buf) != RECORD_MAGIC || get64(buf + 8) != key || get16(buf + 16) != epoch_ || n + RECORD_HEADER > bytes ||
        get32(buf + 20) != crc32(buf + RECORD_HEADER, n)) {
        stats_.stale++;   // overwritten by newer records since
        return false;
    }
    memmove(buf, buf + RECORD_HEADER, n);
    len = n;
    stats_.hits++;
    stats_.bytesRead += n;
    return true;
}

bool PacketCache::put(uint64_t key, const uint8_t* data, uint32_t len) {
    if (!dev_ || len == 0 || len > MAX_PACKET) return false;
    uint32_t sectors = (RECORD_HEADER + len + 511) / 512;
    if (sectors > dataSectors_) return false;
    if (writePos_ + sectors > dataSectors_) writePos_ = 0;   // around the ring
    uint8_t head[RECORD_HEADER];
    put32(head, RECORD_MAGIC);
    put32(head + 4, len);
    put64(head + 8, key);
    put16(head + 16, epoch_);
    put16(head + 18, 0);
    put32(head + 20, crc32(data, len));
    uint64_t off = ((uint64_t)1 + indexSectors_ + writePos_) * 512;
    static const uint8_t ZERO[512] = {};
    uint32_t pad = sectors * 512 - RECORD_HEADER - len;
    bool ok = dev_->beginWrite(off, sectors * 512) && dev_->writeData(head, RECORD_HEADER) && dev_->writeData(data, len) &&
              (pad == 0 || dev_->writeData(ZERO, pad)) && dev_->endWrite();
    if (!ok) {
        stats_.putErrors++;
        return false;
    }
    // its index entry: the same key, else a free (other epoch) one, else one by the hash
    uint64_t isec = (uint64_t)indexSector(key) * 512;
    if (!dev_->read(isec, sector_, 512)) {
        stats_.readErrors++;
        return false;
    }
    int slot = -1;
    for (int i = 0; i < 32 && slot < 0; i++)
        if (get64(sector_ + i * 16) == key) slot = i;
    for (int i = 0; i < 32 && slot < 0; i++)
        if (get16(sector_ + i * 16 + 14) != epoch_) slot = i;
    if (slot < 0) slot = (int)((mix(key) >> 40) % 32);
    uint8_t* e = sector_ + slot * 16;
    put64(e, key);
    put32(e + 8, writePos_);
    put16(e + 12, (uint16_t)sectors);
    put16(e + 14, epoch_);
    if (!dev_->write(isec, sector_, 512)) {
        stats_.putErrors++;
        return false;
    }
    writePos_ += sectors;
    stats_.puts++;
    stats_.bytesPut += len;
    if (++putsSinceHeader_ >= 64) writeHeader();
    return true;
}

bool PacketCache::flush() {
    if (!dev_) return false;
    return writeHeader() && dev_->flush();
}

void PacketCache::statusLine(char* buf, size_t cap) const {
    if (!dev_) {
        snprintf(buf, cap, "chunk cache: none");
        return;
    }
    uint32_t looks = stats_.hits + stats_.misses + stats_.stale;
    snprintf(buf, cap, "chunk cache: %s, %u MB, %u entries | %u hits, %u misses (%u stale) = %u%% | %u stored (%llu KB), %u errors",
             dev_->describe(), (unsigned)(capacityBytes() >> 20), (unsigned)entries(), (unsigned)stats_.hits,
             (unsigned)(stats_.misses + stats_.stale), (unsigned)stats_.stale, looks ? (unsigned)(stats_.hits * 100 / looks) : 0,
             (unsigned)stats_.puts, (unsigned long long)(stats_.bytesPut >> 10),
             (unsigned)(stats_.putErrors + stats_.readErrors));
}

}  // namespace mc
