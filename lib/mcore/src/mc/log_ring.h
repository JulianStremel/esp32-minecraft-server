// The last log lines, kept in memory for the dashboard's console (builds with
// MC_DASHBOARD install one). mc::logf appends every line it writes, from any thread; a
// reader asks for the lines from a sequence number on, and learns how many it missed
// when the ring has overwritten them.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace mc {

class LogRing {
public:
    static constexpr size_t MAX_LINE = 255;   // longer lines are cut

    // `cap` bytes from plat::bigAlloc (PSRAM on the board); false when that fails
    bool init(size_t cap);
    ~LogRing();

    void append(uint8_t level, uint32_t ms, const char* text);

    // Calls fn(seq, ms, level, text, len) for the lines from `from` on (the oldest kept
    // when `from` is older), in order, until fn returns false. Returns the sequence
    // number after the last line passed. fn runs under the ring's lock: no logging in it.
    template <class Fn>
    uint32_t read(uint32_t from, Fn fn) {
        Cursor c;
        c.seq = from;
        read(c, fn);
        return c.seq;
    }
    // The same for a reader that comes back: it keeps where it stopped, so the next read
    // starts there instead of walking the ring from its oldest line.
    struct Cursor {
        uint32_t seq = 0;    // the next line wanted
        size_t off = 0;      // where it is, when known
        bool known = false;
    };
    template <class Fn>
    void read(Cursor& c, Fn fn);

    uint32_t first() const { return firstSeq_; }   // the oldest line kept
    uint32_t next() const { return nextSeq_; }     // the one the next append gets
    size_t used() const { return used_; }
    size_t capacity() const { return cap_; }

private:
    static constexpr size_t HDR = 10;   // u32 seq, u32 ms, u8 level, u8 len
    void put(size_t off, const void* src, size_t n);
    void get(size_t off, void* dst, size_t n) const;
    void lock() const;
    void unlock() const;

    uint8_t* buf_ = nullptr;
    size_t cap_ = 0;
    size_t head_ = 0;   // the oldest record
    size_t tail_ = 0;   // where the next one goes
    size_t used_ = 0;
    uint32_t firstSeq_ = 0, nextSeq_ = 0;
    void* mutex_ = nullptr;
};

template <class Fn>
void LogRing::read(Cursor& c, Fn fn) {
    if (!buf_) return;
    lock();
    uint32_t from = c.seq;
    size_t off = head_;
    uint32_t seq = firstSeq_;
    // still kept (records never move): carry on from there
    if (c.known && (int32_t)(from - firstSeq_) >= 0 && (int32_t)(nextSeq_ - from) >= 0) {
        off = c.off;
        seq = from;
    }
    // else skip what the reader has (a walk over the headers)
    while (seq != nextSeq_ && (int32_t)(seq - from) < 0) {
        uint8_t len;
        get((off + 9) % cap_, &len, 1);
        off = (off + HDR + len) % cap_;
        seq++;
    }
    char text[MAX_LINE + 1];
    while (seq != nextSeq_) {
        uint8_t h[HDR];
        get(off, h, HDR);
        uint32_t ms = (uint32_t)h[4] | (uint32_t)h[5] << 8 | (uint32_t)h[6] << 16 | (uint32_t)h[7] << 24;
        uint8_t len = h[9];
        get((off + HDR) % cap_, text, len);
        text[len] = 0;
        if (!fn(seq, ms, h[8], (const char*)text, (size_t)len)) break;
        off = (off + HDR + len) % cap_;
        seq++;
    }
    c.seq = seq;
    c.off = off;
    c.known = true;
    unlock();
}

// The ring mc::logf feeds (nullptr: none). Set once, before other threads log.
void setLogRing(LogRing* r);
LogRing* logRing();

}  // namespace mc
