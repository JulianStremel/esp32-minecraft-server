#include "mc/platform.h"
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
