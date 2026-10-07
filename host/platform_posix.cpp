// POSIX implementation of the platform layer (native build: tests + PC server).
#include "mc/platform.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

namespace mc {

namespace {

void setNonBlocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

class PosixConn : public Conn {
public:
    explicit PosixConn(int fd, const char* peer) : fd_(fd) {
        snprintf(peer_, sizeof(peer_), "%s", peer);
        setNonBlocking(fd_);
        int one = 1;
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    ~PosixConn() override { close(); }
    int read(uint8_t* buf, size_t n) override {
        if (fd_ < 0) return -1;
        ssize_t r = ::recv(fd_, buf, n, 0);
        if (r > 0) return (int)r;
        if (r == 0) { open_ = false; return -1; }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
        open_ = false;
        return -1;
    }
    int write(const uint8_t* buf, size_t n) override {
        if (fd_ < 0) return -1;
        ssize_t r = ::send(fd_, buf, n, MSG_NOSIGNAL);
        if (r >= 0) return (int)r;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
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
    char peer_[48];
};

class PosixListener : public Listener {
public:
    explicit PosixListener(int fd) : fd_(fd) {}
    ~PosixListener() override { if (fd_ >= 0) ::close(fd_); }
    Conn* accept() override {
        sockaddr_in addr;
        socklen_t len = sizeof(addr);
        int c = ::accept(fd_, (sockaddr*)&addr, &len);
        if (c < 0) return nullptr;
        char ip[40];
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        char peer[48];
        snprintf(peer, sizeof(peer), "%s:%u", ip, ntohs(addr.sin_port));
        return new PosixConn(c, peer);
    }

private:
    int fd_;
};

}  // namespace

namespace plat {

uint32_t millis() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000ull + ts.tv_nsec / 1000000ull);
}
void delayMs(uint32_t ms) { usleep(ms * 1000); }
void yield() { usleep(200); }
uint32_t random32() {
    uint32_t v;
    if (getrandom(&v, sizeof(v), 0) != sizeof(v)) v = (uint32_t)rand();
    return v;
}
size_t freeHeap() { return 64u * 1024 * 1024; }
void* bigAlloc(size_t n) { return malloc(n); }
void bigFree(void* p) { free(p); }

void logWrite(LogLevel lvl, const char* msg) {
    static const char* names[] = {"DEBUG", "INFO ", "WARN ", "ERROR"};
    fprintf(stderr, "[%7.3f] %s %s\n", millis() / 1000.0, names[lvl], msg);
}

Listener* listen(uint16_t port) {
    signal(SIGPIPE, SIG_IGN);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return nullptr;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0 || ::listen(fd, 8) < 0) {
        ::close(fd);
        return nullptr;
    }
    setNonBlocking(fd);
    return new PosixListener(fd);
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
        pollfd p = {fd, POLLOUT, 0};
        if (poll(&p, 1, (int)timeoutMs) <= 0) { ::close(fd); return nullptr; }
        int err = 0;
        socklen_t el = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err) { ::close(fd); return nullptr; }
    } else if (r < 0) {
        ::close(fd);
        return nullptr;
    }
    return new PosixConn(fd, host);
}

}  // namespace plat
}  // namespace mc
