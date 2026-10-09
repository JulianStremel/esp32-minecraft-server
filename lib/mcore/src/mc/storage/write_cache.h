// A write-back cache in front of a block device, for flash media (SD cards): small
// writes (region maps, superblocks, player records, log entries) are gathered into
// aligned lines and reach the device as few large writes, in the order the lines were
// first changed, on flush() or when the cache needs room. The card's controller then
// sees large sequential writes instead of many small ones.
//
// Streamed writes (chunk records, at least DIRECT_MIN bytes) bypass the lines: they
// are gathered and written at once, so the cache does not read whole lines from the card
// to change part of them; cached lines they overlap get the new bytes too. Such a record
// reaches the device before the region map that points to it (maps stay in the lines).
//
// Ordering: lines are written oldest-first, so a power cut can only lose what was not
// flushed yet. The world store keeps two copies of every record, so a cut between
// lines leaves the old copy (or, for a chunk saved for the first time, nothing).
#pragma once
#include <stdint.h>
#include "mc/storage/block_device.h"

namespace mc {

class WriteBackCache : public BlockDevice {
public:
    // lineBytes: a power of two (16 KiB suits SD cards); lines: how many (in PSRAM)
    WriteBackCache(BlockDevice* inner, uint32_t lineBytes = 16 * 1024, int lines = 32);
    ~WriteBackCache() override;
    bool ok() const { return mem_ != nullptr; }
    static const uint32_t DIRECT_MIN = 512;

    uint64_t size() override { return inner_->size(); }
    bool read(uint64_t off, void* buf, uint32_t len) override;
    bool write(uint64_t off, const void* buf, uint32_t len) override;
    bool flush() override;
    bool flushLater() override;
    bool beginWrite(uint64_t off, uint32_t len) override;
    bool writeData(const void* buf, uint32_t len) override;
    bool endWrite() override;
    bool available() override { return inner_->available(); }
    const char* describe() override { return desc_; }

    int dirtyLines() const;
    uint32_t lineBytes() const { return lineBytes_; }
    BlockDevice* inner() { return inner_; }
    BlockDevice* backing() override { return inner_; }

private:
    enum : uint8_t { L_FREE = 0, L_CLEAN, L_DIRTY };
    struct Line {
        uint64_t off;
        uint32_t len;        // lineBytes, or less for the device's last line
        uint32_t dirtySeq;   // when it was first changed since the last write-out
        uint32_t used;       // for choosing a clean line to reuse
        uint8_t state;
    };
    int find(uint64_t lineOff) const;
    int take(uint64_t lineOff, bool fill);   // a line for lineOff (loaded unless fill = false)
    bool writeOut(int maxLines);             // the oldest dirty lines, merged into runs
    uint8_t* data(int i) { return mem_ + (size_t)i * lineBytes_; }

    BlockDevice* inner_;
    uint32_t lineBytes_;
    int nLines_;
    Line* lines_ = nullptr;
    uint8_t* mem_ = nullptr;
    uint8_t* stage_ = nullptr;   // a run of lines, copied together for one write
    int stageLines_ = 0;
    uint32_t seq_ = 0, clock_ = 0;
    // a direct (bypassing) stream being gathered in stage_
    bool direct_ = false;
    uint64_t directOff_ = 0;
    uint32_t directLen_ = 0, directFill_ = 0;
    char desc_[112];
};

}  // namespace mc
