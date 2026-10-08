#include "mc/server/chunk_jobs.h"
#include <stdio.h>
#include <stdlib.h>
#include "mc/server/chunk_codec.h"
#include "mc/server/server.h"

namespace mc {

// ------------------------------------------------------------------ jobs
// Generates a chunk, or decodes it from its stored record.
class LoadJob : public Job {
public:
    ChunkJobs* owner = nullptr;
    int cx = 0, cz = 0;
    uint8_t dim = 0;
    const Generator* gen = nullptr;
    const ChunkStore* store = nullptr;   // set: decode rec instead of generating
    ChunkRecord rec;
    bool readOnly = false;               // storage failed: generate, never overwrite the stored copy
    Chunk* result = nullptr;
    bool failed = false;
    bool fetching = false; // game loop only; CPU job not submitted yet

    ~LoadJob() override { delete result; }
    void run(WorkerScratch&) override {
        result = new Chunk(cx, cz, dim);
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

// The I/O stage owns the records until publication; CPU jobs are submitted only
// from finish(), so neither queue can delete a job while the other uses it.
class FetchTask : public StorageTask {
public:
    ChunkJobs* owner;
    int n = 0;
    int32_t x[LoadBatch::MAX], z[LoadBatch::MAX];
    uint8_t d[LoadBatch::MAX];
    LoadJob* jobs[LoadBatch::MAX];
    ChunkRecord* records[LoadBatch::MAX];
    LoadResult results[LoadBatch::MAX];
    explicit FetchTask(ChunkJobs* o) : owner(o) {}
    void run(Storage& s) override { s.fetchChunks(n, d, x, z, records, results); }
    void finish() override {
        for (int k = 0; k < n; ++k) {
            auto* j = jobs[k];
            j->fetching = false;
            int i = owner->findPending(j->dim, j->cx, j->cz);
            if (i < 0) { delete j; continue; }
            if (results[k] == LOAD_OK) j->store = owner->srv_->storage;
            else if (results[k] == LOAD_ERROR) {
                j->readOnly = true;
                owner->srv_->world.noteLoadError();
            }
            if (owner->pending_[i].superseded) { owner->loadFinished(*j); delete j; }
            else owner->q_.submit(j, (JobPriority)owner->pending_[i].prio);
        }
    }
};

// Rare corruption recovery also belongs to the I/O thread: loadChunk tries the
// older A/B record if the newest record's payload cannot be decoded.
class FallbackTask : public StorageTask {
public:
    ChunkJobs* owner;
    LoadJob* job;
    LoadResult result = LOAD_ERROR;
    FallbackTask(ChunkJobs* o, LoadJob* j) : owner(o), job(j) {}
    void run(Storage& s) override {
        job->result = new Chunk(job->cx, job->cz, job->dim);
        result = s.loadChunk(*job->result);
    }
    void finish() override {
        job->fetching = false;
        if (result == LOAD_OK) {
            job->result->recomputeHeightmap();
            job->result->dirty = false;
            job->result->lightDirty = true;
            job->failed = false;
            owner->loadFinished(*job);
            delete job;
        } else {
            delete job->result; job->result = nullptr;
            job->store = nullptr; job->failed = false;
            job->readOnly = result == LOAD_ERROR;
            if (job->readOnly) owner->srv_->world.noteLoadError();
            int i = owner->findPending(job->dim, job->cx, job->cz);
            owner->q_.submit(job, i >= 0 ? (JobPriority)owner->pending_[i].prio : PRIO_NORMAL);
        }
    }
};

class WriteTask : public StorageTask {
public:
    ChunkJobs* owner;
    Chunk header; // private coordinates + A/B sequence, never a live chunk
    ChunkRecord rec;
    bool ok = false;
    WriteTask(ChunkJobs* o, const Chunk& c) : owner(o), header(c.cx, c.cz, c.dim) {
        header.storeSeq = c.storeSeq; header.storeSlot = c.storeSlot;
    }
    void run(Storage& s) override {
        ok = s.writeChunk(header, rec);
        // NBD writes are posted. Only an acknowledged flush makes the save clean.
        if (ok) ok = s.flush();
    }
    void finish() override { owner->writeFinished(*this); }
};

// What a light computation needs: shared snapshots of the chunk and, for exact light
// across borders, its 8 neighbours (nine[(dz + 1) * 3 + (dx + 1)]); otherwise the
// neighbours' border heights only. Released when the job is deleted (game loop).
struct LightInput {
    ChunkSnap* nine[9] = {};
    bool region = false;
    NeighbourEdges edges;
    ~LightInput() {
        for (ChunkSnap* s : nine)
            if (s) s->release();
    }
    const Chunk& centre() const { return *nine[4]->chunk; }
    bool compute(ChunkLight& L) const {
        if (!region) return L.compute(centre(), edges);
        const Chunk* chunks[9];
        for (int k = 0; k < 9; k++) chunks[k] = nine[k]->chunk;
        return L.computeRegion(chunks);
    }
    // false when out of memory. exact: use the neighbours if they are all resident.
    bool take(World& w, uint8_t dim, int cx, int cz, bool exact) {
        if (exact) {
            region = true;
            for (int k = 0; k < 9 && region; k++)
                if (k != 4 && !w.peek(dim, cx + k % 3 - 1, cz + k / 3 - 1)) region = false;
        }
        if (region) {
            for (int k = 0; k < 9; k++)
                if (!(nine[k] = w.snapshot(dim, cx + k % 3 - 1, cz + k / 3 - 1))) return false;
            return true;
        }
        edges.gather(w, dim, cx, cz);
        return (nine[4] = w.snapshot(dim, cx, cz)) != nullptr;
    }
};

// Light + Update Light + Chunk Data for one player, from a snapshot.
class SendJob : public Job {
public:
    ChunkJobs* owner = nullptr;
    LightInput in;
    int threshold = -1;
    int slot = 0;
    uint32_t session = 0;
    int cx = 0, cz = 0;
    uint8_t dim = 0;
    uint32_t version = 0;
    ByteBuf out;
    bool ok = false;
    SendJob* prevSend = nullptr;   // ChunkJobs::sends_ list (game loop only)
    SendJob* nextSend = nullptr;

    void run(WorkerScratch& ws) override {
        const Chunk& c = in.centre();
        uint64_t t0 = plat::micros();
        ok = in.compute(ws.light);
        lightUs = (uint32_t)(plat::micros() - t0);
        ok = ok && framePacket([&](Writer& w) { writeChunkPacket(w, c, ws.light); }, threshold, ws.deflateWs, out, ws.tmp);
    }
    uint32_t lightUs = 0;
    void finish() override { owner->sendFinished(*this); }
    const char* kind() const override { return "send"; }
};

// Recomputed light of a changed chunk, for everyone who has it.
class LightJob : public Job {
public:
    ChunkJobs* owner = nullptr;
    LightInput in;
    int threshold = -1;
    int cx = 0, cz = 0;
    uint8_t dim = 0;
    ByteBuf out;
    bool ok = false;

    void run(WorkerScratch& ws) override {
        const Chunk& c = in.centre();
        uint64_t t0 = plat::micros();
        ok = in.compute(ws.light);
        lightUs = (uint32_t)(plat::micros() - t0);
        ok = ok && framePacket([&](Writer& w) { writeLightPacket(w, c, ws.light, true); }, threshold, ws.deflateWs, out,
                               ws.tmp);
    }
    uint32_t lightUs = 0;
    void finish() override { owner->lightFinished(*this); }
    const char* kind() const override { return "light"; }
};

// Encodes a snapshot, then hands its record to the dedicated I/O thread.
class SaveJob : public Job {
public:
    ChunkJobs* owner = nullptr;
    const ChunkStore* store = nullptr;
    Chunk* snap = nullptr;
    WriteTask* write = nullptr;
    bool ok = false;
    ~SaveJob() override { delete snap; delete write; }
    void run(WorkerScratch& ws) override {
        ok = store->encodeChunk(*snap, write->rec, ws.deflateWs);
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

// enough loads in flight to keep every worker busy while results are applied; urgent
// (regular + reserve) and other loads (regular, or the share) must both fit in pending_
int ChunkJobs::loadSlots(int workers) {
    int m = workers > 0 ? 2 * workers + 2 : 2;
    int cap = (MAX_PENDING - URGENT_RESERVE) / 2;
    static_assert(NON_URGENT_SHARE <= (MAX_PENDING - URGENT_RESERVE) / 2, "the share must fit");
    return m < cap ? m : cap;
}

void ChunkJobs::init(Server* srv, int workers) {
    srv_ = srv;
    if (workers > 0 && !q_.start(workers)) workers = 0;
    if (workers <= 0) q_.start(0);
    maxLoads_ = loadSlots(q_.workers());
}

// Chebyshev distance of a chunk from a player's view centre
static int viewDistance(const Player& p, int cx, int cz) {
    int dx = abs(cx - p.centerCx), dz = abs(cz - p.centerCz);
    return dx > dz ? dx : dz;
}

void ChunkJobs::setWorkers(int workers) {
    drain();
    q_.stop();  // includes both I/O and CPU stages
    if (workers > 0 && !q_.start(workers)) workers = 0;
    if (workers <= 0) q_.start(0);
    maxLoads_ = loadSlots(q_.workers());
}

void ChunkJobs::stop() { if (srv_) drain(); q_.stop(); }

void ChunkJobs::poll() { srv_->storageIo.poll(); q_.poll(q_.threaded() ? 64 : 2); }

void ChunkJobs::drain() {
    if (!srv_) return;
    do {
        srv_->storageIo.poll();
        q_.poll();
        if (srv_->storageIo.inFlight() || q_.inFlight()) plat::delayMs(1);
    } while (srv_->storageIo.inFlight() || q_.inFlight());
}

int ChunkJobs::findPending(uint8_t dim, int cx, int cz) const {
    for (int i = 0; i < pendingCount_; i++)
        if (pending_[i].cx == cx && pending_[i].cz == cz && pending_[i].dim == dim) return i;
    return -1;
}

void ChunkJobs::onSyncLoad(uint8_t dim, int cx, int cz) {
    int i = findPending(dim, cx, cz);
    if (i >= 0) pending_[i].superseded = true;
}

void ChunkJobs::unref(uint8_t dim, int cx, int cz) {
    Chunk* c = srv_->world.peek(dim, cx, cz);
    if (c && c->jobRefs) c->jobRefs--;
}

Chunk* ChunkJobs::acquire(uint8_t dim, int cx, int cz) {
    Chunk* c = srv_->world.get(dim, cx, cz);
    if (c) return c;
    LoadBatch b;
    beginBatch(b);
    b.forPlayers = false;  // not tied to a player's view: never cancelled as stale
    want(b, dim, cx, cz, 0);
    requestLoads(b);
    return srv_->world.get(dim, cx, cz);  // stores without split I/O load synchronously
}

void ChunkJobs::beginBatch(LoadBatch& b) const {
    b.n = 0;
    b.cap = LoadBatch::MAX;
    b.forPlayers = true;
}

void ChunkJobs::want(LoadBatch& b, uint8_t dim, int cx, int cz, int dist, int player) {
    int i = findPending(dim, cx, cz);
    if (i >= 0) {
        // already loading: move it up if a player needs it more urgently now
        JobPriority p = prioForDistance(dist);
        if (p < pending_[i].prio && (pending_[i].job->fetching || q_.promote((Job*)pending_[i].job, p))) {
            pending_[i].prio = p;
            stats_.promoted++;
        }
        return;
    }
    if (srv_->world.isResident(dim, cx, cz)) return;
    b.add(dim, cx, cz, dist, player);
}

void ChunkJobs::requestLoads(const LoadBatch& b) {
    if (b.n == 0 || (srv_->storage && !srv_->storageIo.available())) return;
    World& w = srv_->world;
    ChunkStore* st = w.store();
    // closest first
    int order[LoadBatch::MAX];
    for (int i = 0; i < b.n; i++) {
        int k = i;
        while (k > 0 && b.dist[order[k - 1]] > b.dist[i]) {
            order[k] = order[k - 1];
            k--;
        }
        order[k] = i;
    }
    // admission (see the header): urgent first, then the guaranteed non-urgent share
    // round-robin over players, then the closest of the rest
    bool admit[LoadBatch::MAX] = {};
    bool asUrgent[LoadBatch::MAX] = {};
    int urgentNew = 0, otherNew = 0;
    for (int oi = 0; oi < b.n; oi++) {
        int i = order[oi];
        if (prioForDistance(b.dist[i]) != PRIO_URGENT) continue;
        // the regular slots plus the reserve, whatever the non-urgent share holds
        if (urgentInFlight_ + urgentNew >= maxLoads_ + URGENT_RESERVE) break;
        admit[i] = asUrgent[i] = true;
        urgentNew++;
    }
    int otherInFlight = loadsInFlight_ - urgentInFlight_;
    int start = (int)(rotation_++ % (MC_MAX_PLAYERS + 1));
    for (bool progress = true; progress && otherInFlight + otherNew < NON_URGENT_SHARE;) {
        progress = false;   // one pass: each player's closest remaining chunk, in rotation
        for (int r = 0; r <= MC_MAX_PLAYERS && otherInFlight + otherNew < NON_URGENT_SHARE; r++) {
            int owner = (start + r) % (MC_MAX_PLAYERS + 1) - 1;   // -1: loads not for a player
            for (int oi = 0; oi < b.n; oi++) {
                int i = order[oi];
                if (admit[i] || b.owner[i] != owner) continue;
                admit[i] = progress = true;
                otherNew++;
                break;
            }
        }
    }
    for (int oi = 0; oi < b.n; oi++) {
        int i = order[oi];
        if (admit[i]) continue;
        if (loadsInFlight_ + urgentNew + otherNew >= maxLoads_) break;
        admit[i] = true;
        otherNew++;
    }

    LoadJob* jobs[LoadBatch::MAX];
    auto* fetch = new FetchTask(this);
    uint8_t prios[LoadBatch::MAX];
    bool urgentSlot[LoadBatch::MAX];
    int nj = 0;
    for (int oi = 0; oi < b.n; oi++) {
        int i = order[oi];
        if (!admit[i]) continue;
        int cx = b.cx[i], cz = b.cz[i];
        uint8_t dim = b.dim[i];
        JobPriority prio = prioForDistance(b.dist[i]);
        if (w.isResident(dim, cx, cz) || findPending(dim, cx, cz) >= 0 || pendingCount_ + nj >= MAX_PENDING) continue;
        bool stored = st && w.chunkInBounds(cx, cz) && st->chunkInRange(cx, cz);
        if (stored && !st->splitIo()) {
            w.load(dim, cx, cz);  // this store only loads synchronously
            continue;
        }
        LoadJob* j = new LoadJob();
        j->owner = this;
        j->cx = cx;
        j->cz = cz;
        j->dim = dim;
        j->gen = &w.generator(dim);
        prios[nj] = prio;
        urgentSlot[nj] = asUrgent[i];
        jobs[nj++] = j;
        if (stored) {
            int k = fetch->n++;
            fetch->x[k] = cx; fetch->z[k] = cz; fetch->d[k] = dim;
            fetch->records[k] = &j->rec; fetch->jobs[k] = j;
            j->fetching = true;
        }
    }
    for (int i = 0; i < nj; i++) {
        pending_[pendingCount_++] =
            PendingLoad{jobs[i]->cx, jobs[i]->cz, jobs[i]->dim, false, b.forPlayers, urgentSlot[i], prios[i], jobs[i]};
        loadsInFlight_++;
        if (urgentSlot[i]) urgentInFlight_++;
        if (!jobs[i]->fetching) q_.submit(jobs[i], (JobPriority)prios[i]);
    }
    if (fetch->n) srv_->storageIo.submit(fetch); // admission reserved above; no intervening submissions
    else delete fetch;
}

void ChunkJobs::cancelStale() {
    // loads: cancel the ones no player can see any more (if they have not started)
    for (int i = 0; i < pendingCount_; i++) {
        PendingLoad& pl = pending_[i];
        if (!pl.forPlayers || pl.superseded) continue;
        bool seen = false;
        for (int k = 0; k < MC_MAX_PLAYERS && !seen; k++) {
            const Player& p = srv_->players[k];
            // one chunk of slack: walking back and forth over a border must not
            // cancel and re-request the edge of the view every time
            seen = p.inPlay() && p.viewReady && p.e.dim == pl.dim && viewDistance(p, pl.cx, pl.cz) <= p.viewDist + 1;
        }
        if (!seen) {
            if (pl.job->fetching) { pl.superseded = true; stats_.cancelled++; }
            else if (q_.cancel((Job*)pl.job)) stats_.cancelled++;
        }
    }
    // sends: cancel the ones whose cell is gone (player moved, respawned or left), and
    // promote the ones whose player came closer
    for (SendJob* j = sends_; j; j = j->nextSend) {
        if (j->cancelled()) continue;
        Player& p = srv_->players[j->slot];
        uint8_t* cell = p.session == j->session && p.e.dim == j->dim ? p.viewCell(j->cx, j->cz) : nullptr;
        if (!cell || *cell != VIEW_PENDING) {
            if (q_.cancel(j)) stats_.cancelled++;
            continue;
        }
        JobPriority want = prioForDistance(viewDistance(p, j->cx, j->cz));
        if (want < j->priority() && q_.promote(j, want)) stats_.promoted++;
    }
}

void ChunkJobs::loadFinished(LoadJob& j) {
    int pending = findPending(j.dim, j.cx, j.cz);
    if (j.failed && j.store && !j.cancelled() && pending >= 0 && !pending_[pending].superseded) {
        // The CPU queue deletes j after finish: move its pending identity to a new job.
        auto* retry = new LoadJob();
        retry->owner = this; retry->cx = j.cx; retry->cz = j.cz; retry->dim = j.dim;
        retry->gen = j.gen; retry->store = j.store; retry->fetching = true;
        auto* task = new FallbackTask(this, retry);
        if (srv_->storageIo.submit(task)) { pending_[pending].job = retry; return; }
        delete task; delete retry; // full: drop this attempt; the view requests it again
    }
    loadsInFlight_--;
    int i = findPending(j.dim, j.cx, j.cz);
    bool superseded = i >= 0 && pending_[i].superseded;
    if (i >= 0 && pending_[i].urgentSlot) urgentInFlight_--;
    if (i >= 0) pending_[i] = pending_[--pendingCount_];
    if (superseded || j.cancelled()) return;  // loaded synchronously meanwhile (that copy wins), or stale
    if (j.failed) return;
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
    SendJob* j = new SendJob();
    int d = viewDistance(p, c.cx, c.cz);
    bool exact = d <= srv_->cfg.exactLightDistance;
    bool ok = j->in.take(srv_->world, c.dim, c.cx, c.cz, exact && regionLightFree());
    account(srv_, LagProfile::P_SNAPSHOT, t0);
    if (!ok) {
        delete j;
        return false;
    }
    if (j->in.region) regionInFlight_++;
    // near a player, but a neighbour is missing or exact light is busy: resent later
    else if (exact) c.lightPartial = true;
    j->owner = this;
    j->threshold = p.conn.compression();
    j->slot = p.slot;
    j->session = p.session;
    j->cx = c.cx;
    j->cz = c.cz;
    j->dim = c.dim;
    j->version = c.version;
    c.jobRefs++;
    p.pendingSends++;
    j->nextSend = sends_;
    if (sends_) sends_->prevSend = j;
    sends_ = j;
    q_.submit(j, prioForDistance(viewDistance(p, c.cx, c.cz)));
    return true;
}

void ChunkJobs::sendFinished(SendJob& j) {
    if (j.in.region) regionInFlight_--;
    if (j.prevSend) j.prevSend->nextSend = j.nextSend;
    else sends_ = j.nextSend;
    if (j.nextSend) j.nextSend->prevSend = j.prevSend;
    unref(j.dim, j.cx, j.cz);
    Player& p = srv_->players[j.slot];
    if (p.session != j.session) return;  // the player left (the slot may hold someone else)
    p.pendingSends--;
    // cancelled: its cell had already left VIEW_PENDING; if the cell is pending again,
    // that belongs to a newer send of the same chunk
    if (j.cancelled()) return;
    uint8_t* cell = p.e.dim == j.dim ? p.viewCell(j.cx, j.cz) : nullptr;
    if (!cell || *cell != VIEW_PENDING) return;  // moved away, or the view was reset
    const Chunk* live = srv_->world.peek(j.dim, j.cx, j.cz);
    if (!j.ok || !live || live->version != j.version || !p.inPlay()) {
        *cell = VIEW_NONE;  // changed while it was prepared: send it again
        stats_.retried++;
        return;
    }
    p.conn.sendRaw(j.out.data(), j.out.size());
    *cell = VIEW_SENT;
    stats_.sent++;
    stats_.sendUs = j.runUs;
    noteLight(j.in.region, j.lightUs);
}

bool ChunkJobs::resendLight(Chunk& c) {
    if (lightInFlight_ >= 4) return false;
    // exact when anyone who has the chunk is close enough
    bool exact = false;
    for (int i = 0; i < MC_MAX_PLAYERS && !exact; i++) {
        const Player& p = srv_->players[i];
        exact = p.inPlay() && p.hasChunk(c.dim, c.cx, c.cz) && viewDistance(p, c.cx, c.cz) <= srv_->cfg.exactLightDistance;
    }
    if (exact && !regionLightFree()) return false;   // retried next tick
    LightJob* j = new LightJob();
    if (!j->in.take(srv_->world, c.dim, c.cx, c.cz, exact)) {
        delete j;
        return false;
    }
    if (j->in.region) regionInFlight_++;
    c.lightPartial = exact && !j->in.region;
    j->owner = this;
    j->threshold = srv_->cfg.compressionThreshold < 0 ? -1 : srv_->cfg.compressionThreshold;
    j->cx = c.cx;
    j->cz = c.cz;
    j->dim = c.dim;
    c.jobRefs++;
    lightInFlight_++;
    q_.submit(j, PRIO_HIGH);  // players are looking at the change
    return true;
}

void ChunkJobs::lightFinished(LightJob& j) {
    lightInFlight_--;
    if (j.in.region) regionInFlight_--;
    unref(j.dim, j.cx, j.cz);
    if (!j.ok) return;
    noteLight(j.in.region, j.lightUs);
    // a later change queues another resend, so even a stale snapshot is fine to send
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& p = srv_->players[i];
        if (p.inPlay() && p.hasChunk(j.dim, j.cx, j.cz) && p.conn.compression() == j.threshold)
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
        if (!saveChunk(*c)) break;
        n++;
    }
    return n;
}

bool ChunkJobs::saveChunk(Chunk& c) {
    if (c.readOnly || !srv_->storage || !srv_->storage->splitIo()) return false;
    if (c.saving || savesInFlight_ >= 4 || !srv_->storageIo.available()) return false;
    srv_->prepareChunkSave(c);
    Chunk* snap = c.clone();
    if (!snap) return false;
    srv_->attachTicks(*snap);
    auto* j = new SaveJob();
    j->owner = this; j->store = srv_->world.store(); j->snap = snap;
    j->write = new WriteTask(this, c);
    c.dirty = false; c.saving = true; c.jobRefs++;
    savesInFlight_++;
    q_.submit(j, PRIO_BACKGROUND);
    return true;
}

void ChunkJobs::saveFinished(SaveJob& j) {
    if (j.ok && srv_->storageIo.submit(j.write)) { j.write = nullptr; return; }
    writeFinished(*j.write); // encoding/backpressure failure: keep dirty and retry later
}

void ChunkJobs::writeFinished(WriteTask& j) {
    savesInFlight_--;
    World& w = srv_->world;
    Chunk* live = w.peek(j.header.dim, j.header.cx, j.header.cz);
    if (!live) return;
    if (live->jobRefs) live->jobRefs--;
    live->saving = false;
    if (j.ok) {
        live->storeSeq = j.header.storeSeq; live->storeSlot = j.header.storeSlot;
        w.noteSaved(); stats_.saved++;
    } else {
        live->dirty = true;
        w.noteSaveError();
    }
}

int ChunkJobs::pinnedChunks() const {
    const World& w = srv_->world;
    int n = 0;
    for (int i = 0; i < w.tableSize(); i++) {
        const Chunk* c = const_cast<World&>(w).slot(i);
        if (c && c->jobRefs) n++;
    }
    return n;
}

void ChunkJobs::noteLight(bool exact, uint32_t us) {
    if (exact) {
        stats_.lightExact++;
        stats_.lightExactUs += us;
        if (us > stats_.lightExactMaxUs) stats_.lightExactMaxUs = us;
    } else {
        stats_.lightChunk++;
        stats_.lightChunkUs += us;
        if (us > stats_.lightChunkMaxUs) stats_.lightChunkMaxUs = us;
    }
}

void ChunkJobs::statusLine(char* buf, size_t cap) {
    char q[128];
    q_.statusLine(q, sizeof(q));
    snprintf(buf, cap, "%s; %d chunks pinned by jobs; last generate %.1f ms, send %.1f ms", q, pinnedChunks(),
             stats_.genUs / 1000.0, stats_.sendUs / 1000.0);
}

}  // namespace mc
