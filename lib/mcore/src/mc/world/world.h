// Resident chunk cache with LRU eviction. Chunks come from the ChunkStore
// (network storage) when they were saved before, otherwise from the generator.
// Unmodified chunks are dropped on eviction (they can be regenerated);
// modified ("dirty") chunks are written back first.
#pragma once
#include <stdint.h>
#include "mc/world/chunk.h"
#include "mc/world/generator.h"

namespace mc {

class ChunkStore {
public:
    virtual ~ChunkStore() {}
    // true: chunk was stored and has been filled in. false: not stored (or unreadable).
    virtual bool loadChunk(Chunk& c) = 0;
    virtual bool saveChunk(const Chunk& c) = 0;
    virtual bool chunkInRange(int cx, int cz) const = 0;
};

class WorldListener {
public:
    virtual ~WorldListener() {}
    virtual void onBlockChanged(int x, int y, int z, uint16_t oldState, uint16_t newState) = 0;
    virtual void onChunkEvicted(Chunk& c) {}
};

class ChunkPinner {
public:
    virtual ~ChunkPinner() {}
    virtual bool isChunkPinned(int cx, int cz) = 0;   // e.g. inside some player's view
};

struct WorldStats {
    uint32_t loads = 0, generated = 0, saves = 0, saveErrors = 0, evictions = 0;
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
    Chunk* load(int cx, int cz);          // resident, stored or freshly generated
    bool isResident(int cx, int cz) { return find(cx, cz) >= 0; }

    // Block access by world coordinates. getBlock only looks at resident chunks
    // (returns `missing` otherwise); setBlock loads the chunk if needed.
    uint16_t getBlock(int x, int y, int z, uint16_t missing = 0);
    uint16_t setBlock(int x, int y, int z, uint16_t state, bool notify = true);
    int heightAt(int x, int z);           // heightmap value (top motion-blocking y + 1)
    void markDirty(int cx, int cz);

    // Evicts least recently used, unpinned chunks until within capacity.
    void maintain();
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
