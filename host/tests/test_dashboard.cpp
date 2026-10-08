// The HTTP status dashboard (MC_DASHBOARD builds): requests through an in-memory
// connection that accepts only a few bytes per write, as a full socket would.
#if MC_DASHBOARD
#include <zlib.h>
#include <string.h>
#include <string>
#include "testing.h"
#include "mc/platform.h"
#include "mc/server/dashboard.h"
#include "mc/server/dashboard_page.h"
#include "mc/server/server.h"

using namespace mc;

namespace {

// writes go to *out, which outlives the connection (the dashboard deletes it on close)
struct MemConn : Conn {
    std::string in;
    std::string* out = nullptr;
    size_t inPos = 0;
    size_t writeChunk = 700;   // bytes a write takes at most (0: socket full)
    bool open = true;
    bool* closedFlag = nullptr;
    int read(uint8_t* buf, size_t n) override {
        if (!open) return -1;
        size_t k = std::min(n, in.size() - inPos);
        memcpy(buf, in.data() + inPos, k);
        inPos += k;
        return (int)k;
    }
    int write(const uint8_t* buf, size_t n) override {
        if (!open) return -1;
        size_t k = std::min(n, writeChunk);
        if (out) out->append((const char*)buf, k);
        return (int)k;
    }
    bool connected() override { return open; }
    void close() override {
        open = false;
        if (closedFlag) *closedFlag = true;
    }
};

Server* dashServer() {
    static Server* s = nullptr;
    if (s) return s;
    s = new Server();
    ServerConfig cfg;
    cfg.port = 0;
    cfg.worldType = WORLD_FLAT;
    cfg.spawnMobs = false;
    cfg.seed = 77;
    cfg.motd = "Dash \"test\"";
    if (!s->begin(cfg, nullptr)) return nullptr;
    return s;
}

// sends `req`, polls until the dashboard closes the connection; the raw response
std::string fetch(Dashboard& d, const std::string& req, size_t chunk = 700) {
    MemConn* c = new MemConn();   // owned by the dashboard
    std::string result;
    c->in = req;
    c->writeChunk = chunk;
    c->out = &result;
    bool closed = false;
    c->closedFlag = &closed;
    if (!d.adopt(c)) return std::string();
    for (int i = 0; i < 5000 && !closed; i++) d.poll();
    return closed ? result : std::string();
}

std::string body(const std::string& resp) {
    size_t p = resp.find("\r\n\r\n");
    return p == std::string::npos ? std::string() : resp.substr(p + 4);
}

}  // namespace

TEST(dashboard_serves_the_gzipped_page_with_an_etag) {
    Server* s = dashServer();
    CHECK(s != nullptr);
    if (!s) return;
    Dashboard d(*s);
    std::string r = fetch(d, "GET / HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip\r\n\r\n");
    CHECK(r.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
    CHECK(r.find("Content-Encoding: gzip\r\n") != std::string::npos);
    CHECK(r.find("Content-Type: text/html") != std::string::npos);
    std::string b = body(r);
    CHECK_EQ(b.size(), DASHBOARD_PAGE_GZ_LEN);
    // it unpacks to an HTML page that polls the JSON
    std::string html(64 * 1024, '\0');
    z_stream z;
    memset(&z, 0, sizeof(z));
    inflateInit2(&z, 16 + MAX_WBITS);
    z.next_in = (Bytef*)b.data();
    z.avail_in = (uInt)b.size();
    z.next_out = (Bytef*)&html[0];
    z.avail_out = (uInt)html.size();
    CHECK_EQ(inflate(&z, Z_FINISH), Z_STREAM_END);
    html.resize(z.total_out);
    inflateEnd(&z);
    CHECK(html.rfind("<!doctype html>", 0) == 0);
    CHECK(html.find("/api/status") != std::string::npos);
    // a revalidation with the same ETag: 304 and no body
    std::string again = fetch(d, std::string("GET / HTTP/1.1\r\nIf-None-Match: ") + DASHBOARD_PAGE_ETAG + "\r\n\r\n");
    CHECK(again.rfind("HTTP/1.1 304 Not Modified\r\n", 0) == 0);
    CHECK(body(again).empty());
}

TEST(dashboard_status_json_has_the_servers_state) {
    Server* s = dashServer();
    if (!s) return;
    Dashboard d(*s);
    // tiny writes: the response goes out over many polls
    std::string got = fetch(d, "GET /api/status HTTP/1.1\r\nHost: x\r\n\r\n", 37);
    CHECK(got.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
    std::string j = body(got);
    printf("    %u bytes of JSON, %u us to build\n", (unsigned)j.size(), (unsigned)d.stats().maxUs);
    size_t cl = got.find("Content-Length: ");
    CHECK(cl != std::string::npos);
    if (cl != std::string::npos) CHECK_EQ((size_t)atoi(got.c_str() + cl + 16), j.size());
    CHECK(j.front() == '{' && j.back() == '}');
    CHECK(j.find("\"motd\":\"Dash \\\"test\\\"\"") != std::string::npos);   // escaped
    CHECK(j.find("\"seed\":\"77\"") != std::string::npos);
    CHECK(j.find("\"tps\":") != std::string::npos);
    CHECK(j.find("\"list\":[]") != std::string::npos);
    CHECK(j.find("\"storage\":null") != std::string::npos);
    // balanced braces and brackets outside strings
    int depth = 0, minDepth = 1;
    bool inStr = false;
    for (size_t i = 0; i < j.size(); i++) {
        char ch = j[i];
        if (inStr) {
            if (ch == '\\') i++;
            else if (ch == '"') inStr = false;
            continue;
        }
        if (ch == '"') inStr = true;
        else if (ch == '{' || ch == '[') depth++;
        else if (ch == '}' || ch == ']') {
            depth--;
            if (i + 1 < j.size() && depth < minDepth) minDepth = depth;
        }
    }
    CHECK_EQ(depth, 0);
    CHECK(minDepth >= 1);   // one top-level object
}

TEST(dashboard_status_json_fits_every_player_slot) {
    Server* s = dashServer();
    if (!s) return;
    Dashboard d(*s);
    // every slot in play with the longest names: must still fit the buffer
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        s->players[i].state = CS_PLAY;
        snprintf(s->players[i].name, sizeof(s->players[i].name), "Player_%09d", i);
        s->players[i].e.x = -29999984.5;
        s->players[i].e.z = 29999984.5;
    }
    static char buf[Dashboard::OUT_CAP];
    size_t n = d.statusJson(buf, sizeof(buf) - 320);   // the room respond() leaves
    for (int i = 0; i < MC_MAX_PLAYERS; i++) s->players[i].state = CS_FREE;
    printf("    %d players: %u bytes of JSON (room for %u)\n", MC_MAX_PLAYERS, (unsigned)n, (unsigned)(sizeof(buf) - 320));
    CHECK(n > 0);
    char last[24];
    snprintf(last, sizeof(last), "Player_%09d", MC_MAX_PLAYERS - 1);
    CHECK(strstr(buf, last) != nullptr);
    CHECK(strstr(buf, "\"x\":-29999984.5,") != nullptr);   // fixed-point numbers
    CHECK(strstr(buf, "\"z\":29999984.5,") != nullptr);
    CHECK(strstr(buf, "\"health\":20.0}") != nullptr);
}

TEST(dashboard_answers_errors_and_limits_clients) {
    Server* s = dashServer();
    if (!s) return;
    Dashboard d(*s);
    CHECK(fetch(d, "GET /nope HTTP/1.1\r\n\r\n").rfind("HTTP/1.1 404 ", 0) == 0);
    CHECK(fetch(d, "POST /api/status HTTP/1.1\r\n\r\n").rfind("HTTP/1.1 405 ", 0) == 0);
    std::string h = fetch(d, "HEAD /api/status HTTP/1.1\r\n\r\n");
    CHECK(h.rfind("HTTP/1.1 200 ", 0) == 0);
    CHECK(body(h).empty());
    std::string ico = fetch(d, "GET /favicon.png HTTP/1.1\r\n\r\n", 1 << 20);
    CHECK(body(ico).compare(0, 8, "\x89PNG\r\n\x1a\n") == 0);
    // headers longer than the buffer: answered from what fit
    std::string big = "GET /api/status HTTP/1.1\r\nX-Pad: " + std::string(3000, 'a') + "\r\n\r\n";
    CHECK(fetch(d, big).rfind("HTTP/1.1 200 ", 0) == 0);
    // every slot taken by a connection that just opened: one more is refused
    bool closed[Dashboard::MAX_CLIENTS + 2] = {};
    for (int i = 0; i < Dashboard::MAX_CLIENTS + 1; i++) {
        MemConn* c = new MemConn();
        c->closedFlag = &closed[i];
        CHECK_EQ(d.adopt(c), i < Dashboard::MAX_CLIENTS);
    }
    CHECK(closed[Dashboard::MAX_CLIENTS]);
    CHECK_EQ(d.stats().refused, 1u);
    // once they have been silent for a while, a newcomer takes the oldest one's place
    plat::delayMs(Dashboard::IDLE_MS + 20);
    d.poll();
    std::string got;
    MemConn* late = new MemConn();
    late->in = "GET /api/status HTTP/1.1\r\n\r\n";
    late->out = &got;
    late->closedFlag = &closed[Dashboard::MAX_CLIENTS + 1];
    CHECK(d.adopt(late));
    CHECK(closed[0]);
    CHECK(!closed[1]);
    CHECK_EQ(d.stats().idleDropped, 1u);
    for (int i = 0; i < 50 && !closed[Dashboard::MAX_CLIENTS + 1]; i++) d.poll();
    CHECK(got.rfind("HTTP/1.1 200 ", 0) == 0);
}   // the others are freed with the dashboard
#endif  // MC_DASHBOARD
