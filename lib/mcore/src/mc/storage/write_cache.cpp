#include "mc/storage/write_cache.h"
#include <stdio.h>
#include <string.h>
#include "mc/platform.h"

namespace mc {

WriteBackCache::WriteBackCache(BlockDevice* inner, uint32_t lineBytes, int lines)
    : inner_(inner), lineBytes_(lineBytes), nLines_(lines) {
    lines_ = (Line*)plat::bigAlloc(sizeof(Line) * (size_t)lines);
    mem_ = (uint8_t*)plat::bigAlloc((size_t)lineBytes * (size_t)lines);
    if (!lines_ || !mem_) {
        plat::bigFree(lines_);
        plat::bigFree(mem_);
        lines_ = nullptr;
        mem_ = nullptr;
        nLines_ = 0;
    } else {
        memset(lines_, 0, sizeof(Line) * (size_t)lines);
        stageLines_ = (int)((128 * 1024) / lineBytes);
        if (stageLines_ < 1) stageLines_ = 1;
        if (stageLines_ > lines) stageLines_ = lines;
        stage_ = (uint8_t*)plat::bigAlloc((size_t)stageLines_ * lineBytes);
        if (!stage_) stageLines_ = 1;
    }
    snprintf(desc_, sizeof(desc_), "%s, %u x %u KiB write cache", inner->describe(), (unsigned)lines,
             (unsigned)(lineBytes / 1024));
}

WriteBackCache::~WriteBackCache() {
    flush();
    plat::bigFree(lines_);
    plat::bigFree(mem_);
    plat::bigFree(stage_);
}

int WriteBackCache::find(uint64_t lineOff) const {
    for (int i = 0; i < nLines_; i++)
        if (lines_[i].state != L_FREE && lines_[i].off == lineOff) return i;
    return -1;
}

int WriteBackCache::dirtyLines() const {
    int n = 0;
    for (int i = 0; i < nLines_; i++) n += lines_[i].state == L_DIRTY;
    return n;
}

// Writes the (up to maxLines) oldest dirty lines; neighbours in that order that are also
// neighbours on the device go out as one write.
bool WriteBackCache::writeOut(int maxLines) {
    int done = 0;
    while (done < maxLines) {
        int first = -1;
        for (int i = 0; i < nLines_; i++)
            if (lines_[i].state == L_DIRTY && (first < 0 || lines_[i].dirtySeq < lines_[first].dirtySeq)) first = i;
        if (first < 0) return true;
        // the run: the next-oldest dirty lines while each follows the previous on the device
        int run[64];
        int n = 0;
        run[n++] = first;
        while (n < stageLines_ && n < 64) {
            const Line& last = lines_[run[n - 1]];
            int next = -1;
            for (int i = 0; i < nLines_; i++)
                if (lines_[i].state == L_DIRTY && lines_[i].dirtySeq > last.dirtySeq &&
                    (next < 0 || lines_[i].dirtySeq < lines_[next].dirtySeq))
                    next = i;
            if (next < 0 || lines_[next].off != last.off + last.len) break;
            run[n++] = next;
        }
        // contiguous on the device: one write (copied together: lines are not contiguous
        // in memory)
        if (n == 1) {
            if (!inner_->write(lines_[first].off, data(first), lines_[first].len)) return false;
        } else {
            uint32_t total = 0;
            for (int k = 0; k < n; k++) {
                memcpy(stage_ + total, data(run[k]), lines_[run[k]].len);
                total += lines_[run[k]].len;
            }
            if (!inner_->write(lines_[first].off, stage_, total)) return false;
        }
        for (int k = 0; k < n; k++) lines_[run[k]].state = L_CLEAN;
        done += n;
    }
    return true;
}

int WriteBackCache::take(uint64_t lineOff, bool fill) {
    int i = find(lineOff);
    if (i >= 0) return i;
    // a free line, else the least recently used clean one, else write out the oldest dirty
    int pick = -1;
    for (int k = 0; k < nLines_ && pick < 0; k++)
        if (lines_[k].state == L_FREE) pick = k;
    if (pick < 0)
        for (int k = 0; k < nLines_; k++)
            if (lines_[k].state == L_CLEAN && (pick < 0 || lines_[k].used < lines_[pick].used)) pick = k;
    if (pick < 0) {
        if (!writeOut(nLines_ / 4 > 0 ? nLines_ / 4 : 1)) return -1;
        for (int k = 0; k < nLines_; k++)
            if (lines_[k].state == L_CLEAN && (pick < 0 || lines_[k].used < lines_[pick].used)) pick = k;
        if (pick < 0) return -1;
    }
    Line& l = lines_[pick];
    uint64_t end = inner_->size();
    l.off = lineOff;
    l.len = lineOff + lineBytes_ <= end ? lineBytes_ : (uint32_t)(end - lineOff);
    l.state = L_CLEAN;
    l.used = ++clock_;
    if (fill && !inner_->read(lineOff, data(pick), l.len)) {
        l.state = L_FREE;
        return -1;
    }
    return pick;
}

bool WriteBackCache::read(uint64_t off, void* buf, uint32_t len) {
    if (!mem_) return inner_->read(off, buf, len);
    if (off + len > size()) return false;
    uint8_t* out = (uint8_t*)buf;
    stats_.reads++;
    stats_.bytesRead += len;
    while (len) {
        uint64_t lineOff = off & ~(uint64_t)(lineBytes_ - 1);
        uint32_t in = (uint32_t)(off - lineOff), n = lineBytes_ - in < len ? lineBytes_ - in : len;
        int i = find(lineOff);
        if (i >= 0) {
            memcpy(out, data(i) + in, n);
            lines_[i].used = ++clock_;
        } else {
            // uncached: straight from the device, as far as the next cached line
            uint32_t run = n;
            while (run < len && find(lineOff + in + run) < 0) {
                uint32_t more = len - run < lineBytes_ ? len - run : lineBytes_;
                run += more;
            }
            if (!inner_->read(off, out, run)) return false;
            n = run;
        }
        out += n;
        off += n;
        len -= n;
    }
    return true;
}

bool WriteBackCache::write(uint64_t off, const void* buf, uint32_t len) {
    if (!mem_) return inner_->write(off, buf, len);
    if (off + len > size()) return false;
    const uint8_t* in = (const uint8_t*)buf;
    stats_.writes++;
    stats_.bytesWritten += len;
    while (len) {
        uint64_t lineOff = off & ~(uint64_t)(lineBytes_ - 1);
        uint32_t at = (uint32_t)(off - lineOff), n = lineBytes_ - at < len ? lineBytes_ - at : len;
        uint64_t end = inner_->size();
        uint32_t lineLen = lineOff + lineBytes_ <= end ? lineBytes_ : (uint32_t)(end - lineOff);
        bool whole = at == 0 && n == lineLen;
        int i = take(lineOff, !whole);   // a partial write needs the rest of the line
        if (i < 0) {
            stats_.errors++;
            return false;
        }
        memcpy(data(i) + at, in, n);
        Line& l = lines_[i];
        if (l.state != L_DIRTY) {
            l.state = L_DIRTY;
            l.dirtySeq = ++seq_;
        }
        l.used = ++clock_;
        in += n;
        off += n;
        len -= n;
    }
    return true;
}

bool WriteBackCache::flush() {
    stats_.flushes++;
    if (mem_ && !writeOut(nLines_)) {
        stats_.errors++;
        return false;
    }
    return inner_->flush();
}

bool WriteBackCache::flushLater() {
    stats_.flushes++;
    if (mem_ && !writeOut(nLines_)) {
        stats_.errors++;
        return false;
    }
    return inner_->flushLater();
}

}  // namespace mc
