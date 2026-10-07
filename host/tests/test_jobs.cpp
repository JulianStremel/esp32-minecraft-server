// Worker pool and the snapshot/split APIs the background chunk jobs rely on.
#include <atomic>
#include <deque>
#include <vector>
#include "testing.h"
#include "mc/jobs.h"
#include "mc/net/connection.h"
#include "mc/registry.h"
#include "mc/server/chunk_codec.h"
#include "mc/storage/block_device.h"
#include "mc/storage/world_store.h"
#include "mc/world/light.h"
#include "mc/world/world.h"

using namespace mc;

namespace {

std::atomic<int> g_running{0};
std::atomic<int> g_maxParallel{0};

struct SumJob : Job {
    int n;
    uint64_t sum = 0;
    int worker = -2;
    std::vector<int>* finished;
    SumJob(int n_, std::vector<int>* f) : n(n_), finished(f) {}
    void run(WorkerScratch& ws) override {
        int now = ++g_running;
        int m = g_maxParallel.load();
        while (now > m && !g_maxParallel.compare_exchange_weak(m, now)) {}
        for (int i = 1; i <= n; i++) sum += (uint64_t)i * i;
        worker = ws.index;
        --g_running;
    }
    void finish() override {
        uint64_t want = (uint64_t)n * (n + 1) * (2 * n + 1) / 6;
        CHECK(sum == want);
        finished->push_back(n);
    }
};

std::vector<uint8_t> chunkBytes(const Chunk& c) {
    ByteBuf b;
    Writer w(b);
    writeChunkPacket(w, c);
    return std::vector<uint8_t>(b.data(), b.data() + b.size());
}

struct CaptureConn : Conn {
    std::vector<uint8_t> out;
    int read(uint8_t*, size_t) override { return 0; }
    int write(const uint8_t* b, size_t n) override {
        out.insert(out.end(), b, b + n);
        return (int)n;
    }
    bool connected() override { return true; }
    void close() override {}
};

}  // namespace

TEST(jobqueue_threads_run_every_job_once) {
    std::vector<int> done;
    JobQueue q;
    CHECK(q.start(2));
    CHECK_EQ(q.workers(), 2);
    g_maxParallel = 0;
    for (int i = 1; i <= 200; i++) q.submit(new SumJob(20000 + i, &done));
    CHECK_EQ(q.inFlight(), 200);
    q.drain();
    CHECK_EQ(q.inFlight(), 0);
    CHECK_EQ((int)done.size(), 200);
    std::vector<bool> seen(201, false);
    for (int n : done) {
        int i = n - 20000;
        CHECK(i >= 1 && i <= 200 && !seen[i]);
        if (i >= 1 && i <= 200) seen[i] = true;
    }
    CHECK(g_maxParallel.load() >= 1);
    char line[128];
    q.statusLine(line, sizeof(line));
    CHECK(strstr(line, "2 workers") != nullptr);
    q.stop();
    CHECK_EQ(q.workers(), 0);
}

TEST(jobqueue_inline_mode_runs_in_poll) {
    std::vector<int> done;
    JobQueue q;
    CHECK(q.start(0));
    CHECK(!q.threaded());
    for (int i = 1; i <= 5; i++) q.submit(new SumJob(i, &done));
    CHECK_EQ((int)done.size(), 0);   // nothing runs before poll()
    CHECK_EQ(q.poll(2), 2);
    CHECK_EQ((int)done.size(), 2);
    q.drain();
    CHECK_EQ((int)done.size(), 5);
    CHECK_EQ(done[0], 1);            // FIFO
    CHECK_EQ(done[4], 5);
}

TEST(jobqueue_stop_finishes_queued_jobs) {
    std::vector<int> done;
    {
        JobQueue q;
        CHECK(q.start(2));
        for (int i = 0; i < 50; i++) q.submit(new SumJob(50000, &done));
        // the destructor stops the queue: everything runs and finishes first
    }
    CHECK_EQ((int)done.size(), 50);
}

// ------------------------------------------------------------------ scheduling
namespace {

uint32_t g_clock = 0;
uint32_t fakeClock() { return g_clock; }

// Records the order in which jobs run and how they finish.
struct TagJob : Job {
    int tag;
    std::vector<int>* ran;
    std::vector<int>* finished;
    std::vector<int>* cancelledTags;
    std::atomic<bool>* gate = nullptr;   // run() waits for it (threaded tests)
    TagJob(int t, std::vector<int>* r, std::vector<int>* f, std::vector<int>* c)
        : tag(t), ran(r), finished(f), cancelledTags(c) {}
    void run(WorkerScratch&) override {
        while (gate && !gate->load()) plat::delayMs(1);
        ran->push_back(tag);
    }
    void finish() override {
        finished->push_back(tag);
        if (cancelled()) cancelledTags->push_back(tag);
    }
};

}  // namespace

TEST(jobqueue_runs_most_urgent_class_first_fifo_within_a_class) {
    std::vector<int> ran, fin, can;
    JobQueue q;
    q.setClock(fakeClock);
    CHECK(q.start(0));
    g_clock = 1000;
    q.submit(new TagJob(1, &ran, &fin, &can), PRIO_BACKGROUND);
    q.submit(new TagJob(2, &ran, &fin, &can), PRIO_NORMAL);
    q.submit(new TagJob(3, &ran, &fin, &can), PRIO_HIGH);
    q.submit(new TagJob(4, &ran, &fin, &can), PRIO_URGENT);
    q.submit(new TagJob(5, &ran, &fin, &can), PRIO_NORMAL);
    CHECK_EQ(q.queued(PRIO_NORMAL), 2);
    q.drain();
    int want[] = {4, 3, 2, 5, 1};
    CHECK_EQ((int)ran.size(), 5);
    for (int i = 0; i < 5 && i < (int)ran.size(); i++) CHECK_EQ(ran[i], want[i]);
    CHECK_EQ(q.queued(PRIO_NORMAL), 0);
    CHECK(can.empty());
}

TEST(jobqueue_constant_urgent_stream_cannot_starve_background) {
    // one background job, then an urgent job every 10 ms forever: the background job runs
    // as soon as its 3000 ms deadline is earlier than the newest urgent job's
    std::vector<int> ran, fin, can;
    JobQueue q;
    q.setClock(fakeClock);
    CHECK(q.start(0));
    g_clock = 0;
    q.submit(new TagJob(-1, &ran, &fin, &can), PRIO_BACKGROUND);
    int ranAt = -1;
    for (int k = 1; k <= 400 && ranAt < 0; k++) {
        g_clock = (uint32_t)k * 10;
        q.submit(new TagJob(k, &ran, &fin, &can), PRIO_URGENT);
        q.poll(1);
        if (!ran.empty() && ran.back() == -1) ranAt = k;
    }
    CHECK_EQ(ranAt, 301);  // tie at 3000 ms goes to the urgent job, 3010 ms to the old one
    // every urgent job submitted before that ran first, in order
    for (int i = 0; i + 1 < (int)ran.size(); i++) CHECK_EQ(ran[i], i + 1);
    q.drain();

    // the same for normal (500 ms) under a stream of high-priority (100 ms) jobs
    ran.clear();
    g_clock = 100000;
    q.submit(new TagJob(-2, &ran, &fin, &can), PRIO_NORMAL);
    ranAt = -1;
    for (int k = 1; k <= 100 && ranAt < 0; k++) {
        g_clock = 100000 + (uint32_t)k * 10;
        q.submit(new TagJob(k, &ran, &fin, &can), PRIO_HIGH);
        q.poll(1);
        if (!ran.empty() && ran.back() == -2) ranAt = k;
    }
    CHECK_EQ(ranAt, 41);  // 100410 + 100 > 100000 + 500
    q.drain();

    // with a backlog: two urgent jobs arrive per job run. The background job still runs
    // right after the urgent jobs that were due before it (deadline <= its own: ties go
    // to the urgent class), and before every later one.
    ran.clear();
    g_clock = 200000;
    q.submit(new TagJob(-3, &ran, &fin, &can), PRIO_BACKGROUND);   // due 203000
    int before = -1;
    for (int k = 1; k <= 1000 && before < 0; k++) {
        g_clock = 200000 + (uint32_t)k * 10;
        q.submit(new TagJob(k, &ran, &fin, &can), PRIO_URGENT);
        q.submit(new TagJob(k, &ran, &fin, &can), PRIO_URGENT);
        q.poll(1);
        for (size_t i = 0; i < ran.size(); i++)
            if (ran[i] == -3) before = (int)i;
    }
    CHECK_EQ(before, 600);   // urgent jobs due by 203000 ms: 2 per step for steps 1..300
    q.drain();
}

TEST(jobqueue_deadlines_survive_clock_wraparound) {
    std::vector<int> ran, fin, can;
    JobQueue q;
    q.setClock(fakeClock);
    CHECK(q.start(0));
    g_clock = 0xFFFFFC00u;                                     // 1 s before the wrap
    q.submit(new TagJob(1, &ran, &fin, &can), PRIO_NORMAL);   // due at 0xFFFFFDF4, before the wrap
    g_clock = 0x10;                                            // wrapped: 1040 ms later
    q.submit(new TagJob(2, &ran, &fin, &can), PRIO_URGENT);   // due at 0x10, after the wrap
    q.submit(new TagJob(3, &ran, &fin, &can), PRIO_BACKGROUND);
    q.drain();
    // the overdue normal job first; a plain unsigned comparison would put 0x10 first
    int want[] = {1, 2, 3};
    CHECK_EQ((int)ran.size(), 3);
    for (int i = 0; i < 3 && i < (int)ran.size(); i++) CHECK_EQ(ran[i], want[i]);
}

TEST(jobqueue_promote_moves_a_queued_job_up) {
    std::vector<int> ran, fin, can;
    JobQueue q;
    q.setClock(fakeClock);
    CHECK(q.start(0));
    g_clock = 0;
    TagJob* a = new TagJob(1, &ran, &fin, &can);
    TagJob* b = new TagJob(2, &ran, &fin, &can);
    TagJob* c = new TagJob(3, &ran, &fin, &can);
    q.submit(a, PRIO_NORMAL);
    q.submit(b, PRIO_NORMAL);
    q.submit(c, PRIO_NORMAL);
    CHECK(!q.promote(b, PRIO_BACKGROUND));   // not more urgent
    CHECK(q.promote(c, PRIO_URGENT));
    CHECK_EQ(c->priority(), PRIO_URGENT);
    CHECK_EQ(q.queued(PRIO_NORMAL), 2);
    CHECK_EQ(q.queued(PRIO_URGENT), 1);
    // a job that is due sooner than the promotion would make it stays where it is
    g_clock = 1000;   // a and b are 500 ms overdue; HIGH would mean "due at 1100"
    CHECK(!q.promote(a, PRIO_HIGH));
    q.drain();
    int want[] = {3, 1, 2};
    CHECK_EQ((int)ran.size(), 3);
    for (int i = 0; i < 3 && i < (int)ran.size(); i++) CHECK_EQ(ran[i], want[i]);

    // the promoted job's deadline really moves: due at 50 it overtakes a HIGH job due at
    // 100, and it queues behind an urgent job that was already waiting
    ran.clear();
    g_clock = 10000;
    q.submit(new TagJob(10, &ran, &fin, &can), PRIO_URGENT);   // due 10000
    q.submit(new TagJob(11, &ran, &fin, &can), PRIO_HIGH);     // due 10100
    TagJob* n = new TagJob(12, &ran, &fin, &can);
    q.submit(n, PRIO_NORMAL);                                   // due 10500
    g_clock = 10050;
    CHECK(q.promote(n, PRIO_URGENT));                           // due 10050
    q.drain();
    int want2[] = {10, 12, 11};
    CHECK_EQ((int)ran.size(), 3);
    for (int i = 0; i < 3 && i < (int)ran.size(); i++) CHECK_EQ(ran[i], want2[i]);
}

TEST(jobqueue_cancel_skips_run_but_still_finishes) {
    std::vector<int> ran, fin, can;
    {
        JobQueue q;
        CHECK(q.start(0));
        TagJob* a = new TagJob(1, &ran, &fin, &can);
        TagJob* b = new TagJob(2, &ran, &fin, &can);
        TagJob* c = new TagJob(3, &ran, &fin, &can);
        q.submit(a);
        q.submit(b, PRIO_HIGH);
        q.submit(c);
        CHECK(q.cancel(b));
        CHECK(!q.cancel(b));   // already cancelled
        CHECK_EQ(q.queued(PRIO_HIGH), 0);
        CHECK_EQ(q.inFlight(), 3);   // still owed a finish()
        q.drain();
        CHECK_EQ(q.inFlight(), 0);
    }
    CHECK_EQ((int)ran.size(), 2);
    CHECK_EQ((int)fin.size(), 3);
    CHECK_EQ((int)can.size(), 1);
    if (can.size() == 1) CHECK_EQ(can[0], 2);
    for (int t : ran) CHECK(t != 2);

    // threaded: a running job cannot be cancelled, queued ones can
    ran.clear();
    fin.clear();
    can.clear();
    std::atomic<bool> gate{false};
    {
        JobQueue q;
        CHECK(q.start(1));
        TagJob* blocker = new TagJob(10, &ran, &fin, &can);
        blocker->gate = &gate;
        q.submit(blocker);
        while (q.queued(PRIO_NORMAL) > 0) plat::delayMs(1);   // the worker took it
        TagJob* js[5];
        for (int i = 0; i < 5; i++) {
            js[i] = new TagJob(20 + i, &ran, &fin, &can);
            q.submit(js[i], (JobPriority)(i % PRIO_COUNT));
        }
        CHECK(!q.cancel(blocker));   // running
        CHECK(q.cancel(js[1]));
        CHECK(q.cancel(js[3]));
        gate = true;
        q.drain();
    }
    CHECK_EQ((int)fin.size(), 6);
    CHECK_EQ((int)ran.size(), 4);
    CHECK_EQ((int)can.size(), 2);
    for (int t : ran) CHECK(t != 21 && t != 23);
}

TEST(jobqueue_status_reports_queues_and_waits) {
    std::vector<int> ran, fin, can;
    JobQueue q;
    q.setClock(fakeClock);
    CHECK(q.start(0));
    g_clock = 5000;
    q.submit(new TagJob(1, &ran, &fin, &can), PRIO_BACKGROUND);
    q.submit(new TagJob(2, &ran, &fin, &can), PRIO_HIGH);
    char line[200];
    q.statusLine(line, sizeof(line));
    CHECK(strstr(line, "queued 0/1/0/1") != nullptr);
    g_clock = 5250;
    q.drain();
    q.statusLine(line, sizeof(line));
    CHECK(strstr(line, "max wait 0/250/0/250 ms") != nullptr);
    q.statusLine(line, sizeof(line));   // waits are per report
    CHECK(strstr(line, "max wait 0/0/0/0 ms") != nullptr);
}

TEST(frame_packet_matches_connection_streamed_output) {
    Generator g;
    g.init(7, WORLD_NORMAL);
    Chunk c(3, -2);
    g.generate(c);
    uint8_t* ws = (uint8_t*)plat::bigAlloc(DeflateSink::WORKSPACE);
    for (int threshold : {-1, 64, 1 << 20}) {
        CaptureConn* cc = new CaptureConn();
        Connection conn;
        conn.attach(cc);
        conn.setCompression(threshold);
        conn.sendStreamed([&](Writer& w) { writeChunkPacket(w, c); });
        conn.flush();
        ByteBuf out, tmp;
        CHECK(framePacket([&](Writer& w) { writeChunkPacket(w, c); }, threshold, ws, out, tmp));
        CHECK_EQ(out.size(), cc->out.size());
        CHECK(out.size() == cc->out.size() && memcmp(out.data(), cc->out.data(), out.size()) == 0);
        conn.close();
    }
    plat::bigFree(ws);
}

TEST(chunk_clone_is_deep_and_identical) {
    Generator g;
    g.init(11, WORLD_NORMAL);
    Chunk c(0, 0);
    g.generate(c);
    c.set(1, 70, 1, bs::Glowstone);
    TileEntity* t = c.addTile(TILE_SIGN, 2, 71, 3);
    strcpy(t->text[0], "hello");
    c.addTile(TILE_CHEST, 4, 72, 5)->items[0] = ItemStack::of(itm::Diamond, 3);
    c.version = 42;
    c.storeSeq = 7;
    c.storeSlot = 1;
    Chunk* k = c.clone();
    CHECK(k != nullptr);
    if (!k) return;
    CHECK(chunkBytes(c) == chunkBytes(*k));
    CHECK_EQ(k->version, 42);
    CHECK_EQ(k->storeSeq, 7);
    CHECK_EQ(k->storeSlot, 1);
    CHECK_EQ(k->tileCount(), 2);
    CHECK_STR(k->tileAt(2, 71, 3)->text[0], "hello");
    CHECK_EQ(k->tileAt(4, 72, 5)->items[0].count, 3);
    for (int z = 0; z < 16; z++)
        for (int x = 0; x < 16; x++) CHECK_EQ(k->height(x, z), c.height(x, z));
    // deep: editing the copy leaves the original alone
    k->set(1, 70, 1, bs::Stone);
    strcpy(k->tileAt(2, 71, 3)->text[0], "bye");
    CHECK_EQ(c.get(1, 70, 1), bs::Glowstone);
    CHECK_STR(c.tileAt(2, 71, 3)->text[0], "hello");
    delete k;
}

TEST(light_from_edge_snapshot_equals_world_lookup) {
    Generator g;
    g.init(5, WORLD_NORMAL);
    World w;
    w.init(&g, nullptr, 64, 64);
    for (int cz = -1; cz <= 1; cz++)
        for (int cx = -1; cx <= 1; cx++) w.load(cx, cz);
    Chunk* c = w.get(0, 0);
    ChunkLight a, b;
    CHECK(a.compute(*c, &w));
    NeighbourEdges e;
    e.gather(w, 0, 0);
    for (int k = 0; k < 4; k++) CHECK(e.present[k]);
    Chunk* snap = c->clone();
    CHECK(b.compute(*snap, e));
    CHECK_EQ(a.sections(), b.sections());
    for (int s = 0; s < a.sections(); s++) {
        CHECK(memcmp(a.sky(s), b.sky(s), 2048) == 0);
        CHECK(memcmp(a.block(s), b.block(s), 2048) == 0);
    }
    delete snap;
}

TEST(store_split_io_matches_synchronous_paths) {
    MemDevice dev(16u << 20);
    WorldStore ws(&dev);
    StoreParams sp;
    sp.radius = 4;
    CHECK(ws.open(sp, true));
    CHECK(ws.splitIo());
    Generator g;
    g.init(3, WORLD_NORMAL);
    Chunk c(1, -2);
    g.generate(c);
    c.set(5, 80, 5, bs::GoldBlock);
    uint8_t* dws = (uint8_t*)plat::bigAlloc(DeflateSink::WORKSPACE);

    // encode (worker) + write (game loop), twice: the copies alternate slots
    for (int round = 0; round < 3; round++) {
        c.set(6, 80 + round, 6, bs::Glass);
        ChunkRecord rec;
        Chunk* snap = c.clone();
        CHECK(ws.encodeChunk(*snap, rec, dws));
        delete snap;
        int8_t prevSlot = c.storeSlot;
        CHECK(ws.writeChunk(c, rec));
        CHECK(c.storeSlot != prevSlot);
        CHECK_EQ(c.storeSeq, (uint32_t)round + 1);

        // the synchronous loader reads what the split writer wrote
        Chunk a(1, -2);
        CHECK_EQ(ws.loadChunk(a), LOAD_OK);
        a.recomputeHeightmap();  // as World::load does
        CHECK(chunkBytes(a) == chunkBytes(c));
        // fetch (game loop) + decode (worker) gives the same chunk and slot
        ChunkRecord fetched;
        CHECK_EQ(ws.fetchChunk(1, -2, fetched), LOAD_OK);
        Chunk b(1, -2);
        CHECK(ws.decodeChunk(fetched, b));
        b.recomputeHeightmap();
        CHECK(chunkBytes(b) == chunkBytes(c));
        CHECK_EQ(b.storeSeq, c.storeSeq);
        CHECK_EQ(b.storeSlot, c.storeSlot);
    }
    // a synchronous save continues the same sequence
    c.set(7, 90, 7, bs::Stone);
    CHECK(ws.saveChunk(c));
    CHECK_EQ(c.storeSeq, 4u);
    ChunkRecord fetched;
    CHECK_EQ(ws.fetchChunk(1, -2, fetched), LOAD_OK);
    CHECK_EQ(fetched.seq, 4u);
    // never stored / out of range
    ChunkRecord none;
    CHECK_EQ(ws.fetchChunk(0, 0, none), LOAD_ABSENT);
    CHECK_EQ(ws.fetchChunk(100, 0, none), LOAD_ABSENT);
    // batched fetch: stored, never stored and out-of-range chunks in one call
    {
        Chunk d(-3, 2);
        g.generate(d);
        CHECK(ws.saveChunk(d));
        int32_t bx[4] = {1, 0, -3, 100}, bz[4] = {-2, 0, 2, 0};
        ChunkRecord r[4];
        ChunkRecord* rp[4] = {&r[0], &r[1], &r[2], &r[3]};
        LoadResult res[4];
        ws.fetchChunks(4, bx, bz, rp, res);
        CHECK_EQ(res[0], LOAD_OK);
        CHECK_EQ(res[1], LOAD_ABSENT);
        CHECK_EQ(res[2], LOAD_OK);
        CHECK_EQ(res[3], LOAD_ABSENT);
        CHECK_EQ(r[0].seq, 4u);
        Chunk e(-3, 2);
        CHECK(ws.decodeChunk(r[2], e));
        e.recomputeHeightmap();
        CHECK(chunkBytes(e) == chunkBytes(d));
    }
    // a corrupted record fails to decode (the caller then falls back to loadChunk)
    fetched.bytes.data()[fetched.bytes.size() / 2] ^= 0x55;
    Chunk bad(1, -2);
    CHECK(!ws.decodeChunk(fetched, bad));
    plat::bigFree(dws);
}

// ------------------------------------------------------------------ ChunkJobs inside a server
#include <unistd.h>
#include "mc/server/server.h"

static void pollUntil(Server& s, bool (*done)(Server&), int maxMs = 5000) {
    for (int t = 0; t < maxMs && !done(s); t++) {
        s.chunkJobs.poll();
        usleep(1000);
    }
}

TEST(chunk_jobs_load_save_and_supersede) {
    MemDevice dev(32u << 20);
    WorldStore* ws = new WorldStore(&dev);
    StoreParams sp;
    sp.radius = 8;
    CHECK(ws->open(sp, true));
    ServerConfig cfg;
    cfg.port = 0;
    cfg.seed = 9;
    cfg.spawnMobs = false;
    cfg.workerThreads = 2;
    Server* s = new Server();
    CHECK(s->begin(cfg, ws));
    CHECK(s->chunkJobs.queue().threaded());

    // background loads: nullptr first, resident once the job finished
    CHECK(s->chunkJobs.acquire(5, 5) == nullptr);
    CHECK(s->chunkJobs.acquire(5, 5) == nullptr);  // no duplicate job
    CHECK_EQ(s->storageIo.inFlight(), 1); // fetch precedes CPU submission
    pollUntil(*s, [](Server& sv) { return sv.world.isResident(5, 5); });
    CHECK(s->world.isResident(5, 5));
    CHECK(s->chunkJobs.acquire(5, 5) != nullptr);

    // a synchronous load while a background load is pending wins (its edit survives)
    CHECK(s->chunkJobs.acquire(-6, 3) == nullptr);
    s->world.setBlock(-6 * 16 + 1, 100, 3 * 16 + 1, bs::GoldBlock, false);   // loads (-6, 3) synchronously
    s->chunkJobs.drain();
    CHECK_EQ(s->world.getBlock(-6 * 16 + 1, 100, 3 * 16 + 1), bs::GoldBlock);

    // background saves: encode on a CPU worker, write on the I/O thread
    s->world.setBlock(5 * 16 + 2, 120, 5 * 16 + 2, bs::Glowstone, false);
    CHECK(s->world.dirtyCount() >= 2);
    while (s->chunkJobs.saveDirty(4) > 0 || s->chunkJobs.savesInFlight() > 0) s->chunkJobs.poll();
    CHECK_EQ(s->world.dirtyCount(), 0);
    CHECK(s->chunkJobs.stats().saved >= 2);
    CHECK(s->storage->flush());
    delete s;
    delete ws;

    // reopen: both edits were stored, and the stored chunks load through decode jobs
    WorldStore* ws2 = new WorldStore(&dev);
    CHECK(ws2->open(sp, false));
    Server* s2 = new Server();
    cfg.port = 0;
    CHECK(s2->begin(cfg, ws2));
    CHECK(s2->chunkJobs.acquire(5, 5) == nullptr);
    CHECK(s2->chunkJobs.acquire(-6, 3) == nullptr);
    pollUntil(*s2, [](Server& sv) { return sv.world.isResident(5, 5) && sv.world.isResident(-6, 3); });
    CHECK_EQ(s2->world.getBlock(5 * 16 + 2, 120, 5 * 16 + 2), bs::Glowstone);
    CHECK_EQ(s2->world.getBlock(-6 * 16 + 1, 100, 3 * 16 + 1), bs::GoldBlock);
    CHECK(s2->chunkJobs.stats().decoded >= 2);
    delete s2;
    delete ws2;
}

TEST(chunk_jobs_batch_keeps_closest_and_reserves_urgent_slots) {
    // the batch keeps the closest candidates when it is full
    LoadBatch b;
    for (int i = 0; i < LoadBatch::MAX; i++) b.add(100 + i, 0, 6);
    b.add(0, 0, 1);
    b.add(1, 0, 6);   // not closer than anything left: dropped
    CHECK_EQ(b.n, LoadBatch::MAX);
    bool haveNear = false;
    for (int i = 0; i < b.n; i++) haveNear |= b.cx[i] == 0 && b.dist[i] == 1;
    CHECK(haveNear);
    b.add(0, 0, 0);   // same chunk, closer: distance updated
    for (int i = 0; i < b.n; i++)
        if (b.cx[i] == 0) CHECK_EQ(b.dist[i], 0);

    ServerConfig cfg;
    cfg.port = 0;
    cfg.seed = 3;
    cfg.spawnMobs = false;
    cfg.workerThreads = 0;   // inline: nothing runs before poll(), so the queue is observable
    Server* s = new Server();
    CHECK(s->begin(cfg, nullptr));
    ChunkJobs& cj = s->chunkJobs;
    // inline mode has 2 load slots; urgent loads may use URGENT_RESERVE more
    LoadBatch req;
    cj.beginBatch(req);
    for (int i = 0; i < 4; i++) cj.want(req, 20 + i, 20, 1);   // urgent
    for (int i = 0; i < 4; i++) cj.want(req, 30 + i, 20, 5);   // normal
    cj.requestLoads(req);
    CHECK_EQ(cj.queue().queued(PRIO_URGENT), 4);
    // the regular slots are taken, but the non-urgent share is always available
    CHECK_EQ(cj.queue().queued(PRIO_NORMAL), ChunkJobs::NON_URGENT_SHARE);
    CHECK(cj.loadsFull());
    // urgent loads stop at the regular slots + the reserve
    LoadBatch more;
    cj.beginBatch(more);
    for (int i = 0; i < 8; i++) cj.want(more, 50 + i, 20, 0);
    cj.requestLoads(more);
    CHECK_EQ(cj.queue().queued(PRIO_URGENT), 2 + ChunkJobs::URGENT_RESERVE);   // inline: 2 regular slots

    // nobody can see those chunks: the queued loads are cancelled, nothing is generated
    cj.cancelStale();
    CHECK_EQ(cj.stats().cancelled, (uint32_t)(2 + ChunkJobs::URGENT_RESERVE + ChunkJobs::NON_URGENT_SHARE));
    CHECK_EQ(cj.queue().queued(PRIO_URGENT), 0);
    cj.drain();
    CHECK(!cj.loadsFull());
    for (int i = 0; i < 4; i++) CHECK(!s->world.isResident(20 + i, 20));
    CHECK_EQ(cj.queue().queued(PRIO_URGENT) + cj.queue().queued(PRIO_NORMAL), 0);
    CHECK_EQ(cj.stats().generated, 0u);

    // loads that do not come from players' views (acquire) are never cancelled as stale
    CHECK(cj.acquire(40, 40) == nullptr);
    cj.cancelStale();
    cj.drain();
    CHECK(s->world.isResident(40, 40));
    delete s;
}

TEST(jobqueue_threaded_stress_with_cancel_and_promote) {
    // workers race submit / promote / cancel from the game loop; nothing is lost, nothing
    // cancelled runs. (Pointers stay valid until poll() finishes the job, so the loop only
    // touches jobs it submitted since its last poll.)
    struct StressJob : Job {
        std::atomic<int>* ranCount;
        bool ran = false;
        int* bad;
        int* finishedCount;
        int* cancelledCount;
        void run(WorkerScratch&) override {
            volatile uint32_t x = 1;
            for (int i = 0; i < 2000; i++) x = x * 1664525u + 1013904223u;
            ran = true;
            (*ranCount)++;
        }
        void finish() override {
            (*finishedCount)++;
            if (cancelled()) {
                (*cancelledCount)++;
                if (ran) (*bad)++;
            } else if (!ran) {
                (*bad)++;
            }
        }
    };
    std::atomic<int> ranCount{0};
    int bad = 0, finished = 0, cancelledN = 0, submitted = 0;
    JobQueue q;
    CHECK(q.start(2));
    uint32_t rng = 12345;
    auto next = [&]() { rng = rng * 1103515245u + 12345u; return rng >> 8; };
    for (int round = 0; round < 200; round++) {
        StressJob* batch[16];
        for (int i = 0; i < 16; i++) {
            StressJob* j = new StressJob();
            j->ranCount = &ranCount;
            j->bad = &bad;
            j->finishedCount = &finished;
            j->cancelledCount = &cancelledN;
            batch[i] = j;
            q.submit(j, (JobPriority)(next() % PRIO_COUNT));
            submitted++;
        }
        for (int i = 0; i < 16; i++) {
            uint32_t r = next() % 4;
            if (r == 0) q.cancel(batch[i]);
            else if (r == 1) q.promote(batch[i], PRIO_URGENT);
        }
        q.poll();
    }
    q.drain();
    CHECK_EQ(finished, submitted);
    CHECK_EQ(bad, 0);
    CHECK_EQ(ranCount.load() + cancelledN, submitted);
    CHECK(cancelledN > 0);
    for (int p = 0; p < PRIO_COUNT; p++) CHECK_EQ(q.queued((JobPriority)p), 0);
}

// a player in play on a fake connection, view centred on chunk (cx, cz)
static Player& testPlayer(Server& s, int slot, int cx, int cz) {
    Player& p = s.players[slot];
    p.reset(&s, slot);
    p.conn.attach(new CaptureConn());
    p.state = CS_PLAY;
    p.viewDist = 4;
    p.e.x = cx * 16 + 8;
    p.e.z = cz * 16 + 8;
    p.e.y = 80;
    p.updateView(true);
    return p;
}

static Server* inlineServer() {
    ServerConfig cfg;
    cfg.port = 0;
    cfg.seed = 3;
    cfg.spawnMobs = false;
    cfg.workerThreads = 0;   // inline: jobs run only in poll(), so the queue is observable
    Server* s = new Server();
    if (!s->begin(cfg, nullptr)) return nullptr;
    return s;
}

TEST(chunk_jobs_cancelled_send_leaves_a_newer_send_alone) {
    // regression: after a respawn-like view reset, the cancelled old send of a chunk must
    // not reset the view cell that a new send of the same chunk has claimed
    Server* s = inlineServer();
    CHECK(s != nullptr);
    if (!s) return;
    ChunkJobs& cj = s->chunkJobs;
    Player& p = testPlayer(*s, 0, 0, 0);
    Chunk* c = s->world.load(1, 0);
    CHECK(cj.sendChunk(p, *c));
    *p.viewCell(1, 0) = VIEW_PENDING;
    p.resetView();
    p.updateView(true);        // every cell VIEW_NONE again, same session
    cj.cancelStale();          // the old send is still queued: cancelled
    CHECK_EQ(cj.stats().cancelled, 1u);
    CHECK(cj.sendChunk(p, *c));
    *p.viewCell(1, 0) = VIEW_PENDING;
    cj.drain();                // finishes the cancelled one, then runs the new one
    CHECK_EQ(*p.viewCell(1, 0), VIEW_SENT);
    CHECK_EQ(p.pendingSends, 0);
    CHECK_EQ(c->jobRefs, 0);
    CHECK_EQ(cj.stats().sent, 1u);
    delete s;
}

TEST(chunk_jobs_stale_loads_cancelled_with_one_chunk_of_slack) {
    Server* s = inlineServer();
    CHECK(s != nullptr);
    if (!s) return;
    ChunkJobs& cj = s->chunkJobs;
    testPlayer(*s, 0, 0, 0);   // view distance 4 around chunk (0, 0)
    LoadBatch b;
    cj.beginBatch(b);
    cj.want(b, 3, 0, 0, 0);    // in view          (dist 0: admitted as urgent)
    cj.want(b, 5, 0, 0, 0);    // one beyond: kept (slack)
    cj.want(b, 6, 0, 0, 0);    // two beyond: cancelled
    cj.requestLoads(b);
    cj.cancelStale();
    CHECK_EQ(cj.stats().cancelled, 1u);
    cj.drain();
    CHECK(s->world.isResident(3, 0));
    CHECK(s->world.isResident(5, 0));
    CHECK(!s->world.isResident(6, 0));
    delete s;
}

TEST(chunk_jobs_session_change_cancels_queued_sends) {
    Server* s = inlineServer();
    CHECK(s != nullptr);
    if (!s) return;
    ChunkJobs& cj = s->chunkJobs;
    Player& p = testPlayer(*s, 0, 0, 0);
    Chunk* c = s->world.load(2, 1);
    CHECK(cj.sendChunk(p, *c));
    CHECK_EQ(c->jobRefs, 1);
    p.reset(s, 0);             // the player left: new session in this slot
    cj.cancelStale();
    CHECK_EQ(cj.stats().cancelled, 1u);
    cj.drain();
    CHECK_EQ(c->jobRefs, 0);   // the pin is released on the cancelled path too
    CHECK_EQ(cj.stats().sent, 0u);
    delete s;
}

TEST(chunk_jobs_send_promoted_when_player_comes_closer) {
    Server* s = inlineServer();
    CHECK(s != nullptr);
    if (!s) return;
    ChunkJobs& cj = s->chunkJobs;
    Player& p = testPlayer(*s, 0, 0, 0);
    Chunk* c = s->world.load(4, 0);
    CHECK(cj.sendChunk(p, *c));          // 4 chunks away: normal
    *p.viewCell(4, 0) = VIEW_PENDING;
    CHECK_EQ(cj.queue().queued(PRIO_NORMAL), 1);
    p.e.x = 3 * 16 + 8;                  // walks next to it
    p.updateView(false);
    CHECK_EQ(*p.viewCell(4, 0), VIEW_PENDING);
    cj.cancelStale();
    CHECK_EQ(cj.stats().promoted, 1u);
    CHECK_EQ(cj.queue().queued(PRIO_URGENT), 1);
    // and a pending load is promoted the same way through want()
    LoadBatch b;
    cj.beginBatch(b);
    cj.want(b, 7, 3, 5, 0);              // normal
    cj.requestLoads(b);
    LoadBatch b2;
    cj.beginBatch(b2);
    cj.want(b2, 7, 3, 1, 0);             // now needed urgently
    CHECK_EQ(b2.n, 0);                   // not requested twice
    CHECK_EQ(cj.stats().promoted, 2u);
    cj.drain();
    CHECK_EQ(*p.viewCell(4, 0), VIEW_SENT);
    delete s;
}

TEST(chunk_jobs_constant_urgent_demand_cannot_starve_other_loads) {
    // every tick brings more urgent loads than the slots hold; a far chunk wanted by
    // another player still gets admitted through the non-urgent share
    Server* s = inlineServer();
    CHECK(s != nullptr);
    if (!s) return;
    ChunkJobs& cj = s->chunkJobs;
    int fresh = 0;
    for (int tick = 0; tick < 30 && !s->world.isResident(20, 20); tick++) {
        LoadBatch b;
        cj.beginBatch(b);
        for (int i = 0; i < 8; i++, fresh++) cj.want(b, -30 + fresh % 60, -30 - fresh / 60, 0, 0);
        cj.want(b, 20, 20, 6, 1);
        cj.requestLoads(b);
        cj.poll();
    }
    cj.drain();
    CHECK(s->world.isResident(20, 20));
    CHECK_EQ(cj.pinnedChunks(), 0);
    delete s;
}

