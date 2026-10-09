// A status page served by the server itself (builds with MC_DASHBOARD only). HTTP/1.1 on
// its own port, handled on the game loop like the players' sockets: no task of its own.
// Routes:
//   GET /              the page (tools/dashboard/index.html, gzipped in flash)
//   GET /api/events    the server's state pushed as Server-Sent Events, once a second;
//                      with the token, the console's output too (`event: log`, the
//                      lines kept so far first)
//   GET /api/status    the same JSON once (for scripts; the page falls back to it)
//   GET /api/history   the last few minutes of TPS, tick time, free memory and players
//                      (sampled every second whether a page is open or not)
//   GET /favicon.png   the server list icon
//   POST /api/login    checks the token ({"ok":true}, 401, or 429 when locked)
//   POST /api/action   {"action": ..., "value"/"player": ...}: save, kick, difficulty,
//                      time, weather, spawning, pvp, perfbar; runs on the game loop as
//                      the console's commands do
//   POST /api/console  {"command": ...}: a line for the server console, as typed on
//                      the serial console (operator rights); its output comes back
//                      in the log events
// The state is open to everyone on the network; the log and POST requests need the token
// (Authorization: Bearer <token>; plain HTTP, so only on a network you trust). Five
// wrong tokens in a row lock the actions for 30 seconds.
//
// The pushed state costs the game loop only a copy of numbers: once a second, when the
// loop has time before its next tick, it takes a Snapshot; a worker job formats it as
// JSON (printf is the expensive part) and the job's finish() hands the event to the
// streams. Nothing is snapshotted while no page is open.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "mc/log_ring.h"

namespace mc {

class Conn;
class Listener;
class Server;

class Dashboard {
public:
    static constexpr int MAX_CLIENTS = 4;      // connections: requests and event streams
    static constexpr int MAX_STREAMS = 3;      // of them event streams (open pages)
    static constexpr size_t REQ_CAP = 1536;    // the request line, headers and a small body
    static constexpr size_t OUT_CAP = 6144;    // response headers + the JSON
    static constexpr uint32_t REQUEST_TIMEOUT_MS = 5000;
    static constexpr uint32_t RESPONSE_TIMEOUT_MS = 15000;
    static constexpr uint32_t IDLE_MS = 250;   // silent this long: may be closed for a newcomer
    static constexpr uint32_t STREAM_STUCK_MS = 30000;   // an event not taken this long: closed
    static constexpr uint32_t SLACK_MS = 5;    // snapshot when the next tick is this far away...
    static constexpr uint32_t LATE_MS = 3000;  // ... or anyway once an event is this late
    static constexpr size_t LOG_CAP = 64 * 1024;     // the log lines kept (PSRAM)
    static constexpr uint32_t LOG_BATCH_MS = 100;    // new lines go out at most this often

    explicit Dashboard(Server& s);
    ~Dashboard();
    bool begin(uint16_t port);
    // Game loop: new connections, requests, pending output. Cheap when idle.
    void poll();
    // Game loop, after a pass that leaves freeMs until the next tick: starts the next
    // event for the streams when one is due.
    void afterLoop(uint32_t freeMs);
    // Serves an already accepted connection (owned from now on); false: all busy (it
    // is closed then).
    bool adopt(Conn* c);
    void setPushInterval(uint32_t ms) { pushMs_ = ms; }   // default 1000
    int streams() const;
    void setToken(const char* t);
    const char* token() const { return token_; }
    // the history: one sample a second for the last HISTORY seconds
    static constexpr int HISTORY = 180;
    struct Sample { float tps, mspt; uint32_t heapKb; uint8_t players; };
    void sample();   // game loop, about once a second

    // The server's state as plain values; snapshot() fills it on the game loop, and
    // formatJson() turns it into JSON on any thread.
    struct Snapshot;
    void snapshot(Snapshot& s);
    static size_t formatJson(const Snapshot& s, char* out, size_t cap);
    // snapshot + format on the game loop (/api/status); its length, 0 if it did not fit
    size_t statusJson(char* out, size_t cap);

    struct Stats {
        uint32_t requests = 0, refused = 0, errors = 0;
        uint32_t idleDropped = 0;   // connections that sent nothing, closed to make room
        uint32_t maxUs = 0;         // the slowest request on the game loop
        uint64_t totalUs = 0;
        uint32_t events = 0;        // events pushed (one per snapshot, to every stream)
        uint32_t skipped = 0;       // ... not sent to a stream still busy with the last one
        uint32_t snapMaxUs = 0;     // game loop: taking a snapshot
        uint64_t snapTotalUs = 0;
        uint32_t formatMaxUs = 0;   // worker: formatting it
        uint64_t formatTotalUs = 0;
        uint32_t actions = 0;       // POST /api/action carried out
        uint32_t denied = 0;        // requests with a wrong or missing token
        uint32_t commands = 0;      // POST /api/console lines run
        uint32_t logEvents = 0;     // log events sent
        uint32_t logLines = 0;      // lines in them
        uint64_t logTotalUs = 0;    // formatting them (game loop)
        uint32_t logMaxUs = 0;
    };
    const Stats& stats() const { return stats_; }

private:
    friend class DashboardJob;
    struct Client;
    void handle(Client& c);
    void respond(Client& c);
    bool flush(Client& c);   // true when everything was sent
    void drop(Client& c);
    void eventReady(size_t len, uint32_t formatUs);   // the job's finish()
    bool authorized(const char* req);                 // the token, and the lockout
    // POST /api/action: what happened, as JSON, into body
    size_t action(const char* json, char* body, size_t cap, const char*& status);
    size_t historyJson(char* out, size_t cap) const;
    void pushLog(Client& c);   // the next log lines a console stream lacks

    Server& s_;
    Listener* listener_ = nullptr;
    Client* clients_ = nullptr;   // MAX_CLIENTS
    uint32_t startMs_ = 0;
    // the chunks' memory, counted over SWEEP_PARTS snapshots
    static constexpr int SWEEP_PARTS = 32;   // a sweep every ~30 s
    size_t chunkBytes_ = 0;       // the last complete sweep
    size_t sweepBytes_ = 0;
    int sweepPos_ = 0;
    uint64_t busyBase_[4] = {};   // the workers' busy time at the last snapshot
    uint64_t busyBaseUs_ = 0;
    // the event in the making: one at a time, both in PSRAM
    Snapshot* snap_ = nullptr;
    char* event_ = nullptr;       // OUT_CAP
    bool jobInFlight_ = false;
    uint32_t pushMs_ = 1000;
    uint32_t lastPushMs_ = 0;     // last snapshot for the streams (0: none yet)
    Stats stats_;
    char token_[40] = "";
    int failures_ = 0;            // wrong tokens in a row
    uint32_t lockedUntil_ = 0;    // millis; 0: not locked
    Sample* history_ = nullptr;   // HISTORY entries in PSRAM, a ring
    int historyLen_ = 0, historyPos_ = 0;
    uint32_t lastSampleMs_ = 0;
    LogRing log_;
};

}  // namespace mc
