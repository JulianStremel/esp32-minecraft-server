#include "mc/storage/block_device.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc/platform.h"

#if defined(ESP_PLATFORM) || defined(ARDUINO)
#define MC_FSEEK(f, off) fseek((f), (long)(off), SEEK_SET)
#else
#define MC_FSEEK(f, off) fseeko((f), (off_t)(off), SEEK_SET)
#endif

namespace mc {

uint32_t crc32(const void* data, size_t len, uint32_t crc) {
    static const uint32_t T[16] = {0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4,
                                   0x4DB26158, 0x5005713C, 0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
                                   0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C};
    const uint8_t* p = (const uint8_t*)data;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        crc = (crc >> 4) ^ T[crc & 15];
        crc = (crc >> 4) ^ T[crc & 15];
    }
    return ~crc;
}

// ---------------------------------------------------------------- default streaming
// Generic fallback: forwards each piece as a positioned write.
bool BlockDevice::beginWrite(uint64_t off, uint32_t len) {
    streamOff_ = off;
    streamLeft_ = len;
    streamOk_ = true;
    return true;
}

bool BlockDevice::writeData(const void* buf, uint32_t len) {
    if (!streamOk_ || len > streamLeft_) { streamOk_ = false; return false; }
    if (!write(streamOff_, buf, len)) streamOk_ = false;
    streamOff_ += len;
    streamLeft_ -= len;
    return streamOk_;
}

bool BlockDevice::endWrite() { return streamOk_ && streamLeft_ == 0; }

// ---------------------------------------------------------------- RAM
MemDevice::MemDevice(size_t bytes) : size_(bytes) {
    mem_ = (uint8_t*)plat::bigAlloc(bytes);
    if (mem_) memset(mem_, 0, bytes);
    else size_ = 0;
}

MemDevice::~MemDevice() { plat::bigFree(mem_); }

bool MemDevice::read(uint64_t off, void* buf, uint32_t len) {
    if (!mem_ || off + len > size_) return false;
    memcpy(buf, mem_ + off, len);
    stats_.reads++;
    stats_.bytesRead += len;
    return true;
}

bool MemDevice::write(uint64_t off, const void* buf, uint32_t len) {
    if (!mem_ || off + len > size_) return false;
    memcpy(mem_ + off, buf, len);
    stats_.writes++;
    stats_.bytesWritten += len;
    return true;
}

// ---------------------------------------------------------------- file
FileDevice::FileDevice(const char* path, uint64_t size) : f_(nullptr), size_(size) {
    FILE* f = fopen(path, "r+b");
    if (!f) f = fopen(path, "w+b");
    f_ = f;
    snprintf(desc_, sizeof(desc_), "file %s", path);
}

FileDevice::~FileDevice() {
    if (f_) fclose((FILE*)f_);
}

bool FileDevice::read(uint64_t off, void* buf, uint32_t len) {
    FILE* f = (FILE*)f_;
    if (!f || off + len > size_) return false;
    memset(buf, 0, len);  // beyond the end of a sparse file reads as zeros
    if (MC_FSEEK(f, off) != 0) { stats_.errors++; return false; }
    size_t n = fread(buf, 1, len, f);
    (void)n;
    clearerr(f);
    stats_.reads++;
    stats_.bytesRead += len;
    return true;
}

bool FileDevice::write(uint64_t off, const void* buf, uint32_t len) {
    FILE* f = (FILE*)f_;
    if (!f || off + len > size_) return false;
    if (MC_FSEEK(f, off) != 0 || fwrite(buf, 1, len, f) != len) {
        stats_.errors++;
        return false;
    }
    stats_.writes++;
    stats_.bytesWritten += len;
    return true;
}

bool FileDevice::flush() {
    FILE* f = (FILE*)f_;
    if (!f) return false;
    stats_.flushes++;
    return fflush(f) == 0;
}

}  // namespace mc
