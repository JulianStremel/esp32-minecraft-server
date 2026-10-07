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
    CHECK_EQ(s->chunkJobs.queue().inFlight(), 1);
    pollUntil(*s, [](Server& sv) { return sv.world.isResident(5, 5); });
    CHECK(s->world.isResident(5, 5));
    CHECK(s->chunkJobs.acquire(5, 5) != nullptr);

    // a synchronous load while a background load is pending wins (its edit survives)
    CHECK(s->chunkJobs.acquire(-6, 3) == nullptr);
    s->world.setBlock(-6 * 16 + 1, 100, 3 * 16 + 1, bs::GoldBlock, false);   // loads (-6, 3) synchronously
    s->chunkJobs.drain();
    CHECK_EQ(s->world.getBlock(-6 * 16 + 1, 100, 3 * 16 + 1), bs::GoldBlock);

    // background saves: encode on a worker, write on the game loop
    s->world.setBlock(5 * 16 + 2, 120, 5 * 16 + 2, bs::Glowstone, false);
    CHECK(s->world.dirtyCount() >= 2);
    while (s->chunkJobs.saveDirty(4) > 0 || s->chunkJobs.savesInFlight() > 0) s->chunkJobs.poll();
    CHECK_EQ(s->world.dirtyCount(), 0);
    CHECK(s->chunkJobs.stats().saved >= 2);
    CHECK(ws->flush());
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
