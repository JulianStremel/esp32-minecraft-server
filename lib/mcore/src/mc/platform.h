// Platform abstraction. The server core is plain C++17 and talks to the
// outside world only through the functions and interfaces declared here.
// Implementations: host/platform_posix.cpp (Linux/macOS, used for tests and
// as a PC server) and src/platform_esp32.cpp (Arduino-ESP32 / WiFi).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace mc {

class TickSource;

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
uint64_t micros();                  // monotonic microseconds (benchmarks, profiling)
void delayMs(uint32_t ms);
void yield();                       // give background tasks (WiFi stack) a slice
uint32_t random32();
size_t freeHeap();
void* bigAlloc(size_t n);           // prefers PSRAM when present, may return nullptr
void bigFree(void* p);
void logWrite(LogLevel lvl, const char* msg);
Listener* listen(uint16_t port);
Conn* connectTcp(const char* host, uint16_t port, uint32_t timeoutMs);

// ---- threads (used by the worker pool, see mc/jobs.h)
int cpuCores();                     // number of CPU cores (one worker thread each)
// Starts a detached thread running fn(arg); on the ESP32 it is pinned to `core`.
// priority: 0 = lowest application priority, larger = more urgent.
bool startThread(const char* name, int core, int priority, size_t stackBytes, void (*fn)(void*), void* arg);
void* mutexCreate();
void mutexLock(void* m);
void mutexUnlock(void* m);
void mutexDestroy(void* m);
void* semCreate();                  // counting semaphore, initially 0
void semGive(void* s);
bool semTake(void* s, uint32_t timeoutMs);   // false on timeout
void semDestroy(void* s);

// ---- event-driven game loop
// Sleeps until a listening or accepted socket is readable, an accepted socket with
// unsent output is writable, wake() is called or timeoutMs pass. console: the PC server
// also wakes up for input on stdin. Outgoing connections (NBD) never wake it.
void waitForWork(uint32_t timeoutMs, bool console = false);
// Why waitForWork() returned, counted since the last call (several reasons can apply).
struct WaitStats {
    uint32_t calls = 0, timeouts = 0, wakes = 0, readable = 0, writable = 0;
    uint32_t sleptMs = 0;
};
WaitStats takeWaitStats();
// Ends the current (or the next) waitForWork() early. Any thread or timer callback.
void wake();
// The game loop's tick source (see mc/tick_pacer.h). The firmware's is a periodic
// hardware timer that notifies the calling task, so call it from the game loop's task.
TickSource* createTickTimer(uint32_t periodMs);
}  // namespace plat

// Sockets plat::waitForWork() watches; the platform layers keep it up to date.
class WaitSet {
public:
    static constexpr int MAX = 32;
    void add(int fd);
    void remove(int fd);
    void setWantWrite(int fd, bool on);   // unsent output: wake up when writable
    // copies the set; returns the number of sockets
    int snapshot(int* fds, bool* wantWrite, int max);

private:
    void lock();
    void* mutex_ = nullptr;
    int fds_[MAX];
    bool wantWrite_[MAX];
    int n_ = 0;
};
WaitSet& waitSet();

// Scoped lock on a plat mutex.
class LockGuard {
public:
    explicit LockGuard(void* m) : m_(m) { plat::mutexLock(m_); }
    ~LockGuard() { plat::mutexUnlock(m_); }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    void* m_;
};

void logf(LogLevel lvl, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
#define MC_LOGD(...) ::mc::logf(::mc::LOG_DEBUG, __VA_ARGS__)
#define MC_LOGI(...) ::mc::logf(::mc::LOG_INFO, __VA_ARGS__)
#define MC_LOGW(...) ::mc::logf(::mc::LOG_WARN, __VA_ARGS__)
#define MC_LOGE(...) ::mc::logf(::mc::LOG_ERROR, __VA_ARGS__)

// Helpers on top of Conn for blocking users (the storage client).
bool connReadFully(Conn* c, uint8_t* buf, size_t n, uint32_t timeoutMs);
bool connWriteFully(Conn* c, const uint8_t* buf, size_t n, uint32_t timeoutMs);

}  // namespace mc
