#include "mc/world/world.h"
#include <stdlib.h>
#include <string.h>
#include "mc/platform.h"

namespace mc {

static inline uint32_t chunkHash(int cx, int cz) {
    uint32_t h = (uint32_t)cx * 0x9E3779B1u ^ ((uint32_t)cz + 0x7F4A7C15u) * 0x85EBCA77u;
    return h ^ (h >> 15);
}

World::World() {}

World::~World() {
    for (int i = 0; i < tableSize_; i++) delete table_[i];
    free(table_);
}

void World::init(Generator* gen, ChunkStore* store, int capacity, int radiusChunks) {
    gen_ = gen;
    store_ = store;
    capacity_ = capacity < 9 ? 9 : capacity;
    radius_ = radiusChunks;
    tableSize_ = 16;
    while (tableSize_ < capacity_ * 2) tableSize_ <<= 1;
    table_ = (Chunk**)calloc(tableSize_, sizeof(Chunk*));
}

int World::find(int cx, int cz) const {
    uint32_t mask = tableSize_ - 1;
    for (uint32_t i = chunkHash(cx, cz) & mask;; i = (i + 1) & mask) {
        Chunk* c = table_[i];
        if (!c) return -1;
        if (c->cx == cx && c->cz == cz) return (int)i;
    }
}

void World::insert(Chunk* c) {
    if ((count_ + 1) * 4 > tableSize_ * 3) grow();
    uint32_t mask = tableSize_ - 1;
    uint32_t i = chunkHash(c->cx, c->cz) & mask;
    while (table_[i]) i = (i + 1) & mask;
    table_[i] = c;
    count_++;
}

void World::grow() {
    int oldSize = tableSize_;
    Chunk** old = table_;
    tableSize_ <<= 1;
    table_ = (Chunk**)calloc(tableSize_, sizeof(Chunk*));
    count_ = 0;
    for (int i = 0; i < oldSize; i++)
        if (old[i]) insert(old[i]);
    free(old);
}

// Linear-probing deletion with backward shift (no tombstones).
void World::removeAt(int idx) {
    uint32_t mask = tableSize_ - 1;
    uint32_t i = (uint32_t)idx;
    table_[i] = nullptr;
    count_--;
    uint32_t j = i;
    for (;;) {
        j = (j + 1) & mask;
        Chunk* c = table_[j];
        if (!c) break;
        uint32_t k = chunkHash(c->cx, c->cz) & mask;
        // move c back if its home slot k is not cyclically in (i, j]
        bool inRange = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
        if (!inRange) {
            table_[i] = c;
            table_[j] = nullptr;
            i = j;
        }
    }
}

Chunk* World::get(int cx, int cz) {
    int i = find(cx, cz);
    if (i < 0) return nullptr;
    table_[i]->lastUse = ++clock_;
    return table_[i];
}

Chunk* World::peek(int cx, int cz) const {
    int i = find(cx, cz);
    return i < 0 ? nullptr : table_[i];
}

Chunk* World::adopt(Chunk* c, bool generated) {
    Chunk* have = get(c->cx, c->cz);
    if (have) {
        delete c;
        return have;
    }
    if (count_ >= capacity_) evictOne();
    if (generated) stats_.generated++;
    else stats_.loads++;
    c->lastUse = ++clock_;
    insert(c);
    return c;
}

Chunk* World::load(int cx, int cz) {
    Chunk* c = get(cx, cz);
    if (c) return c;
    if (count_ >= capacity_) evictOne();
    c = new Chunk(cx, cz);
    LoadResult res = LOAD_ABSENT;
    if (store_ && chunkInBounds(cx, cz) && store_->chunkInRange(cx, cz)) res = store_->loadChunk(*c);
    if (res == LOAD_OK) {
        stats_.loads++;
        c->recomputeHeightmap();
        c->dirty = false;
        c->lightDirty = true;
    } else {
        if (res == LOAD_ERROR) {
            // show generated terrain but protect the stored copy; retried after eviction
            delete c;
            c = new Chunk(cx, cz);
            stats_.loadErrors++;
        }
        gen_->generate(*c);
        c->readOnly = res == LOAD_ERROR;
        stats_.generated++;
    }
    c->lastUse = ++clock_;
    insert(c);
    if (listener_) listener_->onChunkLoaded(cx, cz);
    return c;
}

uint16_t World::getBlock(int x, int y, int z, uint16_t missing) {
    if (y < 0 || y >= WORLD_HEIGHT) return 0;
    int i = find(x >> 4, z >> 4);
    if (i < 0) return missing;
    return table_[i]->get(x & 15, y, z & 15);
}

bool World::isWritable(int x, int z) {
    Chunk* c = load(x >> 4, z >> 4);
    return !c->readOnly;
}

uint16_t World::setBlock(int x, int y, int z, uint16_t state, bool notify) {
    if (y < 0 || y >= WORLD_HEIGHT) return 0;
    Chunk* c = load(x >> 4, z >> 4);
    if (c->readOnly) return c->get(x & 15, y, z & 15);
    uint16_t old = c->set(x & 15, y, z & 15, state);
    if (old != state) {
        c->dirty = true;
        c->lightDirty = true;
        if (notify && listener_) listener_->onBlockChanged(x, y, z, old, state);
    }
    return old;
}

int World::heightAt(int x, int z) {
    Chunk* c = get(x >> 4, z >> 4);
    return c ? c->height(x & 15, z & 15) : 0;
}

void World::markDirty(int cx, int cz) {
    Chunk* c = get(cx, cz);
    if (c) c->dirty = true;
}

bool World::saveChunk(Chunk* c) {
    if (c->readOnly) { c->dirty = false; return true; }
    if (!store_ || !chunkInBounds(c->cx, c->cz) || !store_->chunkInRange(c->cx, c->cz)) {
        c->dirty = false;  // cannot be persisted; treat as clean
        return true;
    }
    if (store_->saveChunk(*c)) {
        c->dirty = false;
        stats_.saves++;
        return true;
    }
    stats_.saveErrors++;
    return false;
}

bool World::evictOne() {
    int best = -1;
    uint32_t bestUse = 0xFFFFFFFFu;
    for (int i = 0; i < tableSize_; i++) {
        Chunk* c = table_[i];
        if (!c || c->jobRefs) continue;   // a background job will report back on it
        if (pinner_ && pinner_->isChunkPinned(c->cx, c->cz) && !c->readOnly) continue;
        if (c->lastUse < bestUse) { bestUse = c->lastUse; best = i; }
    }
    if (best < 0) return false;
    Chunk* c = table_[best];
    if (c->dirty && !saveChunk(c)) {
        // storage unavailable: keep it resident rather than lose edits
        c->lastUse = ++clock_;
        return false;
    }
    if (listener_) listener_->onChunkEvicted(*c);
    removeAt(best);
    delete c;
    stats_.evictions++;
    return true;
}

void World::maintain() {
    int guard = 8;
    while (count_ > capacity_ && guard-- > 0)
        if (!evictOne()) break;
}

int World::evictUnpinned(int n) {
    int done = 0;
    while (done < n && evictOne()) done++;
    return done;
}

int World::saveDirty(int maxChunks) {
    int n = 0;
    for (int i = 0; i < tableSize_ && n < maxChunks; i++) {
        Chunk* c = table_[i];
        if (c && c->dirty) {
            if (!saveChunk(c)) break;
            n++;
        }
    }
    return n;
}

int World::saveAll() { return saveDirty(1 << 30); }

int World::dirtyCount() {
    int n = 0;
    for (int i = 0; i < tableSize_; i++)
        if (table_[i] && table_[i]->dirty) n++;
    return n;
}

size_t World::residentBytes() {
    size_t n = 0;
    for (int i = 0; i < tableSize_; i++)
        if (table_[i]) n += table_[i]->memoryBytes();
    return n;
}

}  // namespace mc
