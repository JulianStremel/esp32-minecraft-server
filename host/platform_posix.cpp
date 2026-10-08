// POSIX implementation of the platform layer (native build: tests + PC server).
#include "mc/platform.h"
#include "mc/tick_pacer.h"
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
#include <condition_variable>
#include <mutex>
#include <thread>

namespace mc {

namespace {

void setNonBlocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

// Closing a socket that still has unread input makes TCP send a reset, and the client
// may then drop what we sent last (a kick message). So a player's socket is half-closed
// and its input discarded until the client closes too (or 2 s pass). Game loop only.
struct Lingering {
    int fd;
    uint32_t until;
};
Lingering s_linger[8];
int s_lingerN = 0;

void lingerService(bool force) {
    for (int i = 0; i < s_lingerN;) {
        char buf[512];
        ssize_t r;
        while ((r = ::recv(s_linger[i].fd, buf, sizeof(buf), 0)) > 0) {
        }
        bool eof = r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
        if (force || eof || (int32_t)(plat::millis() - s_linger[i].until) >= 0) {
            ::close(s_linger[i].fd);
            s_linger[i] = s_linger[--s_lingerN];
        } else {
            i++;
        }
    }
}

void lingerClose(int fd) {
    if (s_lingerN == (int)(sizeof(s_linger) / sizeof(s_linger[0]))) lingerService(true);
    ::shutdown(fd, SHUT_WR);
    s_linger[s_lingerN++] = {fd, plat::millis() + 2000};
    lingerService(false);
}

class PosixConn : public Conn {
public:
    // watched: an accepted connection the game loop waits on (not the NBD client)
    PosixConn(int fd, const char* peer, bool watched) : fd_(fd), watched_(watched) {
        snprintf(peer_, sizeof(peer_), "%s", peer);
        setNonBlocking(fd_);
        int one = 1;
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        if (watched_) waitSet().add(fd_);
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
        if (r >= 0) {
            noteWrite((size_t)r < n);
            return (int)r;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
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
    // the socket buffer is full: wake the game loop when it drains
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
    char peer_[48];
};

class PosixListener : public Listener {
public:
    explicit PosixListener(int fd) : fd_(fd) { waitSet().add(fd_); }
    ~PosixListener() override {
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
        char ip[40];
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        char peer[48];
        snprintf(peer, sizeof(peer), "%s:%u", ip, ntohs(addr.sin_port));
        return new PosixConn(c, peer, true);
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
uint64_t micros() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000ull + ts.tv_nsec / 1000ull;
}
void delayMs(uint32_t ms) { usleep(ms * 1000); }
void yield() { usleep(200); }
uint32_t random32() {
    uint32_t v;
    if (getrandom(&v, sizeof(v), 0) != sizeof(v)) v = (uint32_t)rand();
    return v;
}
size_t freeHeap() { return 64u * 1024 * 1024; }
size_t freeInternalHeap() { return 256u * 1024; }
size_t minFreeInternalHeap() { return 256u * 1024; }
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
    return new PosixConn(fd, host, false);
}

// ---- threads: std::thread / std::mutex (core pinning is not needed on a PC)
int cpuCores() {
    unsigned n = std::thread::hardware_concurrency();
    return n ? (int)n : 1;
}

bool startThread(const char* name, int core, int priority, size_t stackBytes, void (*fn)(void*), void* arg) {
    try {
        std::thread(fn, arg).detach();
    } catch (...) {
        return false;
    }
    return true;
}

void* mutexCreate() { return new std::mutex(); }
void mutexLock(void* m) { ((std::mutex*)m)->lock(); }
void mutexUnlock(void* m) { ((std::mutex*)m)->unlock(); }
void mutexDestroy(void* m) { delete (std::mutex*)m; }

namespace {
struct Sem {
    std::mutex m;
    std::condition_variable cv;
    unsigned count = 0;
};
}  // namespace

void* semCreate() { return new Sem(); }
void semGive(void* s) {
    Sem* x = (Sem*)s;
    // notify under the lock: a waiter may destroy the semaphore right after it wakes
    std::lock_guard<std::mutex> g(x->m);
    x->count++;
    x->cv.notify_one();
}
bool semTake(void* s, uint32_t timeoutMs) {
    Sem* x = (Sem*)s;
    std::unique_lock<std::mutex> g(x->m);
    if (!x->cv.wait_for(g, std::chrono::milliseconds(timeoutMs), [x] { return x->count > 0; })) return false;
    x->count--;
    return true;
}
void semDestroy(void* s) { delete (Sem*)s; }

// ---- event-driven game loop: poll() on the watched sockets plus a self-pipe for wake()
namespace {
int wakePipe[2] = {-1, -1};
std::once_flag wakeOnce;
WaitStats s_waitStats;

void initWakePipe() {
    std::call_once(wakeOnce, [] {
        if (pipe(wakePipe) == 0) {
            setNonBlocking(wakePipe[0]);
            setNonBlocking(wakePipe[1]);
        }
    });
}
}  // namespace

void wake() {
    initWakePipe();
    if (wakePipe[1] < 0) return;
    char b = 1;
    ssize_t r = ::write(wakePipe[1], &b, 1);   // a full pipe already means "wake up"
    (void)r;
}

void waitForWork(uint32_t timeoutMs, bool console) {
    initWakePipe();
    lingerService(false);
    pollfd pfd[WaitSet::MAX + 2 + 8];
    int fds[WaitSet::MAX];
    bool wantWrite[WaitSet::MAX];
    int n = waitSet().snapshot(fds, wantWrite, WaitSet::MAX);
    int k = 0;
    for (int i = 0; i < n; i++) pfd[k++] = {fds[i], (short)(POLLIN | (wantWrite[i] ? POLLOUT : 0)), 0};
    if (wakePipe[0] >= 0) pfd[k++] = {wakePipe[0], POLLIN, 0};
    if (console) pfd[k++] = {0, POLLIN, 0};
    for (int i = 0; i < s_lingerN; i++) pfd[k++] = {s_linger[i].fd, POLLIN, 0};   // drain, see EOF
    uint32_t t0 = millis();
    int r = ::poll(pfd, (nfds_t)k, (int)timeoutMs);
    s_waitStats.calls++;
    s_waitStats.sleptMs += millis() - t0;
    if (r == 0) s_waitStats.timeouts++;
    for (int i = 0; r > 0 && i < n; i++) {
        if (pfd[i].revents & (POLLIN | POLLHUP | POLLERR)) s_waitStats.readable++;
        if (pfd[i].revents & POLLOUT) s_waitStats.writable++;
    }
    if (r > 0 && wakePipe[0] >= 0 && (pfd[n].revents & POLLIN)) {
        s_waitStats.wakes++;
        char buf[64];
        while (::read(wakePipe[0], buf, sizeof(buf)) > 0) {
        }
    }
    if (r > 0 && s_lingerN) lingerService(false);
}

TickSource* createTickTimer(uint32_t periodMs) { return new ClockTickSource(periodMs); }

WaitStats takeWaitStats() {
    WaitStats s = s_waitStats;
    s_waitStats = WaitStats();
    return s;
}

}  // namespace plat
}  // namespace mc
