// ESP32 (Arduino / ESP-IDF) implementation of the platform layer.
// Networking uses lwIP BSD sockets directly in non-blocking mode (the Arduino
// WiFiClient/WiFiServer wrappers block on writes).
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/semphr.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include "mc/platform.h"

namespace mc {

namespace {

void setNonBlocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

class LwipConn : public Conn {
public:
    LwipConn(int fd, const char* peer) : fd_(fd) {
        snprintf(peer_, sizeof(peer_), "%s", peer);
        setNonBlocking(fd_);
        int one = 1;
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
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
        if (r >= 0) return r;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ENOMEM) return 0;
        open_ = false;
        return -1;
    }
    bool connected() override { return fd_ >= 0 && open_; }
    void close() override {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        open_ = false;
    }
    const char* peer() override { return peer_; }

private:
    int fd_;
    bool open_ = true;
    char peer_[32];
};

class LwipListener : public Listener {
public:
    explicit LwipListener(int fd) : fd_(fd) {}
    ~LwipListener() override { if (fd_ >= 0) ::close(fd_); }
    Conn* accept() override {
        sockaddr_in addr;
        socklen_t len = sizeof(addr);
        int c = ::accept(fd_, (sockaddr*)&addr, &len);
        if (c < 0) return nullptr;
        char ip[16];
        inet_ntoa_r(addr.sin_addr, ip, sizeof(ip));
        char peer[32];
        snprintf(peer, sizeof(peer), "%s:%u", ip, ntohs(addr.sin_port));
        return new LwipConn(c, peer);
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
    return new LwipConn(fd, host);
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

}  // namespace plat
}  // namespace mc
