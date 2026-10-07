// Resident chunk cache with LRU eviction. Chunks come from the ChunkStore
// (network storage) when they were saved before, otherwise from the generator.
// Unmodified chunks are dropped on eviction (they can be regenerated);
// modified ("dirty") chunks are written back first.
#pragma once
#include <stdint.h>
#include "mc/bytebuf.h"
#include "mc/world/chunk.h"
#include "mc/world/generator.h"

namespace mc {

enum LoadResult : uint8_t { LOAD_ABSENT = 0, LOAD_OK = 1, LOAD_ERROR = 2 };

// A chunk as stored (usually zlib compressed), handed between the game loop, which
// does the storage I/O, and worker threads, which encode and decode it.
struct ChunkRecord {
    ByteBuf bytes;
    uint32_t raw = 0;      // uncompressed payload size
    uint32_t crc = 0;      // CRC32 of bytes
    uint32_t seq = 0;      // sequence number of this copy
    int8_t slot = -1;      // storage slot it was read from
    uint16_t flags = 0;
};

class ChunkStore {
public:
    virtual ~ChunkStore() {}
    // LOAD_OK: chunk was stored and has been filled in. LOAD_ABSENT: never stored.
    // LOAD_ERROR: storage unreachable or record corrupt (the stored data must be protected).
    virtual LoadResult loadChunk(Chunk& c) = 0;
    virtual bool saveChunk(Chunk& c) = 0;
    virtual bool chunkInRange(int cx, int cz) const = 0;

    // Optional split API for background work: I/O on the game loop thread, the CPU
    // heavy (de)compression on any thread. Stores that do not implement it return false
    // from splitIo() and are only used through loadChunk/saveChunk.
    virtual bool splitIo() const { return false; }
    // Reads the newest valid copy of (cx, cz). Results as loadChunk.
    virtual LoadResult fetchChunk(int cx, int cz, ChunkRecord& rec) { return LOAD_ERROR; }
    // Same for n chunks at once; network stores pipeline this into one or two round trips.
    virtual void fetchChunks(int n, const int32_t* cx, const int32_t* cz, ChunkRecord* const* recs, LoadResult* res) {
        for (int i = 0; i < n; i++) res[i] = fetchChunk(cx[i], cz[i], *recs[i]);
    }
    // Thread-safe. Verifies and decodes rec into c (false: use loadChunk, which also
    // tries the older copy).
    virtual bool decodeChunk(const ChunkRecord& rec, Chunk& c) const { return false; }
    // Thread-safe. Encodes c (a snapshot) into rec; deflateWs is the caller's deflate workspace.
    virtual bool encodeChunk(const Chunk& c, ChunkRecord& rec, uint8_t* deflateWs) const { return false; }
    // Writes rec, encoded from a snapshot of c, as c's newest copy.
    virtual bool writeChunk(Chunk& c, const ChunkRecord& rec) { return false; }
};

class WorldListener {
public:
    virtual ~WorldListener() {}
    virtual void onBlockChanged(int x, int y, int z, uint16_t oldState, uint16_t newState) = 0;
    virtual void onChunkEvicted(Chunk& c) {}
    // load() created (cx, cz) synchronously (storage or generator)
    virtual void onChunkLoaded(int cx, int cz) {}
    // c became resident (loaded, generated or adopted from a background job)
    virtual void onChunkReady(Chunk& c) {}
    // c is about to be stored synchronously (attach what travels with it)
    virtual void onChunkSaving(Chunk& c) {}
    // true: eviction must keep this dirty chunk until an asynchronous save completes.
    virtual bool deferEvictionSave(Chunk& c) { return false; }
};

class ChunkPinner {
public:
    virtual ~ChunkPinner() {}
    virtual bool isChunkPinned(int cx, int cz) = 0;   // e.g. inside some player's view
};

struct WorldStats {
    uint32_t loads = 0, generated = 0, saves = 0, saveErrors = 0, evictions = 0, loadErrors = 0;
};

class World {
public:
    World();
    ~World();
    // capacity: soft limit of resident chunks (pinned chunks may exceed it).
    // radius: world border radius in chunks (edits outside are refused).
    void init(Generator* gen, ChunkStore* store, int capacity, int radiusChunks);
    void setListener(WorldListener* l) { listener_ = l; }
    void setPinner(ChunkPinner* p) { pinner_ = p; }

    Generator& generator() { return *gen_; }
    int radius() const { return radius_; }
    bool chunkInBounds(int cx, int cz) const { return cx >= -radius_ && cx < radius_ && cz >= -radius_ && cz < radius_; }
    bool blockInBounds(int x, int z) const { return chunkInBounds(x >> 4, z >> 4); }

    Chunk* get(int cx, int cz);           // resident chunk or nullptr
    Chunk* peek(int cx, int cz) const;    // like get() without touching the LRU order
    Chunk* load(int cx, int cz);          // resident, stored or freshly generated
    // Background loading (server/chunk_jobs): inserts a chunk that was generated or
    // decoded elsewhere. If (cx, cz) became resident in the meantime, c is deleted
    // and the resident chunk returned.
    Chunk* adopt(Chunk* c, bool generated);
    ChunkStore* store() { return store_; }
    bool isResident(int cx, int cz) { return find(cx, cz) >= 0; }
    // false if the chunk could not be loaded from storage (edits are refused then)
    bool isWritable(int x, int z);

    // Block access by world coordinates. getBlock only looks at resident chunks
    // (returns `missing` otherwise); setBlock loads the chunk if needed.
    uint16_t getBlock(int x, int y, int z, uint16_t missing = 0);
    uint16_t setBlock(int x, int y, int z, uint16_t state, bool notify = true);
    int heightAt(int x, int z);           // heightmap value (top motion-blocking y + 1)
    void markDirty(int cx, int cz);
    // Saving through a background job: the chunk counts as clean until it changes again.
    void noteSaved() { stats_.saves++; }
    void noteSaveError() { stats_.saveErrors++; }
    void noteLoadError() { stats_.loadErrors++; }

    // Evicts least recently used, unpinned chunks until within capacity.
    void maintain();
    // Evicts up to n unpinned chunks regardless of capacity (memory pressure).
    int evictUnpinned(int n);
    // Writes up to maxChunks dirty chunks; returns number written.
    int saveDirty(int maxChunks);
    int saveAll();
    int dirtyCount();

    int residentCount() const { return count_; }
    size_t residentBytes();
    const WorldStats& stats() const { return stats_; }

    // iteration over resident chunks (for broadcasting / saving)
    int tableSize() const { return tableSize_; }
    Chunk* slot(int i) { return table_[i]; }

private:
    int find(int cx, int cz) const;
    void insert(Chunk* c);
    void removeAt(int idx);
    void grow();
    bool evictOne();
    bool saveChunk(Chunk* c);

    Generator* gen_ = nullptr;
    ChunkStore* store_ = nullptr;
    WorldListener* listener_ = nullptr;
    ChunkPinner* pinner_ = nullptr;
    Chunk** table_ = nullptr;
    int tableSize_ = 0;
    int count_ = 0;
    int capacity_ = 64;
    int radius_ = 64;
    uint32_t clock_ = 0;
    WorldStats stats_;
};

}  // namespace mc
