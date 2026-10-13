// /workerbar: what the workers do, as boss bars for the player who asked (operators).
//
//   a legend:   send load light save path spawn dash other idle   (each in its colour)
//   W0 87%      ██████████████████████████████████░░░░░░          (the last second)
//   W1 45%      ...
//   queue 23    ███████████████░░░░                               (waiting, by kind)
//
// A worker's line splits its last second between the job kinds it ran (the running
// job's part so far included) and idle time; the bar's fill is its busy share, green,
// yellow from 70%, red from 90%. The queue line has one segment per waiting job (scaled
// down beyond 40). Updated once a second; the workers are shared, so these are the
// server's queue, not one per worker (workers take the job with the earliest deadline).
#include <stdio.h>
#include <string.h>
#include "mc/registry.h"
#include "mc/server/server.h"
#include "mc/text.h"

namespace mc {

namespace {
constexpr int SEGMENTS = 40;
// the kinds' colours (JobQueue's order), then idle
const char* const KIND_COLOR[JobQueue::JOB_KINDS + 1] = {"green", "aqua", "yellow", "light_purple", "gold", "red",
                                                         "blue", "white", "dark_gray"};
const char* const KIND_LABEL[JobQueue::JOB_KINDS + 1] = {"send", "load", "light", "save", "path", "spawn",
                                                         "dash", "other", "idle"};
enum { BAR_PINK = 0, BAR_BLUE = 1, BAR_RED = 2, BAR_GREEN = 3, BAR_YELLOW = 4, BAR_WHITE = 6 };
enum { BAR_ADD = 0, BAR_REMOVE = 1, BAR_HEALTH = 2, BAR_TITLE = 3, BAR_STYLE = 4 };

void barUuid(int line, uint8_t out[16]) {   // "mc-workerbar" + the line
    static const uint8_t BASE[16] = {0x6d, 0x63, 0x2d, 0x77, 0x6f, 0x72, 0x6b, 0x65, 0x72, 0x62, 0x40, 0x00, 0x80, 0x00, 0x00, 0x00};
    memcpy(out, BASE, 16);
    out[15] = (uint8_t)line;
}

struct Json {
    char* p;
    size_t cap, n = 0;
    bool first = true;
    Json(char* out, size_t c) : p(out), cap(c) { add("["); }
    void add(const char* s) {
        size_t l = strlen(s);
        if (n + l + 1 < cap) {
            memcpy(p + n, s, l);
            n += l;
            p[n] = 0;
        }
    }
    void text(const char* t, const char* color) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s{\"text\":\"", first ? "" : ",");
        add(buf);
        add(t);
        snprintf(buf, sizeof(buf), "\",\"color\":\"%s\"}", color);
        add(buf);
        first = false;
    }
    void blocks(int count, const char* color) {   // a run of █ (3 bytes each in UTF-8)
        if (count <= 0) return;
        char run[SEGMENTS * 3 + 1];
        int k = 0;
        for (int i = 0; i < count && i < SEGMENTS; i++) {
            run[k++] = (char)0xE2; run[k++] = (char)0x96; run[k++] = (char)0x88;
        }
        run[k] = 0;
        text(run, color);
    }
    void end() { add("]"); }
};

// splits SEGMENTS between the values (largest remainders), in order
void segments(const uint32_t* v, int n, uint64_t total, int out[]) {
    int given = 0;
    double rem[JobQueue::JOB_KINDS + 1];
    for (int i = 0; i < n; i++) {
        double exact = total ? (double)v[i] * SEGMENTS / (double)total : 0;
        out[i] = (int)exact;
        rem[i] = exact - out[i];
        given += out[i];
    }
    while (given < SEGMENTS && total) {
        int best = -1;
        for (int i = 0; i < n; i++)
            if (v[i] && (best < 0 || rem[i] > rem[best])) best = i;
        if (best < 0) break;
        out[best]++;
        rem[best] = -1;
        given++;
    }
}
}  // namespace

int Server::workerBarLines() {
    return 2 + chunkJobs.queue().workers();   // the legend, a line per worker, the queue
}

void Server::workerBarState(WorkerBarLine* out, int n) {
    JobQueue& q = chunkJobs.queue();
    // the legend
    {
        Json j(out[0].json, sizeof(out[0].json));
        j.text("workers: ", "gray");
        for (int k = 0; k <= JobQueue::JOB_KINDS; k++) {
            char label[16];
            snprintf(label, sizeof(label), "%s%s", KIND_LABEL[k], k < JobQueue::JOB_KINDS ? " " : "");
            j.text(label, KIND_COLOR[k]);
        }
        j.end();
        out[0].health = 0;
        out[0].color = BAR_WHITE;
    }
    // a line per worker
    for (int i = 0; i < q.workers() && 1 + i < n - 1; i++) {
        WorkerBarLine& l = out[1 + i];
        uint32_t us[JobQueue::JOB_KINDS + 1] = {};
        uint32_t window = 0;
        q.takeWorkerKinds(i, us, window);
        uint64_t busy = 0;
        for (int k = 0; k < JobQueue::JOB_KINDS; k++) busy += us[k];
        if (window < busy) window = (uint32_t)busy;   // (a job finishing as it was read)
        us[JobQueue::JOB_KINDS] = window ? (uint32_t)(window - busy) : 1;   // idle
        int seg[JobQueue::JOB_KINDS + 1];
        segments(us, JobQueue::JOB_KINDS + 1, window ? window : 1, seg);
        float share = window ? (float)busy / (float)window : 0;
        Json j(l.json, sizeof(l.json));
        char head[24];
        snprintf(head, sizeof(head), "W%d %3d%% ", i, (int)(share * 100 + 0.5f));
        j.text(head, "white");
        for (int k = 0; k <= JobQueue::JOB_KINDS; k++) j.blocks(seg[k], KIND_COLOR[k]);
        j.end();
        l.health = share > 1 ? 1 : share;
        l.color = share >= 0.9f ? BAR_RED : share >= 0.7f ? BAR_YELLOW : BAR_GREEN;
    }
    // the queue
    {
        WorkerBarLine& l = out[n - 1];
        int count[JobQueue::JOB_KINDS];
        q.queuedByKind(count);
        uint32_t v[JobQueue::JOB_KINDS];
        int total = 0;
        for (int k = 0; k < JobQueue::JOB_KINDS; k++) {
            v[k] = (uint32_t)count[k];
            total += count[k];
        }
        int seg[JobQueue::JOB_KINDS];
        if (total <= SEGMENTS) for (int k = 0; k < JobQueue::JOB_KINDS; k++) seg[k] = count[k];   // one per job
        else segments(v, JobQueue::JOB_KINDS, (uint64_t)total, seg);
        Json j(l.json, sizeof(l.json));
        char head[40];
        snprintf(head, sizeof(head), q.workers() ? "queue %d " : "queue %d (no workers: on the game loop) ", total);
        j.text(head, "white");
        for (int k = 0; k < JobQueue::JOB_KINDS; k++) j.blocks(seg[k], KIND_COLOR[k]);
        j.end();
        l.health = total >= 64 ? 1 : total / 64.0f;
        l.color = total >= 48 ? BAR_RED : total >= 16 ? BAR_YELLOW : BAR_BLUE;
    }
}

static void sendBar(Player& p, int line, int action, const Server::WorkerBarLine* l) {
    uint8_t uuid[16];
    barUuid(line, uuid);
    Packet pk(pkt::s2c::BossBar);
    pk.w.uuid(uuid);
    pk.w.varint(action);
    if (action == BAR_ADD) {
        writeTextNbt(pk.w, l->json);
        pk.w.f32(l->health);
        pk.w.varint(l->color);
        pk.w.varint(0);   // no notches
        pk.w.u8(0);       // no flags
    } else if (action == BAR_TITLE) {
        writeTextNbt(pk.w, l->json);
    } else if (action == BAR_HEALTH) {
        pk.w.f32(l->health);
    } else if (action == BAR_STYLE) {
        pk.w.varint(l->color);
        pk.w.varint(0);
    }
    p.conn.send(pk);
}

void Server::setWorkerBar(Player& p, bool on) {
    if (on == (p.workerBarShown > 0)) return;
    if (!on) {
        for (int i = 0; i < p.workerBarShown; i++) sendBar(p, i, BAR_REMOVE, nullptr);
        p.workerBarShown = 0;
        return;
    }
    int n = workerBarLines();
    WorkerBarLine* lines = new WorkerBarLine[n];
    workerBarState(lines, n);
    for (int i = 0; i < n; i++) sendBar(p, i, BAR_ADD, &lines[i]);
    delete[] lines;
    p.workerBarShown = (uint8_t)n;
}

// once a second, for the players who have it on
void Server::tickWorkerBar() {
    if (ticks % 20 != 0) return;
    bool any = false;
    for (int i = 0; i < MC_MAX_PLAYERS && !any; i++) any = players[i].inPlay() && players[i].workerBarShown;
    if (!any) return;
    int n = workerBarLines();
    WorkerBarLine* lines = new WorkerBarLine[n];   // (on the heap: ~1 KB a line)
    workerBarState(lines, n);
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& p = players[i];
        if (!p.inPlay() || !p.workerBarShown) continue;
        if (p.workerBarShown != n) {   // the number of workers changed (/workers): rebuilt
            for (int b = 0; b < p.workerBarShown; b++) sendBar(p, b, BAR_REMOVE, nullptr);
            for (int b = 0; b < n; b++) sendBar(p, b, BAR_ADD, &lines[b]);
            p.workerBarShown = (uint8_t)n;
            continue;
        }
        for (int b = 0; b < n; b++) {
            sendBar(p, b, BAR_TITLE, &lines[b]);
            sendBar(p, b, BAR_HEALTH, &lines[b]);
            sendBar(p, b, BAR_STYLE, &lines[b]);
        }
    }
    delete[] lines;
}

}  // namespace mc
