// The status dashboard (see dashboard.h). Compiled only with MC_DASHBOARD, so a build
// without it carries neither the code nor the page.
#if MC_DASHBOARD
#include "mc/server/dashboard.h"
#include <new>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "mc/jobs.h"
#include "mc/platform.h"
#include "mc/server/dashboard_page.h"
#include "mc/server/favicon.h"
#include "mc/server/server.h"

namespace mc {

struct Dashboard::Client {
    Conn* conn = nullptr;
    uint32_t since = 0;        // accepted (reading), answered, or last event handed over
    bool writing = false;      // answering (closed when sent), or a stream
    bool stream = false;       // an event stream: stays open, takes the pushed events
    char* req = nullptr;       // REQ_CAP + OUT_CAP in one PSRAM block, while connected
    char* out = nullptr;
    size_t reqLen = 0;
    size_t outLen = 0, outSent = 0;
    const uint8_t* body = nullptr;   // a body in flash, after out
    size_t bodyLen = 0, bodySent = 0;
};

// ------------------------------------------------------------------ the snapshot
struct Dashboard::Snapshot {
    char motd[96];
    uint32_t uptime;
    float tps, mspt, wakeups;
    uint32_t tickMax, stallMax, overruns, late, skipped;
    LagProfile lag;
    uint32_t heapKb, internalKb, internalMinKb;
    int chunks, chunkCap;
    uint32_t chunkKb;
    uint64_t seed;
    int worldType, generator, radius, raining, difficulty;
    int64_t age, timeOfDay;
    int spawn[3];
    int portals;
    DragonFight dragon;
    int online, maxPlayers, nPlayers;
    struct P {
        char name[17];
        uint8_t dim, mode;
        bool op;
        double x, y, z;
        float health;
        int food, xp, ping;
    } players[MC_MAX_PLAYERS];
    int entities, mobs;
    ChunkJobStats jobs;
    int workers;
    int busyPct[4];
    int queued[PRIO_COUNT];
    Storage* storage;   // its statusLine() locks for itself: read on the worker
    Stats dash;
    int streams;
};

// Only copies: printf costs 50 to 130 us a call here on the ESP32-S3 (measured), so
// all formatting is left to formatJson() on a worker.
void Dashboard::snapshot(Snapshot& o) {
    Server& s = s_;
    uint32_t now = plat::millis();
    size_t m = 0;
    for (const char* p = s.cfg.motd; p && *p && m + 1 < sizeof(o.motd); p++) o.motd[m++] = *p;
    o.motd[m] = 0;
    o.uptime = (now - startMs_) / 1000;
    o.tps = s.tps;
    o.mspt = s.msptAvg;
    o.wakeups = s.wakeupsPerS;
    o.tickMax = s.tickMaxMs;
    o.stallMax = s.stallMaxMs;
    o.overruns = s.overruns;
    o.late = s.lateTicks;
    o.skipped = s.skippedTicks;
    o.lag = s.lag;
    o.heapKb = (uint32_t)(plat::freeHeap() / 1024);
    o.internalKb = (uint32_t)(plat::freeInternalHeap() / 1024);
    o.internalMinKb = (uint32_t)(plat::minFreeInternalHeap() / 1024);
    o.chunks = s.world.residentCount();
    o.chunkCap = s.cfg.chunkCacheSize;
    // the chunks' memory takes a walk over all their sections in PSRAM (about 7 us a
    // chunk on the ESP32-S3): 1/SWEEP_PARTS of the chunk table per snapshot, the total of the
    // last complete sweep shown
    int size = s.world.tableSize(), step = (size + SWEEP_PARTS - 1) / SWEEP_PARTS;
    for (int i = 0; i < step && sweepPos_ < size; i++, sweepPos_++)
        if (Chunk* c = s.world.slot(sweepPos_)) sweepBytes_ += c->memoryBytes();
    if (sweepPos_ >= size || chunkBytes_ == 0) {   // the first time: all at once
        for (; sweepPos_ < size; sweepPos_++)
            if (Chunk* c = s.world.slot(sweepPos_)) sweepBytes_ += c->memoryBytes();
        chunkBytes_ = sweepBytes_;
        sweepBytes_ = 0;
        sweepPos_ = 0;
    }
    o.chunkKb = (uint32_t)(chunkBytes_ / 1024);
    o.seed = s.meta.seed;
    o.worldType = s.meta.worldType;
    o.generator = s.gen.version();
    o.radius = (int)s.meta.radius;
    o.raining = s.meta.raining;
    o.difficulty = s.cfg.difficulty;
    o.age = s.meta.worldAge;
    o.timeOfDay = s.meta.timeOfDay;
    o.spawn[0] = (int)s.meta.spawnX;
    o.spawn[1] = (int)s.meta.spawnY;
    o.spawn[2] = (int)s.meta.spawnZ;
    o.portals = s.wstate.nPortals;
    o.dragon = s.wstate.dragon;
    o.online = s.onlineCount();
    o.maxPlayers = s.cfg.maxPlayers;
    o.nPlayers = 0;
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        const Player& p = s.players[i];
        if (p.state != CS_PLAY) continue;
        Snapshot::P& q = o.players[o.nPlayers++];
        memcpy(q.name, p.name, sizeof(q.name));
        q.name[sizeof(q.name) - 1] = 0;
        q.dim = p.e.dim;
        q.mode = p.gamemode;
        q.op = p.op;
        q.x = p.e.x;
        q.y = p.e.y;
        q.z = p.e.z;
        q.health = p.e.health;
        q.food = p.food;
        q.xp = p.xpLevel;
        q.ping = p.ping;
    }
    o.entities = o.mobs = 0;
    for (int i = 0; i < MC_MAX_ENTITIES; i++) {
        const Entity& e = s.entities[i];
        if (e.kind == EK_NONE || e.removed) continue;
        o.entities++;
        if (e.kind == EK_MOB) o.mobs++;
    }
    o.jobs = s.chunkJobs.stats();
    // the workers' busy share since the last snapshot (JobQueue::statusLine() keeps its
    // own baseline for /tps, so this does not disturb it)
    JobQueue& q = s.chunkJobs.queue();
    o.workers = q.workers() < 4 ? q.workers() : 4;
    uint64_t us = plat::micros(), span = us - busyBaseUs_;
    for (int i = 0; i < o.workers; i++) {
        uint64_t b = q.workerBusyUs(i);
        int pct = busyBaseUs_ && span ? (int)((b - busyBase_[i]) * 100 / span) : 0;
        o.busyPct[i] = pct > 100 ? 100 : pct;
        busyBase_[i] = b;
    }
    busyBaseUs_ = us;
    for (int p = 0; p < PRIO_COUNT; p++) o.queued[p] = q.queued((JobPriority)p);
    o.storage = s.storage;
    o.dash = stats_;
    o.streams = streams();
}

// ------------------------------------------------------------------ the JSON
namespace {
struct Json {
    char* p;
    size_t cap, n = 0;
    bool full = false;
    Json(char* out, size_t c) : p(out), cap(c) {}
    void f(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        if (full) return;
        va_list ap;
        va_start(ap, fmt);
        int r = vsnprintf(p + n, cap - n, fmt, ap);
        va_end(ap);
        if (r < 0 || (size_t)r >= cap - n) full = true;
        else n += (size_t)r;
    }
    // a number with `dec` decimals, in integer arithmetic (no %f: software doubles on
    // the ESP32)
    void num(double v, int dec) {
        long long scale = dec == 0 ? 1 : dec == 1 ? 10 : 100;
        long long q = (long long)(v * scale + (v < 0 ? -0.5 : 0.5));
        const char* sign = q < 0 ? "-" : "";
        if (q < 0) q = -q;
        if (dec == 0) f("%s%lld", sign, q);
        else f("%s%lld.%0*lld", sign, q / scale, dec, q % scale);
    }
    void str(const char* s) {   // a quoted, escaped string
        char esc[400];
        jsonEscape(s ? s : "", esc, sizeof(esc));
        f("\"%s\"", esc);
    }
};
}  // namespace

size_t Dashboard::formatJson(const Snapshot& s, char* out, size_t cap) {
    Json j(out, cap);
    j.f("{\"server\":{\"motd\":");
    j.str(s.motd);
    j.f(",\"version\":\"1.16.5\",\"protocol\":754,\"uptime\":%u}", (unsigned)s.uptime);

    char line[256];
    s.lag.format(line, sizeof(line));
    j.f(",\"perf\":{\"tps\":");
    j.num(s.tps, 2);
    j.f(",\"mspt\":");
    j.num(s.mspt, 2);
    j.f(",\"wakeups\":");
    j.num(s.wakeups, 0);
    j.f(",\"tickMax\":%u,\"stallMax\":%u,\"overruns\":%u,\"late\":%u,\"skipped\":%u,\"lag\":", (unsigned)s.tickMax,
        (unsigned)s.stallMax, (unsigned)s.overruns, (unsigned)s.late, (unsigned)s.skipped);
    j.str(line);
    j.f("}");

    j.f(",\"memory\":{\"heap\":%u,\"internal\":%u,\"internalMin\":%u,\"chunks\":%d,\"chunkKb\":%u,\"chunkCap\":%d}",
        (unsigned)s.heapKb, (unsigned)s.internalKb, (unsigned)s.internalMinKb, s.chunks, (unsigned)s.chunkKb,
        s.chunkCap);

    int crystals = 0;
    for (int i = 0; i < 16; i++) crystals += (s.dragon.crystals >> i) & 1;
    j.f(",\"world\":{\"seed\":\"%lld\",\"type\":%d,\"generator\":%d,\"radius\":%d,\"age\":%lld,\"time\":%lld,"
        "\"weather\":%d,\"difficulty\":%d,\"spawn\":[%d,%d,%d],\"portals\":%d,"
        "\"dragon\":{\"state\":%d,\"crystals\":%d,\"health\":",
        (long long)s.seed, s.worldType, s.generator, s.radius, (long long)s.age, (long long)s.timeOfDay, s.raining,
        s.difficulty, s.spawn[0], s.spawn[1], s.spawn[2], s.portals, s.dragon.state, crystals);
    j.num(s.dragon.dragonHealth, 1);
    j.f("}}");

    j.f(",\"players\":{\"online\":%d,\"max\":%d,\"list\":[", s.online, s.maxPlayers);
    for (int i = 0; i < s.nPlayers; i++) {
        const Snapshot::P& p = s.players[i];
        j.f("%s{\"name\":", i ? "," : "");
        j.str(p.name);
        j.f(",\"dim\":\"%s\",\"food\":%d,\"xp\":%d,\"mode\":%d,\"ping\":%d,\"op\":%s,\"x\":", dimensionName(p.dim),
            p.food, p.xp, p.mode, p.ping, p.op ? "true" : "false");
        j.num(p.x, 1);
        j.f(",\"y\":");
        j.num(p.y, 1);
        j.f(",\"z\":");
        j.num(p.z, 1);
        j.f(",\"health\":");
        j.num(p.health, 1);
        j.f("}");
    }
    j.f("]}");

    j.f(",\"entities\":{\"total\":%d,\"mobs\":%d,\"max\":%d}", s.entities, s.mobs, MC_MAX_ENTITIES);

    int n = s.workers ? snprintf(line, sizeof(line), "%d workers busy", s.workers)
                      : snprintf(line, sizeof(line), "jobs on the game loop");
    for (int i = 0; i < s.workers && n > 0 && (size_t)n < sizeof(line); i++)
        n += snprintf(line + n, sizeof(line) - n, " %d%%", s.busyPct[i]);
    if (n > 0 && (size_t)n < sizeof(line))
        snprintf(line + n, sizeof(line) - n, ", queued %d/%d/%d/%d; last generate %u ms, send %u ms", s.queued[0],
                 s.queued[1], s.queued[2], s.queued[3], (unsigned)((s.jobs.genUs + 500) / 1000),
                 (unsigned)((s.jobs.sendUs + 500) / 1000));
    j.f(",\"jobs\":{\"generated\":%u,\"decoded\":%u,\"sent\":%u,\"saved\":%u,\"lightResends\":%u,\"status\":",
        (unsigned)s.jobs.generated, (unsigned)s.jobs.decoded, (unsigned)s.jobs.sent, (unsigned)s.jobs.saved,
        (unsigned)s.jobs.lightResends);
    j.str(line);
    j.f("},\"storage\":");
    if (s.storage) {
        s.storage->statusLine(line, sizeof(line));
        j.str(line);
    } else {
        j.f("null");
    }

    const Stats& d = s.dash;
    j.f(",\"dashboard\":{\"streams\":%d,\"events\":%u,\"skipped\":%u,\"requests\":%u,\"refused\":%u,"
        "\"idleDropped\":%u,\"snapUs\":%u,\"snapMaxUs\":%u,\"formatUs\":%u,\"formatMaxUs\":%u,\"requestUs\":%u,"
        "\"requestMaxUs\":%u}}",
        s.streams, (unsigned)d.events, (unsigned)d.skipped, (unsigned)d.requests, (unsigned)d.refused,
        (unsigned)d.idleDropped, (unsigned)(d.events ? d.snapTotalUs / d.events : 0), (unsigned)d.snapMaxUs,
        (unsigned)(d.events ? d.formatTotalUs / d.events : 0), (unsigned)d.formatMaxUs,
        (unsigned)(d.requests ? d.totalUs / d.requests : 0), (unsigned)d.maxUs);
    return j.full ? 0 : j.n;
}

size_t Dashboard::statusJson(char* out, size_t cap) {
    Snapshot* s = (Snapshot*)plat::bigAlloc(sizeof(Snapshot));   // ~1.5 KB: not on the stack
    if (!s) return 0;
    new (s) Snapshot();
    snapshot(*s);
    size_t n = formatJson(*s, out, cap);
    plat::bigFree(s);
    return n;
}

// ------------------------------------------------------------------ pushed events
// Formats the snapshot on a worker; finish() hands the event to the streams.
class DashboardJob : public Job {
public:
    explicit DashboardJob(Dashboard& d) : d_(d) {}
    void run(WorkerScratch&) override {
        static const char PRE[] = "data: ";
        uint64_t t0 = plat::micros();
        size_t n = Dashboard::formatJson(*d_.snap_, d_.event_ + 6, Dashboard::OUT_CAP - 6 - 3);
        if (n) {
            memcpy(d_.event_, PRE, 6);
            memcpy(d_.event_ + 6 + n, "\n\n", 3);
            len_ = 6 + n + 2;
        }
        us_ = (uint32_t)(plat::micros() - t0);
    }
    void finish() override { d_.eventReady(len_, us_); }
    const char* kind() const override { return "dashboard"; }

private:
    Dashboard& d_;
    size_t len_ = 0;
    uint32_t us_ = 0;
};

void Dashboard::afterLoop(uint32_t freeMs) {
    if (jobInFlight_ || !clients_ || streams() == 0) return;
    uint32_t now = plat::millis();
    uint32_t since = now - lastPushMs_;
    if (lastPushMs_ && since < pushMs_) return;
    // wait for a pass with time to spare before the next tick, unless that takes too long
    if (lastPushMs_ && freeMs < SLACK_MS && since < pushMs_ + LATE_MS) return;
    if (!snap_) {
        snap_ = (Snapshot*)plat::bigAlloc(sizeof(Snapshot));
        event_ = (char*)plat::bigAlloc(OUT_CAP);
        if (!snap_ || !event_) {
            plat::bigFree(snap_);
            plat::bigFree(event_);
            snap_ = nullptr;
            event_ = nullptr;
            return;
        }
        new (snap_) Snapshot();
    }
    uint64_t t0 = plat::micros();
    snapshot(*snap_);
    uint32_t us = (uint32_t)(plat::micros() - t0);
    stats_.snapTotalUs += us;
    if (us > stats_.snapMaxUs) stats_.snapMaxUs = us;
    lastPushMs_ = now ? now : 1;
    jobInFlight_ = true;
    s_.chunkJobs.queue().submit(new DashboardJob(*this), PRIO_BACKGROUND);
}

void Dashboard::eventReady(size_t len, uint32_t formatUs) {
    jobInFlight_ = false;
    if (!len) {
        stats_.errors++;
        return;
    }
    stats_.events++;
    stats_.formatTotalUs += formatUs;
    if (formatUs > stats_.formatMaxUs) stats_.formatMaxUs = formatUs;
    uint32_t now = plat::millis();
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client& c = clients_[i];
        if (!c.conn || !c.stream) continue;
        if (c.outSent < c.outLen) {   // still sending an older one: this one is skipped
            stats_.skipped++;
            continue;
        }
        memcpy(c.out, event_, len);
        c.outLen = len;
        c.outSent = 0;
        c.since = now;
        if (flush(c) && c.outSent < c.outLen) drop(c);   // write error
    }
}

// ------------------------------------------------------------------ HTTP
Dashboard::Dashboard(Server& s) : s_(s) {}

Dashboard::~Dashboard() {
    // a job still formatting uses snap_ and event_ and calls back
    if (jobInFlight_) s_.chunkJobs.queue().drain();
    if (clients_)
        for (int i = 0; i < MAX_CLIENTS; i++) drop(clients_[i]);
    delete[] clients_;
    delete listener_;
    plat::bigFree(snap_);
    plat::bigFree(event_);
}

bool Dashboard::begin(uint16_t port) {
    listener_ = plat::listen(port);
    if (!listener_) {
        MC_LOGE("dashboard: cannot listen on port %u", port);
        return false;
    }
    if (!clients_) clients_ = new Client[MAX_CLIENTS];
    startMs_ = plat::millis();
    MC_LOGI("dashboard on port %u", port);
    return true;
}

int Dashboard::streams() const {
    int n = 0;
    if (clients_)
        for (int i = 0; i < MAX_CLIENTS; i++) n += clients_[i].conn && clients_[i].stream;
    return n;
}

void Dashboard::drop(Client& c) {
    if (c.conn) {
        c.conn->close();
        delete c.conn;
    }
    if (c.req) plat::bigFree(c.req);
    c = Client();
}

bool Dashboard::adopt(Conn* conn) {
    if (!clients_) clients_ = new Client[MAX_CLIENTS];
    // all busy: a connection that has not sent a byte for a while (browsers open spare
    // ones ahead of time) makes room, the oldest first
    uint32_t now = plat::millis();
    int idle = -1, free = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        const Client& c = clients_[i];
        if (!c.conn) free = i;
        else if (!c.writing && c.reqLen == 0 && now - c.since >= IDLE_MS &&
                 (idle < 0 || (int32_t)(c.since - clients_[idle].since) < 0))
            idle = i;
    }
    if (free < 0 && idle >= 0) {
        drop(clients_[idle]);
        stats_.idleDropped++;
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client& c = clients_[i];
        if (c.conn) continue;
        c.req = (char*)plat::bigAlloc(REQ_CAP + OUT_CAP);
        if (!c.req) break;
        c.out = c.req + REQ_CAP;
        c.conn = conn;
        c.since = plat::millis();
        return true;
    }
    stats_.refused++;
    conn->close();
    delete conn;
    return false;
}

void Dashboard::poll() {
    if (!clients_) return;
    for (int n = 0; listener_ && n < 4; n++) {
        Conn* c = listener_->accept();
        if (!c) break;
        adopt(c);
    }
    uint32_t now = plat::millis();
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client& c = clients_[i];
        if (!c.conn) continue;
        if (!c.conn->connected()) {
            drop(c);
            continue;
        }
        if (!c.writing) {
            handle(c);
            if (c.conn && !c.writing && now - c.since > REQUEST_TIMEOUT_MS) drop(c);
            continue;
        }
        if (c.stream) {
            uint8_t sink[64];   // a stream's browser sends nothing more: this notices it leave
            if (c.conn->read(sink, sizeof(sink)) < 0) {
                drop(c);
                continue;
            }
            if (flush(c) && c.outSent < c.outLen) drop(c);   // write error
            else if (c.outSent < c.outLen && now - c.since > STREAM_STUCK_MS) {
                stats_.errors++;
                drop(c);
            }
            continue;
        }
        if (flush(c)) drop(c);   // Connection: close
        else if (now - c.since > RESPONSE_TIMEOUT_MS) {
            stats_.errors++;
            drop(c);
        }
    }
}

// reads what arrived; once the headers are complete (or fill the buffer), answers
void Dashboard::handle(Client& c) {
    for (;;) {
        if (c.reqLen >= REQ_CAP - 1) break;   // enough: the request line and most headers
        int r = c.conn->read((uint8_t*)c.req + c.reqLen, REQ_CAP - 1 - c.reqLen);
        if (r < 0) {
            drop(c);
            return;
        }
        if (r == 0) break;
        c.reqLen += (size_t)r;
    }
    c.req[c.reqLen] = 0;
    if (c.reqLen < REQ_CAP - 1 && !strstr(c.req, "\r\n\r\n")) return;   // more to come
    uint64_t t0 = plat::micros();
    respond(c);
    uint32_t us = (uint32_t)(plat::micros() - t0);
    stats_.requests++;
    stats_.totalUs += us;
    if (us > stats_.maxUs) stats_.maxUs = us;
    if (c.conn && (flush(c) && (!c.stream || c.outSent < c.outLen))) drop(c);
}

static bool startsWith(const char* s, const char* p) { return strncmp(s, p, strlen(p)) == 0; }

// the value of a header (lower-case name with the colon), or nullptr
static const char* header(const char* req, const char* name) {
    size_t n = strlen(name);
    for (const char* p = strstr(req, "\r\n"); p; p = strstr(p + 2, "\r\n")) {
        const char* h = p + 2;
        size_t i = 0;
        while (i < n && h[i] && (h[i] | 0x20) == name[i]) i++;
        if (i == n) {
            h += n;
            while (*h == ' ') h++;
            return h;
        }
    }
    return nullptr;
}

static size_t base64Decode(const char* in, uint8_t* out, size_t cap) {
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    for (; *in && *in != '='; in++) {
        char ch = *in;
        int v = ch >= 'A' && ch <= 'Z' ? ch - 'A' : ch >= 'a' && ch <= 'z' ? ch - 'a' + 26
              : ch >= '0' && ch <= '9' ? ch - '0' + 52 : ch == '+' ? 62 : ch == '/' ? 63 : -1;
        if (v < 0) continue;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n == cap) return 0;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return n;
}

void Dashboard::respond(Client& c) {
    c.writing = true;
    c.since = plat::millis();
    const char* status = "200 OK";
    const char* type = "text/plain; charset=utf-8";
    const char* extra = "";
    char etagLine[64] = "";
    size_t bodyLen = 0;
    bool head = startsWith(c.req, "HEAD ");
    // the body goes after the headers in out; leave them room
    const size_t HDR = 320;
    char* body = c.out + HDR;
    size_t bodyCap = OUT_CAP - HDR;
    c.body = nullptr;

    char path[64] = "";
    const char* sp = strchr(c.req, ' ');
    if (sp) {
        size_t i = 0;
        for (const char* p = sp + 1; *p && *p != ' ' && *p != '?' && i + 1 < sizeof(path); p++) path[i++] = *p;
        path[i] = 0;
    }
    if (!startsWith(c.req, "GET ") && !head) {
        status = "405 Method Not Allowed";
        extra = "Allow: GET, HEAD\r\n";
        bodyLen = (size_t)snprintf(body, bodyCap, "only GET and HEAD\n");
    } else if (!strcmp(path, "/api/events") && !head) {
        if (streams() >= MAX_STREAMS) {
            status = "503 Service Unavailable";
            bodyLen = (size_t)snprintf(body, bodyCap, "too many open pages\n");
        } else {
            // no length: the events follow as they come, until either side closes.
            // retry: the browser reconnects after 2 s if the connection drops
            int h = snprintf(c.out, OUT_CAP,
                             "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\n"
                             "Connection: close\r\n\r\nretry: 2000\n\n");
            c.outLen = h > 0 ? (size_t)h : 0;
            c.outSent = 0;
            c.bodyLen = c.bodySent = 0;
            c.stream = true;
            lastPushMs_ = 0;   // a new page gets its first event right away
            return;
        }
    } else if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
        snprintf(etagLine, sizeof(etagLine), "ETag: %s\r\n", DASHBOARD_PAGE_ETAG);
        const char* inm = header(c.req, "if-none-match:");
        if (inm && startsWith(inm, DASHBOARD_PAGE_ETAG)) {
            status = "304 Not Modified";
            type = nullptr;
        } else {
            type = "text/html; charset=utf-8";
            extra = "Content-Encoding: gzip\r\n";
            c.body = DASHBOARD_PAGE_GZ;
            bodyLen = DASHBOARD_PAGE_GZ_LEN;
        }
    } else if (!strcmp(path, "/api/status")) {
        type = "application/json";
        bodyLen = statusJson(body, bodyCap);
        if (!bodyLen) {
            status = "500 Internal Server Error";
            type = "text/plain; charset=utf-8";
            bodyLen = (size_t)snprintf(body, bodyCap, "status too large\n");
            stats_.errors++;
        }
    } else if (!strcmp(path, "/favicon.png") || !strcmp(path, "/favicon.ico")) {
        const char* b64 = strchr(FAVICON, ',');
        bodyLen = b64 ? base64Decode(b64 + 1, (uint8_t*)body, bodyCap) : 0;
        type = "image/png";
        extra = "Cache-Control: max-age=86400\r\n";
    } else {
        status = "404 Not Found";
        bodyLen = (size_t)snprintf(body, bodyCap, "not found\n");
    }

    char typeLine[64] = "";
    if (type) snprintf(typeLine, sizeof(typeLine), "Content-Type: %s\r\n", type);
    int h = snprintf(c.out, HDR,
                     "HTTP/1.1 %s\r\n%s%s%sContent-Length: %u\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n",
                     status, typeLine, extra, etagLine, (unsigned)bodyLen);
    if (h < 0 || (size_t)h >= HDR) h = 0;   // cannot happen with these headers
    c.outLen = (size_t)h;
    c.outSent = 0;
    if (c.body) {   // from flash
        c.bodyLen = head ? 0 : bodyLen;
    } else {        // move it up against the headers
        if (!head) memmove(c.out + h, body, bodyLen);
        c.outLen += head ? 0 : bodyLen;
        c.bodyLen = 0;
    }
    c.bodySent = 0;
}

// true when everything was sent, or the connection failed (then outSent < outLen)
bool Dashboard::flush(Client& c) {
    while (c.outSent < c.outLen) {
        int w = c.conn->write((const uint8_t*)c.out + c.outSent, c.outLen - c.outSent);
        if (w < 0) return true;   // gone: drop it
        if (w == 0) return false;
        c.outSent += (size_t)w;
    }
    while (c.bodySent < c.bodyLen) {
        int w = c.conn->write(c.body + c.bodySent, c.bodyLen - c.bodySent);
        if (w < 0) return true;
        if (w == 0) return false;
        c.bodySent += (size_t)w;
    }
    return true;
}

}  // namespace mc
#endif  // MC_DASHBOARD
