// ESP32 (Arduino / ESP-IDF) implementation of the platform layer.
// Networking uses lwIP BSD sockets directly in non-blocking mode (the Arduino
// WiFiClient/WiFiServer wrappers block on writes).
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_vfs_eventfd.h>
#include <freertos/semphr.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <sys/select.h>
#include "mc/platform.h"
#include "mc/tick_pacer.h"

namespace mc {

namespace {

void setNonBlocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

// Closing a socket that still has unread input makes lwIP send a reset, and the client
// may then drop what we sent last (a kick message). So a player's socket is half-closed
// and its input discarded until the client closes too (or 2 s pass). Game loop only.
struct Lingering {
    int fd;
    uint32_t until;
};
Lingering s_linger[4];
int s_lingerN = 0;

void lingerService(bool force) {
    for (int i = 0; i < s_lingerN;) {
        uint8_t buf[256];
        int r;
        while ((r = recv(s_linger[i].fd, buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
        }
        bool eof = r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
        if (force || eof || (int32_t)(::millis() - s_linger[i].until) >= 0) {
            ::close(s_linger[i].fd);
            s_linger[i] = s_linger[--s_lingerN];
        } else {
            i++;
        }
    }
}

void lingerClose(int fd) {
    if (s_lingerN == (int)(sizeof(s_linger) / sizeof(s_linger[0]))) lingerService(true);
    shutdown(fd, SHUT_WR);
    s_linger[s_lingerN++] = {fd, ::millis() + 2000};
    lingerService(false);
}

class LwipConn : public Conn {
public:
    // watched: an accepted connection the game loop waits on (not the NBD client)
    LwipConn(int fd, const char* peer, bool watched) : fd_(fd), watched_(watched) {
        snprintf(peer_, sizeof(peer_), "%s", peer);
        setNonBlocking(fd_);
        int one = 1;
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
        if (watched_) waitSet().add(fd_);
    }
    ~LwipConn() override { close(); }
    int read(uint8_t* buf, size_t n) override {
        if (fd_ < 0) return -1;
        int r = recv(fd_, buf, n, MSG_DONTWAIT);
        if (r > 0) return r;
        if (r == 0) { open_ = false; return -1; }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
        open_ = false;
        return -1;
    }
    int write(const uint8_t* buf, size_t n) override {
        if (fd_ < 0) return -1;
        int r = send(fd_, buf, n, MSG_DONTWAIT);
        if (r >= 0) {
            noteWrite((size_t)r < n);
            return r;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ENOMEM) {
            noteWrite(true);
            return 0;
        }
        open_ = false;
        return -1;
    }
    bool connected() override { return fd_ >= 0 && open_; }
    void close() override {
        if (fd_ >= 0) {
            if (watched_) {
                waitSet().remove(fd_);
                lingerClose(fd_);
            } else {
                ::close(fd_);
            }
            fd_ = -1;
        }
        open_ = false;
    }
    const char* peer() override { return peer_; }

private:
    // the send buffer is full: wake the game loop when it drains
    void noteWrite(bool blocked) {
        if (watched_ && blocked != wantWrite_) {
            wantWrite_ = blocked;
            waitSet().setWantWrite(fd_, blocked);
        }
    }
    int fd_;
    bool watched_;
    bool wantWrite_ = false;
    bool open_ = true;
    char peer_[32];
};

class LwipListener : public Listener {
public:
    explicit LwipListener(int fd) : fd_(fd) { waitSet().add(fd_); }
    ~LwipListener() override {
        if (fd_ >= 0) {
            waitSet().remove(fd_);
            ::close(fd_);
        }
    }
    Conn* accept() override {
        sockaddr_in addr;
        socklen_t len = sizeof(addr);
        int c = ::accept(fd_, (sockaddr*)&addr, &len);
        if (c < 0) return nullptr;
        char ip[16];
        inet_ntoa_r(addr.sin_addr, ip, sizeof(ip));
        char peer[32];
        snprintf(peer, sizeof(peer), "%s:%u", ip, ntohs(addr.sin_port));
        return new LwipConn(c, peer, true);
    }

private:
    int fd_;
};

}  // namespace

namespace plat {

uint32_t millis() { return ::millis(); }
uint64_t micros() { return (uint64_t)esp_timer_get_time(); }
void delayMs(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
void yield() { vTaskDelay(1); }
uint32_t random32() { return esp_random(); }
size_t freeHeap() { return heap_caps_get_free_size(MALLOC_CAP_8BIT); }

void* bigAlloc(size_t n) {
#if defined(BOARD_HAS_PSRAM)
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) return p;
#endif
    return malloc(n);
}

void bigFree(void* p) { free(p); }

void logWrite(LogLevel lvl, const char* msg) {
    static const char* names[] = {"D", "I", "W", "E"};
    Serial.printf("[%8.3f] %s %s\n", ::millis() / 1000.0, names[lvl], msg);
}

Listener* listen(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) return nullptr;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0 || ::listen(fd, 4) < 0) {
        ::close(fd);
        return nullptr;
    }
    setNonBlocking(fd);
    return new LwipListener(fd);
}

Conn* connectTcp(const char* host, uint16_t port, uint32_t timeoutMs) {
    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char ps[8];
    snprintf(ps, sizeof(ps), "%u", port);
    addrinfo* res = nullptr;
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) return nullptr;
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return nullptr; }
    setNonBlocking(fd);
    int r = ::connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (r < 0 && errno == EINPROGRESS) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        timeval tv = {(time_t)(timeoutMs / 1000), (suseconds_t)((timeoutMs % 1000) * 1000)};
        if (select(fd + 1, nullptr, &wfds, nullptr, &tv) <= 0) { ::close(fd); return nullptr; }
        int err = 0;
        socklen_t el = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err) { ::close(fd); return nullptr; }
    } else if (r < 0) {
        ::close(fd);
        return nullptr;
    }
    return new LwipConn(fd, host, false);
}

// ---- threads: FreeRTOS tasks pinned to a core
int cpuCores() { return portNUM_PROCESSORS; }

namespace {
struct ThreadStart {
    void (*fn)(void*);
    void* arg;
};
void threadTrampoline(void* p) {
    ThreadStart st = *(ThreadStart*)p;
    delete (ThreadStart*)p;
    st.fn(st.arg);
    vTaskDelete(nullptr);  // FreeRTOS tasks must not return
}
}  // namespace

bool startThread(const char* name, int core, int priority, size_t stackBytes, void (*fn)(void*), void* arg) {
    // FreeRTOS priority 1 is just above idle (0); the server task runs at 3 and the
    // WiFi/lwIP tasks at 18..23, so worker threads only get time nobody else needs.
    UBaseType_t prio = 1 + (UBaseType_t)priority;
    ThreadStart* st = new ThreadStart{fn, arg};
    BaseType_t r = xTaskCreatePinnedToCore(threadTrampoline, name, (uint32_t)stackBytes, st, prio, nullptr,
                                           core < 0 ? tskNO_AFFINITY : (BaseType_t)(core % portNUM_PROCESSORS));
    if (r != pdPASS) delete st;
    return r == pdPASS;
}

void* mutexCreate() { return xSemaphoreCreateMutex(); }
void mutexLock(void* m) { xSemaphoreTake((SemaphoreHandle_t)m, portMAX_DELAY); }
void mutexUnlock(void* m) { xSemaphoreGive((SemaphoreHandle_t)m); }
void mutexDestroy(void* m) { vSemaphoreDelete((SemaphoreHandle_t)m); }

void* semCreate() { return xSemaphoreCreateCounting(0x7FFF, 0); }
void semGive(void* s) { xSemaphoreGive((SemaphoreHandle_t)s); }
bool semTake(void* s, uint32_t timeoutMs) {
    return xSemaphoreTake((SemaphoreHandle_t)s, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}
void semDestroy(void* s) { vSemaphoreDelete((SemaphoreHandle_t)s); }

// ---- event-driven game loop: select() on the watched sockets plus an eventfd for wake()
namespace {
WaitStats s_waitStats;

int wakeFd() {
    static int fd = [] {
        esp_vfs_eventfd_config_t cfg = ESP_VFS_EVENTD_CONFIG_DEFAULT();
        esp_vfs_eventfd_register(&cfg);
        int f = eventfd(0, 0);
        if (f < 0) MC_LOGE("eventfd failed: the game loop wakes up on sockets and its timeout only");
        return f;
    }();
    return fd;
}

// The game loop's tick source: a periodic esp_timer notifies the game task once per
// period. ulTaskNotifyTake() then returns how many periods passed (more than one: the
// loop overran a tick). Game events never run in the timer callback.
class EspTimerTicks : public TickSource {
public:
    explicit EspTimerTicks(uint32_t periodMs) : periodUs_((int64_t)periodMs * 1000) {
        task_ = xTaskGetCurrentTaskHandle();
        esp_timer_create_args_t args = {};
        args.callback = &EspTimerTicks::onTimer;
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "mc_tick";
        lastUs_ = esp_timer_get_time();
        xTaskNotifyGive(task_);   // the first tick is due right away
        if (esp_timer_create(&args, &timer_) != ESP_OK || esp_timer_start_periodic(timer_, periodUs_) != ESP_OK)
            MC_LOGE("tick timer failed to start");
    }
    uint32_t take() override { return ulTaskNotifyTake(pdTRUE, 0); }
    uint32_t msUntilNext() override {
        int64_t d = lastUs_ + periodUs_ - esp_timer_get_time();
        return d > 0 ? (uint32_t)((d + 999) / 1000) : 0;
    }

private:
    static void onTimer(void* arg) {
        EspTimerTicks* t = (EspTimerTicks*)arg;
        t->lastUs_ = esp_timer_get_time();
        xTaskNotifyGive(t->task_);
        wake();
    }
    int64_t periodUs_;
    volatile int64_t lastUs_ = 0;
    TaskHandle_t task_ = nullptr;
    esp_timer_handle_t timer_ = nullptr;
};
}  // namespace

void wake() {
    int fd = wakeFd();
    if (fd < 0) return;
    uint64_t one = 1;
    ::write(fd, &one, sizeof(one));
}

void waitForWork(uint32_t timeoutMs, bool console) {
    (void)console;
    lingerService(false);
    int fds[WaitSet::MAX];
    bool wantWrite[WaitSet::MAX];
    int n = waitSet().snapshot(fds, wantWrite, WaitSet::MAX);
    fd_set rd, wr;
    FD_ZERO(&rd);
    FD_ZERO(&wr);
    int maxFd = -1;
    for (int i = 0; i < n; i++) {
        FD_SET(fds[i], &rd);
        if (wantWrite[i]) FD_SET(fds[i], &wr);
        if (fds[i] > maxFd) maxFd = fds[i];
    }
    for (int i = 0; i < s_lingerN; i++) {
        FD_SET(s_linger[i].fd, &rd);
        if (s_linger[i].fd > maxFd) maxFd = s_linger[i].fd;
    }
    int efd = wakeFd();
    if (efd >= 0) {
        FD_SET(efd, &rd);
        if (efd > maxFd) maxFd = efd;
    }
    timeval tv = {(time_t)(timeoutMs / 1000), (suseconds_t)((timeoutMs % 1000) * 1000)};
    uint32_t t0 = ::millis();
    int r = select(maxFd + 1, &rd, &wr, nullptr, &tv);
    s_waitStats.calls++;
    s_waitStats.sleptMs += ::millis() - t0;
    if (r == 0) s_waitStats.timeouts++;
    for (int i = 0; r > 0 && i < n; i++) {
        if (FD_ISSET(fds[i], &rd)) s_waitStats.readable++;
        if (FD_ISSET(fds[i], &wr)) s_waitStats.writable++;
    }
    if (r > 0 && efd >= 0 && FD_ISSET(efd, &rd)) {
        s_waitStats.wakes++;
        uint64_t v;
        ::read(efd, &v, sizeof(v));
    }
    if (r > 0 && s_lingerN) lingerService(false);
}

TickSource* createTickTimer(uint32_t periodMs) { return new EspTimerTicks(periodMs); }

WaitStats takeWaitStats() {
    WaitStats s = s_waitStats;
    s_waitStats = WaitStats();
    return s;
}

}  // namespace plat
}  // namespace mc
