// Single owner of the storage connection. Only immutable encode/decode/layout
// operations may bypass the thread. Completions always run on the game loop.
#pragma once
#include <functional>
#include "mc/storage/storage.h"

namespace mc {
class StorageTask {
public:
    virtual ~StorageTask() {}
    virtual void run(Storage& storage) = 0;
    virtual void finish() {}
private:
    friend class StorageIo;
    StorageTask* next_ = nullptr;
    void* waiter_ = nullptr;
};

class StorageIo : public Storage {
public:
    static constexpr int CAPACITY = 32; // includes results waiting for poll()
    ~StorageIo() override { stop(); }
    bool start(Storage* storage);
    void stop();
    bool available() const { return backend_ && inFlight_ < CAPACITY; }
    // Ownership transfers only on success; false means backpressure, not data loss.
    bool submit(StorageTask* task);
    bool post(std::function<void(Storage&)> run, std::function<void()> finish = {});
    void poll();
    void drain();
    int inFlight() const { return inFlight_; }

    // Compatibility calls (startup, explicit synchronous world operations, shutdown).
    // These wait for the I/O thread, never access its connection from the caller.
    bool loadMeta(WorldMeta& m) override;
    bool saveMeta(const WorldMeta& m) override;
    bool loadPlayer(const uint8_t uuid[16], PlayerData& p) override;
    LoadResult fetchPlayer(const uint8_t uuid[16], PlayerData& p) override;
    bool savePlayer(const PlayerData& p) override;
    bool flush() override;
    bool flushLater() override;
    void statusLine(char* buf, size_t cap) override;
    int worldRadius() const override { return backend_->worldRadius(); }
    bool chunkInRange(int cx, int cz) const override { return backend_->chunkInRange(cx, cz); }
    bool splitIo() const override { return backend_->splitIo(); }
    LoadResult loadChunk(Chunk& c) override;
    bool saveChunk(Chunk& c) override;
    LoadResult fetchChunk(uint8_t dim, int cx, int cz, ChunkRecord& r) override;
    void fetchChunks(int n, const uint8_t* dims, const int32_t* x, const int32_t* z, ChunkRecord* const* records,
                     LoadResult* results) override;
    bool decodeChunk(const ChunkRecord& r, Chunk& c) const override { return backend_->decodeChunk(r, c); }
    bool encodeChunk(const Chunk& c, ChunkRecord& r, uint8_t* ws) const override { return backend_->encodeChunk(c, r, ws); }
    bool writeChunk(Chunk& c, const ChunkRecord& r) override;
private:
    void call(std::function<void(Storage&)> fn);
    void enqueue(StorageTask* task);
    static void worker(void* arg);
    Storage* backend_ = nullptr; // outlives stop()
    void *mutex_ = nullptr, *todo_ = nullptr, *exited_ = nullptr, *reply_ = nullptr;
    StorageTask *head_ = nullptr, *tail_ = nullptr, *done_ = nullptr, *doneTail_ = nullptr;
    int inFlight_ = 0; // game loop only
    bool stopping_ = false; // mutex protected
    char status_[256] = {};
};
} // namespace mc
