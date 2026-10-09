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

static int countOf(const std::string& s, const char* what) {
    int n = 0;
    for (size_t p = s.find(what); p != std::string::npos; p = s.find(what, p + 1)) n++;
    return n;
}

TEST(dashboard_pushes_events_formatted_on_a_worker) {
    Server* s = dashServer();
    if (!s) return;
    Dashboard d(*s);
    d.setPushInterval(50);
    std::string got;
    bool closed = false;
    MemConn* c = new MemConn();
    c->in = "GET /api/events HTTP/1.1\r\nAccept: text/event-stream\r\n\r\n";
    c->out = &got;
    c->closedFlag = &closed;
    CHECK(d.adopt(c));
    d.poll();
    CHECK(got.rfind("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n", 0) == 0);
    CHECK_EQ(d.streams(), 1);
    // events while the loop has time to spare: a snapshot, the JSON from a worker
    for (int i = 0; i < 400 && countOf(got, "data: {") < 3; i++) {
        d.afterLoop(40);
        s->chunkJobs.poll();
        d.poll();
        plat::delayMs(2);
    }
    CHECK(!closed);
    CHECK_EQ(countOf(got, "data: {"), 3);
    CHECK_EQ(countOf(got, "}\n\n"), 3);
    CHECK(got.find("\"streams\":1") != std::string::npos);
    printf("    3 events: snapshot %u us on the loop (max %u), JSON %u us on a worker (max %u)\n",
           (unsigned)(d.stats().snapTotalUs / d.stats().events), (unsigned)d.stats().snapMaxUs,
           (unsigned)(d.stats().formatTotalUs / d.stats().events), (unsigned)d.stats().formatMaxUs);
    // a busy loop (no time before the next tick) postpones the next one, but not forever
    uint32_t ev = d.stats().events;
    plat::delayMs(60);
    d.afterLoop(0);
    s->chunkJobs.queue().drain();
    CHECK_EQ(d.stats().events, ev);
    plat::delayMs(Dashboard::LATE_MS);
    d.afterLoop(0);
    s->chunkJobs.queue().drain();
    CHECK_EQ(d.stats().events, ev + 1);
    // the page goes away: no more snapshots
    c->open = false;
    d.poll();
    CHECK(closed);
    CHECK_EQ(d.streams(), 0);
    plat::delayMs(60);
    d.afterLoop(40);
    s->chunkJobs.queue().drain();
    CHECK_EQ(d.stats().events, ev + 1);
}

TEST(dashboard_limits_event_streams) {
    Server* s = dashServer();
    if (!s) return;
    Dashboard d(*s);
    std::string out[Dashboard::MAX_STREAMS + 1];
    for (int i = 0; i <= Dashboard::MAX_STREAMS; i++) {
        MemConn* c = new MemConn();
        c->in = "GET /api/events HTTP/1.1\r\n\r\n";
        c->out = &out[i];
        CHECK(d.adopt(c));
        d.poll();
    }
    CHECK_EQ(d.streams(), Dashboard::MAX_STREAMS);
    CHECK(out[Dashboard::MAX_STREAMS].rfind("HTTP/1.1 503 ", 0) == 0);   // the page polls instead
}
namespace {
std::string postReq(const char* path, const std::string& json, const char* token) {
    std::string r = std::string("POST ") + path + " HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n";
    if (token) r += std::string("Authorization: Bearer ") + token + "\r\n";
    return r + "Content-Length: " + std::to_string(json.size()) + "\r\n\r\n" + json;
}
// polls a stream until `until` shows up in what it got (or a second passes)
void pump(Server* s, Dashboard& d, const std::string& got, const char* until) {
    for (int i = 0; i < 500 && got.find(until) == std::string::npos; i++) {
        s->chunkJobs.poll();
        d.poll();
        plat::delayMs(2);
    }
}
}  // namespace

TEST(dashboard_console_streams_the_log_to_a_signed_in_page_and_runs_commands) {
    Server* s = dashServer();
    if (!s) return;
    Dashboard d(*s);
    d.setToken("console-tok");
    // a backlog bigger than one event, with characters JSON must escape
    char line[160];
    for (int i = 0; i < 120; i++) {
        snprintf(line, sizeof(line), "backlog %03d \"quoted\" \\ %.*s", i, 90, std::string(90, 'b').c_str());
        MC_LOGI("%s", line);
    }
    // the stream without a token: the state only; with a wrong one: refused
    std::string plain, wrong, got;
    MemConn* a = new MemConn();
    a->in = "GET /api/events HTTP/1.1\r\n\r\n";
    a->out = &plain;
    CHECK(d.adopt(a));
    MemConn* b = new MemConn();
    b->in = "GET /api/events HTTP/1.1\r\nAuthorization: Bearer nope\r\n\r\n";
    b->out = &wrong;
    CHECK(d.adopt(b));
    MemConn* c = new MemConn();
    c->in = "GET /api/events HTTP/1.1\r\nAuthorization: Bearer console-tok\r\n\r\n";
    c->out = &got;
    CHECK(d.adopt(c));
    pump(s, d, got, "backlog 119");
    CHECK(wrong.rfind("HTTP/1.1 401 ", 0) == 0);
    CHECK(plain.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
    CHECK(plain.find("event: log") == std::string::npos);
    CHECK(got.find("event: log\ndata: {\"lost\":0,\"lines\":[[") != std::string::npos);
    CHECK(countOf(got, "event: log") >= 3);   // the backlog in several events
    CHECK(got.find("backlog 000 \\\"quoted\\\" \\\\ bbb") != std::string::npos);
    CHECK(got.find("backlog 119") != std::string::npos);
    // every line once, in order
    size_t at = 0;
    bool ordered = true;
    for (int i = 0; i < 120; i++) {
        snprintf(line, sizeof(line), "backlog %03d ", i);
        size_t p = got.find(line, at);
        if (p == std::string::npos) ordered = false;
        else at = p;
        CHECK(countOf(got, line) == 1);
    }
    CHECK(ordered);
    // a command from the page: refused without the token, run with it, its output streamed
    std::string r = fetch(d, postReq("/api/console", "{\"command\":\"/time query daytime\"}", nullptr));
    CHECK(r.rfind("HTTP/1.1 401 ", 0) == 0);
    r = fetch(d, postReq("/api/console", "{\"command\":\"   \"}", "console-tok"));
    CHECK(r.rfind("HTTP/1.1 400 ", 0) == 0);
    r = fetch(d, postReq("/api/console", "{\"command\":\"/say hi \\\"there\\\" \u00e9\"}", "console-tok"));
    CHECK(r.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
    CHECK_EQ(d.stats().commands, 1u);
    pump(s, d, got, "issued server command: say");
    CHECK(got.find("dashboard issued server command: say hi \\\"there\\\" \xc3\xa9") != std::string::npos);
    CHECK(plain.find("issued server command") == std::string::npos);
    printf("    %u log events, %u lines, %u us on the loop each (max %u)\n", (unsigned)d.stats().logEvents,
           (unsigned)d.stats().logLines, (unsigned)(d.stats().logTotalUs / d.stats().logEvents),
           (unsigned)d.stats().logMaxUs);
}
#endif  // MC_DASHBOARD
