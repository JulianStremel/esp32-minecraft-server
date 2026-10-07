// Growable byte buffer (PSRAM on the ESP32) usable as a Sink: the output of
// background jobs (compressed packets, encoded chunk records).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "mc/io.h"
#include "mc/platform.h"

namespace mc {

class ByteBuf : public Sink {
public:
    ByteBuf() {}
    ~ByteBuf() override { plat::bigFree(data_); }
    ByteBuf(const ByteBuf&) = delete;
    ByteBuf& operator=(const ByteBuf&) = delete;

    void put(const uint8_t* d, size_t n) override {
        if (failed_ || !n) return;
        if (len_ + n > cap_ && !reserve(len_ + n)) return;
        memcpy(data_ + len_, d, n);
        len_ += n;
    }
    // Ensures room for `need` bytes in total (grows geometrically).
    bool reserve(size_t need) {
        if (need <= cap_) return true;
        size_t cap = cap_ ? cap_ : 1024;
        while (cap < need) cap *= 2;
        uint8_t* p = (uint8_t*)plat::bigAlloc(cap);
        if (!p) {
            failed_ = true;
            return false;
        }
        if (len_) memcpy(p, data_, len_);
        plat::bigFree(data_);
        data_ = p;
        cap_ = cap;
        return true;
    }
    // Appends n uninitialised bytes and returns them (nullptr when out of memory).
    uint8_t* append(size_t n) {
        if (failed_ || (len_ + n > cap_ && !reserve(len_ + n))) return nullptr;
        uint8_t* p = data_ + len_;
        len_ += n;
        return p;
    }
    void clear() {
        len_ = 0;
        failed_ = false;
    }
    const uint8_t* data() const { return data_; }
    uint8_t* data() { return data_; }
    size_t size() const { return len_; }
    bool failed() const { return failed_; }   // an allocation failed: contents incomplete

private:
    uint8_t* data_ = nullptr;
    size_t len_ = 0, cap_ = 0;
    bool failed_ = false;
};

}  // namespace mc
