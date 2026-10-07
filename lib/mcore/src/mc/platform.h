// Platform abstraction. The server core is plain C++17 and talks to the
// outside world only through the functions and interfaces declared here.
// Implementations: host/platform_posix.cpp (Linux/macOS, used for tests and
// as a PC server) and src/platform_esp32.cpp (Arduino-ESP32 / WiFi).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace mc {

enum LogLevel { LOG_DEBUG = 0, LOG_INFO = 1, LOG_WARN = 2, LOG_ERROR = 3 };

// A non-blocking byte stream (TCP socket).
class Conn {
public:
    virtual ~Conn() {}
    // >0: bytes read, 0: nothing available right now, <0: closed/error
    virtual int read(uint8_t* buf, size_t n) = 0;
    // >=0: bytes accepted (may be < n, 0 = would block), <0: closed/error
    virtual int write(const uint8_t* buf, size_t n) = 0;
    virtual bool connected() = 0;
    virtual void close() = 0;
    virtual const char* peer() { return "?"; }
};

class Listener {
public:
    virtual ~Listener() {}
    // Returns a new connection or nullptr if none is pending.
    virtual Conn* accept() = 0;
};

namespace plat {
uint32_t millis();
void delayMs(uint32_t ms);
void yield();                       // give background tasks (WiFi stack) a slice
uint32_t random32();
size_t freeHeap();
void* bigAlloc(size_t n);           // prefers PSRAM when present, may return nullptr
void bigFree(void* p);
void logWrite(LogLevel lvl, const char* msg);
Listener* listen(uint16_t port);
Conn* connectTcp(const char* host, uint16_t port, uint32_t timeoutMs);
}  // namespace plat

void logf(LogLevel lvl, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
#define MC_LOGD(...) ::mc::logf(::mc::LOG_DEBUG, __VA_ARGS__)
#define MC_LOGI(...) ::mc::logf(::mc::LOG_INFO, __VA_ARGS__)
#define MC_LOGW(...) ::mc::logf(::mc::LOG_WARN, __VA_ARGS__)
#define MC_LOGE(...) ::mc::logf(::mc::LOG_ERROR, __VA_ARGS__)

// Helpers on top of Conn for blocking users (the storage client).
bool connReadFully(Conn* c, uint8_t* buf, size_t n, uint32_t timeoutMs);
bool connWriteFully(Conn* c, const uint8_t* buf, size_t n, uint32_t timeoutMs);

}  // namespace mc
