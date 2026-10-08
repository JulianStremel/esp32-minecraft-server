// The server's background chunk work (see mc/jobs.h):
//  * loading: chunks that must become resident are generated, or decoded from their
//    stored record, on a worker; the storage I/O runs on its dedicated thread
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
class FetchTask;
class WriteTask;
class FallbackTask;

// How urgently a chunk d chunks (Chebyshev distance) from a player is needed.
inline JobPriority prioForDistance(int d) {
    return d <= 1 ? PRIO_URGENT : (d <= 3 ? PRIO_HIGH : PRIO_NORMAL);
}

// Chunks the players need that are not resident, collected during a tick so that their
// storage lookups share one pipelined round trip (see ChunkJobs::requestLoads). When it
// is full, closer chunks replace farther ones -- but every player keeps its closest
// candidate, so nobody is crowded out of the batch.
struct LoadBatch {
    static const int MAX = 16;
    int32_t cx[MAX], cz[MAX];
    uint8_t dim[MAX];
    uint8_t dist[MAX];   // distance to the nearest player that wants it
    int8_t owner[MAX];   // that player's slot (-1: not for a player)
    int n = 0;
    int cap = MAX;
    bool forPlayers = true;   // loads nobody can see any more are cancelled
    int ownerCount(int o) const {
        int k = 0;
        for (int i = 0; i < n; i++) k += owner[i] == o;
        return k;
    }
    void add(uint8_t dm, int x, int z, int d, int player = -1) {
        if (d > 255) d = 255;
        for (int i = 0; i < n; i++)
            if (cx[i] == x && cz[i] == z && dim[i] == dm) {
                if (d < dist[i]) {
                    dist[i] = (uint8_t)d;
                    owner[i] = (int8_t)player;
                }
                return;
            }
        int slot = n;
        if (n >= cap) {
            // replace the farthest entry of a player that has another one; a player without
            // any entry yet gets one even if everything in the batch is closer
            bool has = ownerCount(player) > 0;
            slot = -1;
            for (int i = 0; i < n; i++)
                if ((!has || dist[i] > d) && ownerCount(owner[i]) > 1 && (slot < 0 || dist[i] > dist[slot])) slot = i;
            if (slot < 0) return;
        } else {
            n++;
        }
        cx[slot] = x;
        cz[slot] = z;
        dim[slot] = dm;
        dist[slot] = (uint8_t)d;
        owner[slot] = (int8_t)player;
    }
};

struct ChunkJobStats {
    uint32_t generated = 0, decoded = 0, sent = 0, retried = 0, lightResends = 0, saved = 0;
    uint32_t cancelled = 0, promoted = 0;   // stale jobs dropped / jobs moved up
    uint32_t genUs = 0, sendUs = 0;   // run time of the last generate / send job
    // light computations of sends and light resends: exact (with the neighbours) or per chunk
    uint32_t lightExact = 0, lightChunk = 0;
    uint64_t lightExactUs = 0, lightChunkUs = 0;
    uint32_t lightExactMaxUs = 0, lightChunkMaxUs = 0;
};

class ChunkJobs {
public:
    static const int MAX_SENDS_PER_PLAYER = 4;   // +1 for the chunks under the player
    static const int URGENT_RESERVE = 4;         // extra load slots only urgent loads may use
    static const int NON_URGENT_SHARE = 2;       // load slots urgent loads can never take

    void init(Server* srv, int workers);
    void setWorkers(int workers);   // drains, then restarts the pool (0 = inline)
    void stop();
    void poll();     // game loop: apply finished jobs
    void drain();    // wait for everything in flight (before a full save)

    // Resident chunk, or nullptr after starting to load it in the background
    // (nothing is started while too many loads are in flight: see loadsFull()).
    Chunk* acquire(uint8_t dim, int cx, int cz);
    bool loadsFull() const { return loadsInFlight_ >= maxLoads_; }
    void beginBatch(LoadBatch& b) const;
    // Adds (cx, cz), needed dist chunks from player slot `player`, to b unless it is
    // resident; a load already queued is promoted when it is needed more urgently now.
    void want(LoadBatch& b, uint8_t dim, int cx, int cz, int dist, int player = -1);
    // Starts loading the chunks in b as far as load slots allow, then decode/generate jobs
    // with a priority from their distance; one storage round trip for all their headers
    // (and one more for the records of those that were stored). Admission:
    //  1. urgent loads (the ground under a player), closest first: up to the regular
    //     slots plus URGENT_RESERVE of them;
    //  2. NON_URGENT_SHARE more slots belong to the others, handed out round-robin over
    //     the players, so constant urgent demand cannot starve anyone's view;
    //  3. regular slots left over go to the closest remaining chunks.
    // Urgent loads count against their own limit (regular + reserve), the others against
    // the regular slots or the share, so at most 2 * maxLoads_ + URGENT_RESERVE loads are
    // in flight -- loadSlots() keeps that within pending_.
    void requestLoads(const LoadBatch& b);
    // Once per tick: cancels queued loads and sends for chunks no player can see any
    // more, and promotes queued sends whose player came closer.
    void cancelStale();
    // Starts preparing chunk c for player p (the caller marks the view cell in flight).
    bool sendChunk(Player& p, Chunk& c);
    // Resends the light of c to every player that has it. false: too busy, retry later.
    bool resendLight(Chunk& c);
    // Starts saving up to max dirty chunks; returns the number started.
    int saveDirty(int max);
    bool saveChunk(Chunk& c);   // also used by asynchronous eviction
    int savesInFlight() const { return savesInFlight_; }
    bool regionLightFree() const { return regionInFlight_ < MAX_REGION_LIGHT; }
    // The world loaded (cx, cz) synchronously: a background load of it is stale now.
    void onSyncLoad(uint8_t dim, int cx, int cz);

    JobQueue& queue() { return q_; }
    int pinnedChunks() const;   // resident chunks with jobs in flight (never evicted)
    const ChunkJobStats& stats() const { return stats_; }
    void statusLine(char* buf, size_t cap);

private:
    friend class LoadJob;
    friend class SendJob;
    friend class LightJob;
    friend class SaveJob;
    friend class FetchTask;
    friend class WriteTask;
    friend class FallbackTask;
    struct PendingLoad {
        int32_t cx, cz;
        uint8_t dim;
        bool superseded;
        bool forPlayers;
        bool urgentSlot;   // admitted as urgent (counted in urgentInFlight_)
        uint8_t prio;
        LoadJob* job;   // owned by the queue until loadFinished()
    };
    void loadFinished(LoadJob& j);
    void sendFinished(SendJob& j);
    void lightFinished(LightJob& j);
    void saveFinished(SaveJob& j);
    void writeFinished(WriteTask& j);
    void unref(uint8_t dim, int cx, int cz);
    void noteLight(bool exact, uint32_t us);
    int findPending(uint8_t dim, int cx, int cz) const;

    Server* srv_ = nullptr;
    JobQueue q_;
    static const int MAX_PENDING = 32;
    PendingLoad pending_[MAX_PENDING];
    int pendingCount_ = 0;
    int loadsInFlight_ = 0, urgentInFlight_ = 0, maxLoads_ = 2;
    uint32_t rotation_ = 0;     // round-robin start for the non-urgent share
    static int loadSlots(int workers);
    int lightInFlight_ = 0, savesInFlight_ = 0;
    // exact (region) light is several times the work and memory of per-chunk light: one
    // at a time; chunks that had to do without get it later (Server::lightPartial sweep)
    static const int MAX_REGION_LIGHT = 1;
    int regionInFlight_ = 0;
    SendJob* sends_ = nullptr;   // sends in flight (for cancelStale)
    ChunkJobStats stats_;
};

}  // namespace mc
