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
    SendJob* prevSend = nullptr;   // ChunkJobs::sends_ list (game loop only)
    SendJob* nextSend = nullptr;

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
    q_.stop();  // drains first: every job in flight is finished
    if (workers > 0 && !q_.start(workers)) workers = 0;
    if (workers <= 0) q_.start(0);
    maxLoads_ = loadSlots(q_.workers());
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
    b.forPlayers = false;  // not tied to a player's view: never cancelled as stale
    want(b, cx, cz, 0);
    requestLoads(b);
    return srv_->world.get(cx, cz);  // stores without split I/O load synchronously
}

void ChunkJobs::beginBatch(LoadBatch& b) const {
    b.n = 0;
    b.cap = LoadBatch::MAX;
    b.forPlayers = true;
}

void ChunkJobs::want(LoadBatch& b, int cx, int cz, int dist, int player) {
    int i = findPending(cx, cz);
    if (i >= 0) {
        // already loading: move it up if a player needs it more urgently now
        JobPriority p = prioForDistance(dist);
        if (p < pending_[i].prio && q_.promote((Job*)pending_[i].job, p)) {
            pending_[i].prio = p;
            stats_.promoted++;
        }
        return;
    }
    if (srv_->world.isResident(cx, cz)) return;
    b.add(cx, cz, dist, player);
}

void ChunkJobs::requestLoads(const LoadBatch& b) {
    if (b.n == 0) return;
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
    ChunkRecord* recs[LoadBatch::MAX];
    int32_t fx[LoadBatch::MAX], fz[LoadBatch::MAX];
    LoadJob* fjobs[LoadBatch::MAX];
    uint8_t prios[LoadBatch::MAX];
    bool urgentSlot[LoadBatch::MAX];
    int nj = 0, nf = 0;
    for (int oi = 0; oi < b.n; oi++) {
        int i = order[oi];
        if (!admit[i]) continue;
        int cx = b.cx[i], cz = b.cz[i];
        JobPriority prio = prioForDistance(b.dist[i]);
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
        prios[nj] = prio;
        urgentSlot[nj] = asUrgent[i];
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
        pending_[pendingCount_++] =
            PendingLoad{jobs[i]->cx, jobs[i]->cz, false, b.forPlayers, urgentSlot[i], prios[i], jobs[i]};
        loadsInFlight_++;
        if (urgentSlot[i]) urgentInFlight_++;
        q_.submit(jobs[i], (JobPriority)prios[i]);
    }
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
            seen = p.inPlay() && p.viewReady && viewDistance(p, pl.cx, pl.cz) <= p.viewDist + 1;
        }
        if (!seen && q_.cancel((Job*)pl.job)) stats_.cancelled++;
    }
    // sends: cancel the ones whose cell is gone (player moved, respawned or left), and
    // promote the ones whose player came closer
    for (SendJob* j = sends_; j; j = j->nextSend) {
        if (j->cancelled()) continue;
        Player& p = srv_->players[j->slot];
        uint8_t* cell = p.session == j->session ? p.viewCell(j->cx, j->cz) : nullptr;
        if (!cell || *cell != VIEW_PENDING) {
            if (q_.cancel(j)) stats_.cancelled++;
            continue;
        }
        JobPriority want = prioForDistance(viewDistance(p, j->cx, j->cz));
        if (want < j->priority() && q_.promote(j, want)) stats_.promoted++;
    }
}

void ChunkJobs::loadFinished(LoadJob& j) {
    loadsInFlight_--;
    int i = findPending(j.cx, j.cz);
    bool superseded = i >= 0 && pending_[i].superseded;
    if (i >= 0 && pending_[i].urgentSlot) urgentInFlight_--;
    if (i >= 0) pending_[i] = pending_[--pendingCount_];
    if (superseded || j.cancelled()) return;  // loaded synchronously meanwhile (that copy wins), or stale
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
    j->nextSend = sends_;
    if (sends_) sends_->prevSend = j;
    sends_ = j;
    q_.submit(j, prioForDistance(viewDistance(p, c.cx, c.cz)));
    return true;
}

void ChunkJobs::sendFinished(SendJob& j) {
    if (j.prevSend) j.prevSend->nextSend = j.nextSend;
    else sends_ = j.nextSend;
    if (j.nextSend) j.nextSend->prevSend = j.prevSend;
    unref(j.cx, j.cz);
    Player& p = srv_->players[j.slot];
    if (p.session != j.session) return;  // the player left (the slot may hold someone else)
    p.pendingSends--;
    // cancelled: its cell had already left VIEW_PENDING; if the cell is pending again,
    // that belongs to a newer send of the same chunk
    if (j.cancelled()) return;
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
    q_.submit(j, PRIO_HIGH);  // players are looking at the change
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
        srv_->prepareChunkSave(*c);   // furnace progress into the stored state
        Chunk* snap = c->clone();
        if (!snap) break;
        srv_->attachTicks(*snap);     // its pending block ticks are stored with it
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
        q_.submit(j, PRIO_BACKGROUND);  // nobody waits for it, but its deadline still comes
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

int ChunkJobs::pinnedChunks() const {
    const World& w = srv_->world;
    int n = 0;
    for (int i = 0; i < w.tableSize(); i++) {
        const Chunk* c = const_cast<World&>(w).slot(i);
        if (c && c->jobRefs) n++;
    }
    return n;
}

void ChunkJobs::statusLine(char* buf, size_t cap) {
    char q[128];
    q_.statusLine(q, sizeof(q));
    snprintf(buf, cap, "%s; %d chunks pinned by jobs; last generate %.1f ms, send %.1f ms", q, pinnedChunks(),
             stats_.genUs / 1000.0, stats_.sendUs / 1000.0);
}

}  // namespace mc
