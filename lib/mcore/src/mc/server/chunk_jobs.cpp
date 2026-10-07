#include "mc/server/chunk_jobs.h"
#include <stdio.h>
#include "mc/server/chunk_codec.h"
#include "mc/server/server.h"

namespace mc {

// ------------------------------------------------------------------ jobs
// Generates a chunk, or decodes it from its stored record.
class LoadJob : public Job {
public:
    ChunkJobs* owner = nullptr;
    int cx = 0, cz = 0;
    const Generator* gen = nullptr;
    const ChunkStore* store = nullptr;   // set: decode rec instead of generating
    ChunkRecord rec;
    bool readOnly = false;               // storage failed: generate, never overwrite the stored copy
    Chunk* result = nullptr;
    bool failed = false;

    ~LoadJob() override { delete result; }
    void run(WorkerScratch&) override {
        result = new Chunk(cx, cz);
        if (!result) {
            failed = true;
            return;
        }
        if (store) {
            if (!store->decodeChunk(rec, *result)) {
                delete result;
                result = nullptr;
                failed = true;
                return;
            }
            result->recomputeHeightmap();
            result->dirty = false;
            result->lightDirty = true;
        } else {
            gen->generate(*result);
            result->readOnly = readOnly;
        }
    }
    void finish() override { owner->loadFinished(*this); }
    const char* kind() const override { return "load"; }
};

// Light + Update Light + Chunk Data for one player, from a snapshot.
class SendJob : public Job {
public:
    ChunkJobs* owner = nullptr;
    Chunk* snap = nullptr;
    NeighbourEdges edges;
    int threshold = -1;
    int slot = 0;
    uint32_t session = 0;
    int cx = 0, cz = 0;
    uint32_t version = 0;
    ByteBuf out;
    bool ok = false;

    ~SendJob() override { delete snap; }
    void run(WorkerScratch& ws) override {
        const Chunk& c = *snap;
        ok = ws.light.compute(c, edges) &&
             framePacket([&](Writer& w) { writeLightPacket(w, c, ws.light, false); }, threshold, ws.deflateWs, out,
                         ws.tmp) &&
             framePacket([&](Writer& w) { writeChunkPacket(w, c); }, threshold, ws.deflateWs, out, ws.tmp);
        delete snap;  // free the copy as early as possible
        snap = nullptr;
    }
    void finish() override { owner->sendFinished(*this); }
    const char* kind() const override { return "send"; }
};

// Recomputed light of a changed chunk, for everyone who has it.
class LightJob : public Job {
public:
    ChunkJobs* owner = nullptr;
    Chunk* snap = nullptr;
    NeighbourEdges edges;
    int threshold = -1;
    int cx = 0, cz = 0;
    ByteBuf out;
    bool ok = false;

    ~LightJob() override { delete snap; }
    void run(WorkerScratch& ws) override {
        const Chunk& c = *snap;
        ok = ws.light.compute(c, edges) &&
             framePacket([&](Writer& w) { writeLightPacket(w, c, ws.light, true); }, threshold, ws.deflateWs, out,
                         ws.tmp);
        delete snap;
        snap = nullptr;
    }
    void finish() override { owner->lightFinished(*this); }
    const char* kind() const override { return "light"; }
};

// Encodes (and compresses) a snapshot of a dirty chunk; the game loop writes it.
class SaveJob : public Job {
public:
    ChunkJobs* owner = nullptr;
    const ChunkStore* store = nullptr;
    Chunk* snap = nullptr;
    int cx = 0, cz = 0;
    ChunkRecord rec;
    bool ok = false;

    ~SaveJob() override { delete snap; }
    void run(WorkerScratch& ws) override {
        ok = store->encodeChunk(*snap, rec, ws.deflateWs);
        delete snap;
        snap = nullptr;
    }
    void finish() override { owner->saveFinished(*this); }
    const char* kind() const override { return "save"; }
};

// ------------------------------------------------------------------ ChunkJobs
// game-loop time spent on snapshots / storage lookups, for /lag
static void account(Server* srv, int part, uint64_t t0) {
    LagProfile& l = srv->lagNow();
    l.ms[part] = (uint16_t)(l.ms[part] + (plat::micros() - t0 + 500) / 1000);
}

void ChunkJobs::init(Server* srv, int workers) {
    srv_ = srv;
    if (workers > 0 && !q_.start(workers)) workers = 0;
    if (workers <= 0) q_.start(0);
    // enough loads in flight to keep every worker busy while results are applied
    maxLoads_ = q_.threaded() ? 2 * q_.workers() + 2 : 2;
}

void ChunkJobs::setWorkers(int workers) {
    q_.stop();  // drains first: every job in flight is finished
    if (workers > 0 && !q_.start(workers)) workers = 0;
    if (workers <= 0) q_.start(0);
    maxLoads_ = q_.threaded() ? 2 * q_.workers() + 2 : 2;
}

void ChunkJobs::stop() { q_.stop(); }

void ChunkJobs::poll() { q_.poll(q_.threaded() ? 64 : 2); }

void ChunkJobs::drain() { q_.drain(); }

int ChunkJobs::findPending(int cx, int cz) const {
    for (int i = 0; i < pendingCount_; i++)
        if (pending_[i].cx == cx && pending_[i].cz == cz) return i;
    return -1;
}

void ChunkJobs::onSyncLoad(int cx, int cz) {
    int i = findPending(cx, cz);
    if (i >= 0) pending_[i].superseded = true;
}

void ChunkJobs::unref(int cx, int cz) {
    Chunk* c = srv_->world.peek(cx, cz);
    if (c && c->jobRefs) c->jobRefs--;
}

Chunk* ChunkJobs::acquire(int cx, int cz) {
    Chunk* c = srv_->world.get(cx, cz);
    if (c) return c;
    LoadBatch b;
    beginBatch(b);
    want(b, cx, cz);
    requestLoads(b);
    return srv_->world.get(cx, cz);  // stores without split I/O load synchronously
}

void ChunkJobs::beginBatch(LoadBatch& b) const {
    b.n = 0;
    int cap = maxLoads_ - loadsInFlight_;
    if (MAX_PENDING - pendingCount_ < cap) cap = MAX_PENDING - pendingCount_;
    if (cap > LoadBatch::MAX) cap = LoadBatch::MAX;
    b.cap = cap > 0 ? cap : 0;
}

void ChunkJobs::want(LoadBatch& b, int cx, int cz) const {
    if (b.full() || findPending(cx, cz) >= 0 || srv_->world.isResident(cx, cz)) return;
    b.add(cx, cz);
}

void ChunkJobs::requestLoads(const LoadBatch& b) {
    if (b.n == 0) return;
    World& w = srv_->world;
    ChunkStore* st = w.store();
    LoadJob* jobs[LoadBatch::MAX];
    ChunkRecord* recs[LoadBatch::MAX];
    int32_t fx[LoadBatch::MAX], fz[LoadBatch::MAX];
    LoadJob* fjobs[LoadBatch::MAX];
    int nj = 0, nf = 0;
    for (int i = 0; i < b.n; i++) {
        int cx = b.cx[i], cz = b.cz[i];
        if (w.isResident(cx, cz) || findPending(cx, cz) >= 0 || pendingCount_ + nj >= MAX_PENDING) continue;
        bool stored = st && w.chunkInBounds(cx, cz) && st->chunkInRange(cx, cz);
        if (stored && !st->splitIo()) {
            w.load(cx, cz);  // this store only loads synchronously
            continue;
        }
        LoadJob* j = new LoadJob();
        j->owner = this;
        j->cx = cx;
        j->cz = cz;
        j->gen = &w.generator();
        jobs[nj++] = j;
        if (stored) {
            fx[nf] = cx;
            fz[nf] = cz;
            recs[nf] = &j->rec;
            fjobs[nf++] = j;
        }
    }
    // the storage I/O happens here, on the game loop (pipelined NBD reads, not CPU);
    // decoding is the job
    if (nf) {
        LoadResult res[LoadBatch::MAX];
        uint64_t t0 = plat::micros();
        st->fetchChunks(nf, fx, fz, recs, res);
        account(srv_, LagProfile::P_FETCH, t0);
        for (int i = 0; i < nf; i++) {
            if (res[i] == LOAD_OK) {
                fjobs[i]->store = st;
            } else if (res[i] == LOAD_ERROR) {
                fjobs[i]->readOnly = true;   // show generated terrain, protect the stored copy
                w.noteLoadError();
            }
        }
    }
    for (int i = 0; i < nj; i++) {
        pending_[pendingCount_++] = PendingLoad{jobs[i]->cx, jobs[i]->cz, false};
        loadsInFlight_++;
        q_.submit(jobs[i]);
    }
}

void ChunkJobs::loadFinished(LoadJob& j) {
    loadsInFlight_--;
    int i = findPending(j.cx, j.cz);
    bool superseded = i >= 0 && pending_[i].superseded;
    if (i >= 0) pending_[i] = pending_[--pendingCount_];
    if (superseded) return;  // loaded synchronously meanwhile: that copy (or its save) wins
    if (j.failed) {
        // a damaged newest copy: the synchronous path also tries the older one
        if (j.store) srv_->world.load(j.cx, j.cz);
        return;
    }
    srv_->world.adopt(j.result, j.store == nullptr);
    j.result = nullptr;
    if (j.store) {
        stats_.decoded++;
    } else {
        stats_.generated++;
        stats_.genUs = j.runUs;
    }
}

bool ChunkJobs::sendChunk(Player& p, Chunk& c) {
    uint64_t t0 = plat::micros();
    Chunk* snap = c.clone();
    account(srv_, LagProfile::P_SNAPSHOT, t0);
    if (!snap) return false;
    SendJob* j = new SendJob();
    j->owner = this;
    j->snap = snap;
    j->edges.gather(srv_->world, c.cx, c.cz);
    j->threshold = p.conn.compression();
    j->slot = p.slot;
    j->session = p.session;
    j->cx = c.cx;
    j->cz = c.cz;
    j->version = c.version;
    c.jobRefs++;
    p.pendingSends++;
    q_.submit(j);
    return true;
}

void ChunkJobs::sendFinished(SendJob& j) {
    unref(j.cx, j.cz);
    Player& p = srv_->players[j.slot];
    if (p.session != j.session) return;  // the player left (the slot may hold someone else)
    p.pendingSends--;
    uint8_t* cell = p.viewCell(j.cx, j.cz);
    if (!cell || *cell != VIEW_PENDING) return;  // moved away, or the view was reset
    const Chunk* live = srv_->world.peek(j.cx, j.cz);
    if (!j.ok || !live || live->version != j.version || !p.inPlay()) {
        *cell = VIEW_NONE;  // changed while it was prepared: send it again
        stats_.retried++;
        return;
    }
    p.conn.sendRaw(j.out.data(), j.out.size());
    *cell = VIEW_SENT;
    stats_.sent++;
    stats_.sendUs = j.runUs;
}

bool ChunkJobs::resendLight(Chunk& c) {
    if (lightInFlight_ >= 4) return false;
    Chunk* snap = c.clone();
    if (!snap) return false;
    LightJob* j = new LightJob();
    j->owner = this;
    j->snap = snap;
    j->edges.gather(srv_->world, c.cx, c.cz);
    j->threshold = srv_->cfg.compressionThreshold < 0 ? -1 : srv_->cfg.compressionThreshold;
    j->cx = c.cx;
    j->cz = c.cz;
    c.jobRefs++;
    lightInFlight_++;
    q_.submit(j);
    return true;
}

void ChunkJobs::lightFinished(LightJob& j) {
    lightInFlight_--;
    unref(j.cx, j.cz);
    if (!j.ok) return;
    // a later change queues another resend, so even a stale snapshot is fine to send
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& p = srv_->players[i];
        if (p.inPlay() && p.hasChunk(j.cx, j.cz) && p.conn.compression() == j.threshold)
            p.conn.sendRaw(j.out.data(), j.out.size());
    }
    stats_.lightResends++;
}

int ChunkJobs::saveDirty(int max) {
    World& w = srv_->world;
    ChunkStore* st = w.store();
    if (!st || !st->splitIo()) return w.saveDirty(max);
    int n = 0;
    for (int i = 0; i < w.tableSize() && n < max && savesInFlight_ < 4; i++) {
        Chunk* c = w.slot(i);
        if (!c || !c->dirty || c->saving) continue;
        if (c->readOnly || !w.chunkInBounds(c->cx, c->cz) || !st->chunkInRange(c->cx, c->cz)) {
            c->dirty = false;  // cannot (or must not) be stored: same as World::saveDirty
            continue;
        }
        Chunk* snap = c->clone();
        if (!snap) break;
        SaveJob* j = new SaveJob();
        j->owner = this;
        j->store = st;
        j->snap = snap;
        j->cx = c->cx;
        j->cz = c->cz;
        c->dirty = false;   // changes from now on make it dirty again
        c->saving = true;
        c->jobRefs++;
        savesInFlight_++;
        q_.submit(j);
        n++;
    }
    return n;
}

void ChunkJobs::saveFinished(SaveJob& j) {
    savesInFlight_--;
    World& w = srv_->world;
    Chunk* live = w.peek(j.cx, j.cz);  // resident: chunks with jobs are never evicted
    if (!live) return;
    if (live->jobRefs) live->jobRefs--;
    live->saving = false;
    ChunkStore* st = w.store();
    if (j.ok && st && st->writeChunk(*live, j.rec)) {
        w.noteSaved();
        stats_.saved++;
    } else {
        live->dirty = true;  // try again with the next save
        w.noteSaveError();
    }
}

void ChunkJobs::statusLine(char* buf, size_t cap) {
    char q[96];
    q_.statusLine(q, sizeof(q));
    snprintf(buf, cap, "%s; last generate %.1f ms, send %.1f ms", q, stats_.genUs / 1000.0, stats_.sendUs / 1000.0);
}

}  // namespace mc
