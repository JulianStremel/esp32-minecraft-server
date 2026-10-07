// Random-access byte storage used by the world store. Implementations:
//   NbdDevice  - a Network Block Device export (the main backend on ESP32)
//   FileDevice - a local file (PC builds, SD card / LittleFS on ESP32)
//   MemDevice  - RAM / PSRAM (tests, or worlds that need not survive a reboot)
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace mc {

struct DeviceStats {
    uint64_t bytesRead = 0, bytesWritten = 0;
    uint32_t reads = 0, writes = 0, flushes = 0, errors = 0, reconnects = 0;
    uint32_t lastLatencyMs = 0;
};

// A read to perform as part of a batch (see readMany).
struct ReadOp {
    uint64_t offset;
    void* buf;
    uint32_t len;
};

class BlockDevice {
public:
    virtual ~BlockDevice() {}
    virtual uint64_t size() = 0;
    virtual bool read(uint64_t off, void* buf, uint32_t len) = 0;
    virtual bool write(uint64_t off, const void* buf, uint32_t len) = 0;
    virtual bool flush() = 0;
    // Several reads at once; network devices pipeline them into one round trip.
    virtual bool readMany(ReadOp* ops, int n) {
        for (int i = 0; i < n; i++)
            if (!read(ops[i].offset, ops[i].buf, ops[i].len)) return false;
        return true;
    }
    // Streaming write of exactly `len` bytes at `off`: begin, any number of
    // writeData calls, end. Lets large records be written without a buffer.
    virtual bool beginWrite(uint64_t off, uint32_t len);
    virtual bool writeData(const void* buf, uint32_t len);
    virtual bool endWrite();
    virtual bool available() { return true; }
    virtual const char* describe() = 0;
    const DeviceStats& stats() const { return stats_; }

protected:
    DeviceStats stats_;

private:
    // default streaming implementation buffers through write()
    uint64_t streamOff_ = 0;
    uint32_t streamLeft_ = 0;
    bool streamOk_ = true;
};

class MemDevice : public BlockDevice {
public:
    explicit MemDevice(size_t bytes);
    ~MemDevice() override;
    uint64_t size() override { return size_; }
    bool read(uint64_t off, void* buf, uint32_t len) override;
    bool write(uint64_t off, const void* buf, uint32_t len) override;
    bool flush() override { return true; }
    const char* describe() override { return "RAM"; }
    bool ok() const { return mem_ != nullptr; }

private:
    uint8_t* mem_;
    size_t size_;
};

class FileDevice : public BlockDevice {
public:
    // Opens (creating if needed) a file. size: logical size (the file is sparse).
    FileDevice(const char* path, uint64_t size);
    ~FileDevice() override;
    bool ok() const { return f_ != nullptr; }
    uint64_t size() override { return size_; }
    bool read(uint64_t off, void* buf, uint32_t len) override;
    bool write(uint64_t off, const void* buf, uint32_t len) override;
    bool flush() override;
    const char* describe() override { return desc_; }

private:
    void* f_;
    uint64_t size_;
    char desc_[96];
};

uint32_t crc32(const void* data, size_t len, uint32_t crc = 0);

}  // namespace mc
