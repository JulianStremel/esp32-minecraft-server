// A read-only status page served by the server itself (builds with MC_DASHBOARD only).
// HTTP/1.1 on its own port, handled on the game loop like the players' sockets: no task
// of its own. Three connections at a time; each takes 7 KB of PSRAM while it is open
// and nothing in between. Routes:
//   GET /             the page (tools/dashboard/index.html, gzipped in flash)
//   GET /api/status   the server's state as JSON (the page polls it every 2 s)
//   GET /favicon.png  the server list icon
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace mc {

class Conn;
class Listener;
class Server;

class Dashboard {
public:
    static constexpr int MAX_CLIENTS = 3;
    static constexpr size_t REQ_CAP = 1024;   // the request line and headers we read
    static constexpr size_t OUT_CAP = 6144;   // response headers + the JSON
    static constexpr uint32_t REQUEST_TIMEOUT_MS = 5000;
    static constexpr uint32_t RESPONSE_TIMEOUT_MS = 15000;
    static constexpr uint32_t IDLE_MS = 250;   // silent this long: may be closed for a newcomer

    explicit Dashboard(Server& s) : s_(s) {}
    ~Dashboard();
    bool begin(uint16_t port);
    // Game loop: new connections, requests, pending output. Cheap when idle.
    void poll();
    // Serves an already accepted connection (owned from now on); false: all busy (it
    // is closed then).
    bool adopt(Conn* c);

    // the JSON of /api/status; returns its length (0 if it did not fit)
    size_t statusJson(char* out, size_t cap);

    struct Stats {
        uint32_t requests = 0, refused = 0, errors = 0;
        uint32_t idleDropped = 0;   // connections that sent nothing, closed to make room
        uint32_t maxUs = 0;   // the slowest request (parsing and building the response)
        uint64_t totalUs = 0;
    };
    const Stats& stats() const { return stats_; }

private:
    struct Client;
    void handle(Client& c);
    void respond(Client& c);
    bool flush(Client& c);   // true when everything was sent
    void drop(Client& c);

    Server& s_;
    Listener* listener_ = nullptr;
    Client* clients_ = nullptr;   // MAX_CLIENTS
    uint32_t startMs_ = 0;
    size_t chunkBytes_ = 0;       // World::residentBytes(), refreshed every 10 s
    uint32_t chunkBytesMs_ = 0;
    Stats stats_;
};

}  // namespace mc
