// The status dashboard (see dashboard.h). Compiled only with MC_DASHBOARD, so a build
// without it carries neither the code nor the page.
#if MC_DASHBOARD
#include "mc/server/dashboard.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "mc/platform.h"
#include "mc/server/dashboard_page.h"
#include "mc/server/favicon.h"
#include "mc/server/server.h"

namespace mc {

struct Dashboard::Client {
    Conn* conn = nullptr;
    uint32_t since = 0;        // accepted (reading) or answered (writing)
    bool writing = false;
    char* req = nullptr;       // REQ_CAP + OUT_CAP in one PSRAM block, while connected
    char* out = nullptr;
    size_t reqLen = 0;
    size_t outLen = 0, outSent = 0;
    const uint8_t* body = nullptr;   // a body in flash, after out
    size_t bodyLen = 0, bodySent = 0;
};

Dashboard::~Dashboard() {
    if (clients_)
        for (int i = 0; i < MAX_CLIENTS; i++) drop(clients_[i]);
    delete[] clients_;
    delete listener_;
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
        }
        if (c.conn && c.writing) {
            if (flush(c)) drop(c);   // Connection: close
            else if (now - c.since > RESPONSE_TIMEOUT_MS) {
                stats_.errors++;
                drop(c);
            }
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
    // a number with `dec` decimals in integer arithmetic: printf's %f takes about
    // 50 us a number on the ESP32 (software double precision)
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

size_t Dashboard::statusJson(char* out, size_t cap) {
    Server& s = s_;
    Json j(out, cap);
    j.f("{\"server\":{\"motd\":");
    j.str(s.cfg.motd);
    j.f(",\"version\":\"1.16.5\",\"protocol\":754,\"uptime\":%u}", (unsigned)((plat::millis() - startMs_) / 1000));

    char lag[256];
    s.lag.format(lag, sizeof(lag));
    j.f(",\"perf\":{\"tps\":");
    j.num(s.tps, 2);
    j.f(",\"mspt\":");
    j.num(s.msptAvg, 2);
    j.f(",\"wakeups\":");
    j.num(s.wakeupsPerS, 0);
    j.f(",\"tickMax\":%u,\"stallMax\":%u,\"overruns\":%u,\"late\":%u,\"skipped\":%u,\"lag\":",
        (unsigned)s.tickMaxMs, (unsigned)s.stallMaxMs, (unsigned)s.overruns, (unsigned)s.lateTicks,
        (unsigned)s.skippedTicks);
    j.str(lag);
    j.f("}");

    // the chunks' memory takes a walk over all their sections in PSRAM (about 7 us a
    // chunk on the ESP32-S3): every 10 s is enough for a page polled every 2 s
    uint32_t now = plat::millis();
    if (!chunkBytesMs_ || now - chunkBytesMs_ >= 10000) {
        chunkBytes_ = s.world.residentBytes();
        chunkBytesMs_ = now ? now : 1;
    }
    j.f(",\"memory\":{\"heap\":%u,\"internal\":%u,\"internalMin\":%u,\"chunks\":%d,\"chunkKb\":%u,\"chunkCap\":%d}",
        (unsigned)(plat::freeHeap() / 1024), (unsigned)(plat::freeInternalHeap() / 1024),
        (unsigned)(plat::minFreeInternalHeap() / 1024), s.world.residentCount(),
        (unsigned)(chunkBytes_ / 1024), s.cfg.chunkCacheSize);

    const DragonFight& d = s.wstate.dragon;
    int crystals = 0;
    for (int i = 0; i < 16; i++) crystals += (d.crystals >> i) & 1;
    j.f(",\"world\":{\"seed\":\"%lld\",\"type\":%d,\"generator\":%d,\"radius\":%d,\"age\":%lld,\"time\":%lld,"
        "\"weather\":%d,\"difficulty\":%d,\"spawn\":[%d,%d,%d],\"portals\":%d,"
        "\"dragon\":{\"state\":%d,\"crystals\":%d,\"health\":",
        (long long)s.meta.seed, s.meta.worldType, s.gen.version(), (int)s.meta.radius, (long long)s.meta.worldAge,
        (long long)s.meta.timeOfDay, s.meta.raining, s.cfg.difficulty, (int)s.meta.spawnX, (int)s.meta.spawnY,
        (int)s.meta.spawnZ, s.wstate.nPortals, d.state, crystals);
    j.num(d.dragonHealth, 1);
    j.f("}}");

    j.f(",\"players\":{\"online\":%d,\"max\":%d,\"list\":[", s.onlineCount(), s.cfg.maxPlayers);
    bool first = true;
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        const Player& p = s.players[i];
        if (p.state != CS_PLAY) continue;
        j.f("%s{\"name\":", first ? "" : ",");
        first = false;
        j.str(p.name);
        j.f(",\"dim\":\"%s\",\"food\":%d,\"xp\":%d,\"mode\":%d,\"ping\":%d,\"op\":%s,\"x\":",
            dimensionName(p.e.dim), p.food, p.xpLevel, p.gamemode, p.ping, p.op ? "true" : "false");
        j.num(p.e.x, 1);
        j.f(",\"y\":");
        j.num(p.e.y, 1);
        j.f(",\"z\":");
        j.num(p.e.z, 1);
        j.f(",\"health\":");
        j.num(p.e.health, 1);
        j.f("}");
    }
    j.f("]}");

    int total = 0;
    for (int i = 0; i < MC_MAX_ENTITIES; i++)
        if (s.entities[i].kind != EK_NONE && !s.entities[i].removed) total++;
    j.f(",\"entities\":{\"total\":%d,\"mobs\":%d,\"max\":%d}", total, s.mobCount(), MC_MAX_ENTITIES);

    const ChunkJobStats& js = s.chunkJobs.stats();
    char line[256];
    s.chunkJobs.statusLine(line, sizeof(line));
    j.f(",\"jobs\":{\"generated\":%u,\"decoded\":%u,\"sent\":%u,\"saved\":%u,\"lightResends\":%u,\"status\":",
        (unsigned)js.generated, (unsigned)js.decoded, (unsigned)js.sent, (unsigned)js.saved,
        (unsigned)js.lightResends);
    j.str(line);
    j.f("},\"storage\":");
    if (s.storage) {
        s.storage->statusLine(line, sizeof(line));
        j.str(line);
    } else {
        j.f("null");
    }
    j.f(",\"dashboard\":{\"requests\":%u,\"refused\":%u,\"idleDropped\":%u,\"maxUs\":%u,\"avgUs\":%u}}",
        (unsigned)stats_.requests, (unsigned)stats_.refused, (unsigned)stats_.idleDropped, (unsigned)stats_.maxUs,
        (unsigned)(stats_.requests ? stats_.totalUs / stats_.requests : 0));
    return j.full ? 0 : j.n;
}

}  // namespace mc
#endif  // MC_DASHBOARD
