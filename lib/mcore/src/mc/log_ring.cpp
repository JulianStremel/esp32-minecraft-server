#include "mc/log_ring.h"
#include <string.h>
#include "mc/platform.h"

namespace mc {

static LogRing* g_ring = nullptr;
void setLogRing(LogRing* r) { g_ring = r; }
LogRing* logRing() { return g_ring; }

bool LogRing::init(size_t cap) {
    if (buf_) return true;
    if (!mutex_) mutex_ = plat::mutexCreate();
    buf_ = (uint8_t*)plat::bigAlloc(cap);
    if (!buf_ || !mutex_) return false;
    cap_ = cap;
    return true;
}

LogRing::~LogRing() {
    if (g_ring == this) g_ring = nullptr;
    plat::bigFree(buf_);
}

void LogRing::lock() const { plat::mutexLock(mutex_); }
void LogRing::unlock() const { plat::mutexUnlock(mutex_); }

void LogRing::put(size_t off, const void* src, size_t n) {
    size_t a = n < cap_ - off ? n : cap_ - off;
    memcpy(buf_ + off, src, a);
    memcpy(buf_, (const uint8_t*)src + a, n - a);
}

void LogRing::get(size_t off, void* dst, size_t n) const {
    size_t a = n < cap_ - off ? n : cap_ - off;
    memcpy(dst, buf_ + off, a);
    memcpy((uint8_t*)dst + a, buf_, n - a);
}

void LogRing::append(uint8_t level, uint32_t ms, const char* text) {
    if (!buf_) return;
    size_t len = strlen(text);
    if (len > MAX_LINE) len = MAX_LINE;
    size_t need = HDR + len;
    if (need > cap_) return;
    lock();
    while (cap_ - used_ < need) {   // make room: the oldest lines go
        uint8_t l;
        get((head_ + 9) % cap_, &l, 1);
        head_ = (head_ + HDR + l) % cap_;
        used_ -= HDR + l;
        firstSeq_++;
    }
    uint8_t h[HDR] = {(uint8_t)nextSeq_, (uint8_t)(nextSeq_ >> 8), (uint8_t)(nextSeq_ >> 16), (uint8_t)(nextSeq_ >> 24),
                      (uint8_t)ms, (uint8_t)(ms >> 8), (uint8_t)(ms >> 16), (uint8_t)(ms >> 24),
                      level, (uint8_t)len};
    put(tail_, h, HDR);
    put((tail_ + HDR) % cap_, text, len);
    tail_ = (tail_ + need) % cap_;
    used_ += need;
    nextSeq_++;
    unlock();
}

}  // namespace mc
