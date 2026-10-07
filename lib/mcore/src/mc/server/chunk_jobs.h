// The server's background chunk work (see mc/jobs.h):
//  * loading: chunks that must become resident are generated, or decoded from their
//    stored record, on a worker; the storage I/O itself stays on the game loop
//  * sending: light, Chunk Data and Update Light packets are computed, encoded and
//    compressed on a worker from a snapshot of the chunk
//  * light resends after block changes, and chunk saves (encoding + compression)
// The game loop only snapshots chunks (Chunk::clone) and applies results. A chunk with
// jobs in flight is never evicted, and a send is dropped (and retried) when the chunk
// changed while it was being prepared.
#pragma once
#include <stdint.h>
#include "mc/jobs.h"
#include "mc/world/world.h"

namespace mc {

class Server;
class Player;
class LoadJob;
class SendJob;
class LightJob;
class SaveJob;

// Chunks the players need that are not resident, collected during a tick so that their
// storage lookups share one pipelined round trip (see ChunkJobs::requestLoads).
struct LoadBatch {
    static const int MAX = 16;
    int32_t cx[MAX], cz[MAX];
    int n = 0;
    int cap = MAX;     // loads that may still be started
    bool full() const { return n >= cap; }
    void add(int x, int z) {
        for (int i = 0; i < n; i++)
            if (cx[i] == x && cz[i] == z) return;
        if (n < cap) {
            cx[n] = x;
            cz[n] = z;
            n++;
        }
    }
};

struct ChunkJobStats {
    uint32_t generated = 0, decoded = 0, sent = 0, retried = 0, lightResends = 0, saved = 0;
    uint32_t genUs = 0, sendUs = 0;   // run time of the last generate / send job
};

class ChunkJobs {
public:
    static const int MAX_SENDS_PER_PLAYER = 4;

    void init(Server* srv, int workers);
    void setWorkers(int workers);   // drains, then restarts the pool (0 = inline)
    void stop();
    void poll();     // game loop: apply finished jobs
    void drain();    // wait for everything in flight (before a full save)

    // Resident chunk, or nullptr after starting to load it in the background
    // (nothing is started while too many loads are in flight: see loadsFull()).
    Chunk* acquire(int cx, int cz);
    bool loadsFull() const { return loadsInFlight_ >= maxLoads_; }
    // A batch sized to the loads that may still start now.
    void beginBatch(LoadBatch& b) const;
    // Adds (cx, cz) to b unless it is resident or already being loaded.
    void want(LoadBatch& b, int cx, int cz) const;
    // Starts loading every chunk in b: one storage round trip for all their headers (and
    // one more for the records of those that were stored), then decode/generate jobs.
    void requestLoads(const LoadBatch& b);
    // Starts preparing chunk c for player p (the caller marks the view cell in flight).
    bool sendChunk(Player& p, Chunk& c);
    // Resends the light of c to every player that has it. false: too busy, retry later.
    bool resendLight(Chunk& c);
    // Starts saving up to max dirty chunks; returns the number started.
    int saveDirty(int max);
    int savesInFlight() const { return savesInFlight_; }
    // The world loaded (cx, cz) synchronously: a background load of it is stale now.
    void onSyncLoad(int cx, int cz);

    JobQueue& queue() { return q_; }
    const ChunkJobStats& stats() const { return stats_; }
    void statusLine(char* buf, size_t cap);

private:
    friend class LoadJob;
    friend class SendJob;
    friend class LightJob;
    friend class SaveJob;
    struct PendingLoad {
        int32_t cx, cz;
        bool superseded;
    };
    void loadFinished(LoadJob& j);
    void sendFinished(SendJob& j);
    void lightFinished(LightJob& j);
    void saveFinished(SaveJob& j);
    void unref(int cx, int cz);
    int findPending(int cx, int cz) const;

    Server* srv_ = nullptr;
    JobQueue q_;
    static const int MAX_PENDING = 16;
    PendingLoad pending_[MAX_PENDING];
    int pendingCount_ = 0;
    int loadsInFlight_ = 0, maxLoads_ = 2;
    int lightInFlight_ = 0, savesInFlight_ = 0;
    ChunkJobStats stats_;
};

}  // namespace mc
