#include "mc/platform.h"
#include "mc/log_ring.h"
#include <stdarg.h>
#include <stdio.h>

namespace mc {

void logf(LogLevel lvl, const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    plat::logWrite(lvl, buf);
    if (LogRing* r = logRing()) r->append((uint8_t)lvl, plat::millis(), buf);
}

WaitSet& waitSet() {
    static WaitSet s;
    return s;
}

void WaitSet::lock() {
    if (!mutex_) mutex_ = plat::mutexCreate();   // first use is on the game loop's thread
    plat::mutexLock(mutex_);
}

void WaitSet::add(int fd) {
    lock();
    for (int i = 0; i < n_; i++)
        if (fds_[i] == fd) { wantWrite_[i] = false; plat::mutexUnlock(mutex_); return; }
    if (n_ < MAX) {
        fds_[n_] = fd;
        wantWrite_[n_] = false;
        n_++;
    }
    plat::mutexUnlock(mutex_);
}

void WaitSet::remove(int fd) {
    lock();
    for (int i = 0; i < n_; i++)
        if (fds_[i] == fd) {
            fds_[i] = fds_[n_ - 1];
            wantWrite_[i] = wantWrite_[n_ - 1];
            n_--;
            break;
        }
    plat::mutexUnlock(mutex_);
}

void WaitSet::setWantWrite(int fd, bool on) {
    lock();
    for (int i = 0; i < n_; i++)
        if (fds_[i] == fd) { wantWrite_[i] = on; break; }
    plat::mutexUnlock(mutex_);
}

int WaitSet::snapshot(int* fds, bool* wantWrite, int max) {
    lock();
    int n = n_ < max ? n_ : max;
    for (int i = 0; i < n; i++) {
        fds[i] = fds_[i];
        wantWrite[i] = wantWrite_[i];
    }
    plat::mutexUnlock(mutex_);
    return n;
}

bool connReadFully(Conn* c, uint8_t* buf, size_t n, uint32_t timeoutMs) {
    uint32_t start = plat::millis();
    size_t got = 0;
    while (got < n) {
        int r = c->read(buf + got, n - got);
        if (r < 0) return false;
        if (r == 0) {
            if (plat::millis() - start > timeoutMs) return false;
            plat::yield();
            continue;
        }
        got += (size_t)r;
        start = plat::millis();
    }
    return true;
}

bool connWriteFully(Conn* c, const uint8_t* buf, size_t n, uint32_t timeoutMs) {
    uint32_t start = plat::millis();
    size_t sent = 0;
    while (sent < n) {
        int r = c->write(buf + sent, n - sent);
        if (r < 0) return false;
        if (r == 0) {
            if (plat::millis() - start > timeoutMs) return false;
            plat::yield();
            continue;
        }
        sent += (size_t)r;
        start = plat::millis();
    }
    return true;
}

}  // namespace mc
