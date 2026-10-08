#include "mc/storage/storage_io.h"
#include <stdio.h>
#include <utility>

namespace mc {
namespace {
class FunctionTask : public StorageTask {
public:
    std::function<void(Storage&)> work;
    std::function<void()> completion;
    FunctionTask(std::function<void(Storage&)> r, std::function<void()> f = {})
        : work(std::move(r)), completion(std::move(f)) {}
    void run(Storage& s) override { work(s); }
    void finish() override { if (completion) completion(); }
};
}
bool StorageIo::start(Storage* s) {
    if (backend_) return false;
    mutex_ = plat::mutexCreate(); todo_ = plat::semCreate(); exited_ = plat::semCreate();
    reply_ = plat::semCreate();
    backend_ = s;
    s->statusLine(status_, sizeof(status_));
    if (mutex_ && todo_ && exited_ && reply_ && plat::startThread("mcstorage", 0, 1, 16384, worker, this)) return true;
    backend_ = nullptr;
    if (todo_) plat::semDestroy(todo_);
    if (exited_) plat::semDestroy(exited_);
    if (reply_) plat::semDestroy(reply_);
    if (mutex_) plat::mutexDestroy(mutex_);
    mutex_ = todo_ = exited_ = reply_ = nullptr;
    return false;
}
void StorageIo::enqueue(StorageTask* t) {
    { LockGuard lock(mutex_);
      t->next_ = nullptr;
      if (tail_) tail_->next_ = t; else head_ = t;
      tail_ = t;
    }
    plat::semGive(todo_);
}
bool StorageIo::submit(StorageTask* t) {
    if (!available()) return false;
    ++inFlight_;
    enqueue(t);
    return true;
}
bool StorageIo::post(std::function<void(Storage&)> run, std::function<void()> finish) {
    if (!available()) return false;
    auto* t = new FunctionTask(std::move(run), std::move(finish));
    if (submit(t)) return true;
    delete t;
    return false;
}
void StorageIo::worker(void* arg) {
    auto& q = *static_cast<StorageIo*>(arg);
    for (;;) {
        if (!plat::semTake(q.todo_, 1000)) continue;
        StorageTask* t;
        { LockGuard lock(q.mutex_);
          t = q.head_;
          if (!t) { if (q.stopping_) break; else continue; }
          q.head_ = t->next_;
          if (!q.head_) q.tail_ = nullptr;
        }
        t->run(*q.backend_);
        char status[sizeof(q.status_)];
        q.backend_->statusLine(status, sizeof(status));
        void* waiter = t->waiter_;
        { LockGuard lock(q.mutex_);
          snprintf(q.status_, sizeof(q.status_), "%s", status);
          if (!waiter) {
              t->next_ = nullptr;
              if (q.doneTail_) q.doneTail_->next_ = t; else q.done_ = t;
              q.doneTail_ = t;
          }
        }
        // No access to t after publication: the caller may immediately destroy it.
        if (waiter) plat::semGive(waiter);
        else plat::wake();
        plat::yield();
    }
    plat::semGive(q.exited_);
}
void StorageIo::poll() {
    if (!backend_) return;
    StorageTask* list;
    { LockGuard lock(mutex_); list = done_; done_ = doneTail_ = nullptr; }
    while (list) {
        auto* t = list; list = t->next_;
        --inFlight_;
        t->finish();
        delete t;
    }
}
void StorageIo::drain() {
    while (inFlight_) { poll(); if (inFlight_) plat::delayMs(1); }
}
void StorageIo::stop() {
    if (!backend_) return;
    drain();
    { LockGuard lock(mutex_); stopping_ = true; }
    plat::semGive(todo_);
    // A timeout must never free state a live worker may still access.
    while (!plat::semTake(exited_, 1000)) {}
    plat::semDestroy(todo_); plat::semDestroy(exited_); plat::semDestroy(reply_); plat::mutexDestroy(mutex_);
    backend_ = nullptr; mutex_ = todo_ = exited_ = reply_ = nullptr; stopping_ = false;
}
void StorageIo::call(std::function<void(Storage&)> fn) {
    FunctionTask t(std::move(fn));
    t.waiter_ = reply_;
    // One extra reserved slot for synchronous barriers, independent of async capacity.
    enqueue(&t);
    while (!plat::semTake(t.waiter_, 1000)) {}
}
bool StorageIo::loadMeta(WorldMeta& m) { bool ok; call([&](Storage& s){ok=s.loadMeta(m);}); return ok; }
bool StorageIo::saveMeta(const WorldMeta& m) { bool ok; call([&](Storage& s){ok=s.saveMeta(m);}); return ok; }
bool StorageIo::loadPlayer(const uint8_t* u, PlayerData& p) { bool ok; call([&](Storage& s){ok=s.loadPlayer(u,p);}); return ok; }
LoadResult StorageIo::fetchPlayer(const uint8_t* u, PlayerData& p) { LoadResult v; call([&](Storage& s){v=s.fetchPlayer(u,p);}); return v; }
bool StorageIo::resetWorld(const WorldMeta& m) { bool ok; call([&](Storage& s){ok=s.resetWorld(m);}); return ok; }
bool StorageIo::savePlayer(const PlayerData& p) { bool ok; call([&](Storage& s){ok=s.savePlayer(p) && s.flush();}); return ok; }
bool StorageIo::flush() { bool ok; call([&](Storage& s){ok=s.flush();}); return ok; }
bool StorageIo::flushLater() {
    return post([](Storage& s){ if (!s.flush()) MC_LOGW("storage: background flush failed"); });
}
LoadResult StorageIo::loadChunk(Chunk& c) { LoadResult r; call([&](Storage& s){r=s.loadChunk(c);}); return r; }
bool StorageIo::saveChunk(Chunk& c) { bool ok; call([&](Storage& s){ok=s.saveChunk(c) && s.flush();}); return ok; }
LoadResult StorageIo::fetchChunk(uint8_t d,int x,int z,ChunkRecord& r) { LoadResult v; call([&](Storage& s){v=s.fetchChunk(d,x,z,r);}); return v; }
void StorageIo::fetchChunks(int n,const uint8_t* d,const int32_t* x,const int32_t* z,ChunkRecord* const* r,LoadResult* v) { call([&](Storage& s){s.fetchChunks(n,d,x,z,r,v);}); }
bool StorageIo::writeChunk(Chunk& c,const ChunkRecord& r) { bool ok; call([&](Storage& s){ok=s.writeChunk(c,r) && s.flush();}); return ok; }
void StorageIo::statusLine(char* buf,size_t cap) {
    LockGuard lock(mutex_);
    snprintf(buf,cap,"I/O %d/%d | %s",inFlight_,CAPACITY,status_);
}
} // namespace mc
