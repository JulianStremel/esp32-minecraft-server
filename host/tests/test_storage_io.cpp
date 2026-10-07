#include <atomic>
#include <thread>
#include <vector>
#include "testing.h"
#include "mc/server/server.h"
#include "mc/storage/world_store.h"
#include "mc/registry.h"

using namespace mc;
namespace {
struct FaultDevice : MemDevice {
    using MemDevice::MemDevice;
    std::atomic<bool> failFlush{false};
    bool flush() override { return !failFlush.load(); }
};
struct GateStore : WorldStore {
    using WorldStore::WorldStore;
    std::atomic<bool> holdWrite{false}, enteredWrite{false}, failRead{false};
    std::atomic<bool> rejectDecode{false};
    std::atomic<int> recoveries{0};
    bool writeChunk(Chunk& c, const ChunkRecord& r) override {
        enteredWrite = true;
        while (holdWrite.load()) plat::delayMs(1);
        return WorldStore::writeChunk(c, r);
    }
    void fetchChunks(int n, const int32_t* x, const int32_t* z, ChunkRecord* const* records, LoadResult* results) override {
        if (failRead) { for (int i = 0; i < n; ++i) results[i] = LOAD_ERROR; }
        else WorldStore::fetchChunks(n, x, z, records, results);
    }
    bool decodeChunk(const ChunkRecord& r, Chunk& c) const override {
        return !rejectDecode.load() && WorldStore::decodeChunk(r, c);
    }
    LoadResult loadChunk(Chunk& c) override { ++recoveries; return WorldStore::loadChunk(c); }
};
void initialize(WorldStore& store) {
    StoreParams sp; sp.radius = 2; sp.playerSlots = 8;
    CHECK(store.open(sp, true));
}
void begin(Server& s, Storage& store) {
    ServerConfig cfg; cfg.port = 0; cfg.seed = 42; cfg.worldType = WORLD_FLAT;
    cfg.workerThreads = 2; cfg.spawnMobs = false;
    CHECK(s.begin(cfg, &store));
}
}

TEST(storage_io_bounded_fifo_owner_and_shutdown) {
    MemDevice dev(4 * 1024 * 1024); WorldStore store(&dev); initialize(store);
    StorageIo io; CHECK(io.start(&store));
    std::thread::id caller = std::this_thread::get_id(), owner;
    std::atomic<bool> wrongThread{false};
    std::vector<int> executed, completed;
    for (int i = 0; i < StorageIo::CAPACITY; ++i) {
        CHECK(io.post([&, i](Storage&) {
            if (i == 0) owner = std::this_thread::get_id();
            if (std::this_thread::get_id() == caller || std::this_thread::get_id() != owner) wrongThread = true;
            executed.push_back(i);
        }, [&, i]() {
            CHECK(std::this_thread::get_id() == caller);
            completed.push_back(i);
        }));
    }
    CHECK(!io.post([](Storage&) {})); // completed-but-unpolled work still consumes capacity
    CHECK_EQ(io.inFlight(), StorageIo::CAPACITY);
    CHECK(io.flush()); // reserved synchronous slot must work even when the queue is full
    CHECK_EQ(completed.size(), 0); // synchronous calls must not reenter game-loop callbacks
    io.stop(); // finishes accepted work before destroying thread state
    CHECK(!wrongThread);
    CHECK_EQ(executed.size(), StorageIo::CAPACITY);
    CHECK_EQ(completed.size(), StorageIo::CAPACITY);
    for (int i = 0; i < StorageIo::CAPACITY; ++i) { CHECK_EQ(executed[i], i); CHECK_EQ(completed[i], i); }
}

TEST(storage_io_save_snapshot_edit_and_failed_ack) {
    FaultDevice dev(4 * 1024 * 1024); GateStore store(&dev); initialize(store);
    Server s; begin(s, store);
    Chunk* live = s.world.load(0, 0);
    live->set(1, 30, 1, bs::GoldBlock); live->dirty = true;
    store.holdWrite = true;
    CHECK(s.chunkJobs.saveChunk(*live));
    uint32_t deadline = plat::millis() + 5000;
    while (!store.enteredWrite && plat::millis() < deadline) { s.chunkJobs.poll(); plat::delayMs(1); }
    CHECK(store.enteredWrite);
    CHECK(live->saving && live->jobRefs > 0);
    CHECK_EQ(live->storeSeq, 0); // worker has not modified the live object
    live->set(1, 30, 1, bs::Glowstone); live->dirty = true;
    store.holdWrite = false;
    s.chunkJobs.drain();
    CHECK(live->dirty); CHECK(!live->saving); CHECK_EQ(live->jobRefs, 0);
    Chunk first(0, 0); CHECK_EQ(s.storage->loadChunk(first), LOAD_OK);
    CHECK_EQ(first.get(1,30,1), bs::GoldBlock); // in-flight snapshot, not the later edit
    CHECK(s.chunkJobs.saveChunk(*live)); s.chunkJobs.drain();
    CHECK(!live->dirty); CHECK_EQ(live->storeSeq, 2);
    Chunk second(0, 0); CHECK_EQ(s.storage->loadChunk(second), LOAD_OK);
    CHECK_EQ(second.get(1,30,1), bs::Glowstone);

    dev.failFlush = true;
    live->set(2,30,2,bs::GoldBlock); live->dirty = true;
    CHECK(s.chunkJobs.saveChunk(*live)); s.chunkJobs.drain();
    CHECK(live->dirty); CHECK(!live->saving); CHECK_EQ(live->jobRefs, 0);
    CHECK_EQ(s.world.stats().saveErrors, 1);
    CHECK_EQ(live->storeSeq, 2); // no acknowledgement, no successful-save metadata
    dev.failFlush = false;
    CHECK(s.chunkJobs.saveChunk(*live)); s.chunkJobs.drain();
    CHECK(!live->dirty);
    Chunk retried(0,0); CHECK_EQ(s.storage->loadChunk(retried),LOAD_OK);
    CHECK_EQ(retried.get(2,30,2),bs::GoldBlock);
}

TEST(storage_io_load_failure_and_decode_recovery) {
    FaultDevice dev(4 * 1024 * 1024); GateStore store(&dev); initialize(store);
    Chunk saved(1,1); saved.set(3,20,3,bs::Glowstone);
    CHECK(store.saveChunk(saved)); CHECK(store.flush());
    Server s; begin(s,store);
    int initial = store.recoveries;
    store.rejectDecode = true; // CPU decode fails; full load retries the stored copies on the I/O thread
    CHECK(s.chunkJobs.acquire(1,1) == nullptr); s.chunkJobs.drain();
    CHECK(store.recoveries > initial);
    CHECK_EQ(s.world.getBlock(19,20,19),bs::Glowstone);
    store.rejectDecode = false;
    store.failRead = true;
    CHECK(s.chunkJobs.acquire(-1,-1) == nullptr); s.chunkJobs.drain();
    auto* failed = s.world.peek(-1,-1);
    CHECK(failed != nullptr);
    CHECK(failed && failed->readOnly);
    CHECK_EQ(s.world.stats().loadErrors,1);
}
