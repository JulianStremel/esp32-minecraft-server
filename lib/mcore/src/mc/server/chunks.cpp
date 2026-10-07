// Chunk streaming: view tracking, chunk data and light packets.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc/registry.h"
#include "mc/server/chunk_codec.h"
#include "mc/server/server.h"

namespace mc {

// Offsets within the maximum view square, nearest first.
static int8_t s_order[VIEW_SIDE * VIEW_SIDE][2];
static bool s_orderReady = false;

static void buildOrder() {
    int n = 0;
    const int R = MC_MAX_VIEW_DISTANCE;
    for (int dz = -R; dz <= R; dz++)
        for (int dx = -R; dx <= R; dx++) { s_order[n][0] = (int8_t)dx; s_order[n][1] = (int8_t)dz; n++; }
    qsort(s_order, (size_t)n, sizeof(s_order[0]), [](const void* a, const void* b) {
        const int8_t* p = (const int8_t*)a;
        const int8_t* q = (const int8_t*)b;
        int da = p[0] * p[0] + p[1] * p[1], db = q[0] * q[0] + q[1] * q[1];
        return da - db;
    });
    s_orderReady = true;
}

static inline int viewIndex(int dx, int dz) {
    return (dz + MC_MAX_VIEW_DISTANCE) * VIEW_SIDE + (dx + MC_MAX_VIEW_DISTANCE);
}

bool Player::hasChunk(int cx, int cz) const {
    if (state != CS_PLAY || !viewReady) return false;
    int dx = cx - centerCx, dz = cz - centerCz;
    if (abs(dx) > MC_MAX_VIEW_DISTANCE || abs(dz) > MC_MAX_VIEW_DISTANCE) return false;
    return sent[viewIndex(dx, dz)] == VIEW_SENT;
}

uint8_t* Player::viewCell(int cx, int cz) {
    if (!viewReady) return nullptr;
    int dx = cx - centerCx, dz = cz - centerCz;
    if (abs(dx) > viewDist || abs(dz) > viewDist) return nullptr;
    return &sent[viewIndex(dx, dz)];
}

void Player::resetView() {
    memset(sent, 0, sizeof(sent));
    viewReady = false;
}

void Player::updateView(bool force) {
    int cx = (int)floor(e.x) >> 4, cz = (int)floor(e.z) >> 4;
    if (!force && viewReady && cx == centerCx && cz == centerCz) return;
    uint8_t old[VIEW_SIDE * VIEW_SIDE];
    memcpy(old, sent, sizeof(old));
    int ocx = centerCx, ocz = centerCz;
    bool hadView = viewReady;
    memset(sent, 0, sizeof(sent));
    if (hadView) {
        const int R = MC_MAX_VIEW_DISTANCE;
        for (int dz = -R; dz <= R; dz++)
            for (int dx = -R; dx <= R; dx++) {
                uint8_t was = old[viewIndex(dx, dz)];
                if (was == VIEW_NONE) continue;
                int wx = ocx + dx, wz = ocz + dz;
                int ndx = wx - cx, ndz = wz - cz;
                if (abs(ndx) <= viewDist && abs(ndz) <= viewDist) {
                    sent[viewIndex(ndx, ndz)] = was;   // sent, or still being prepared
                } else if (was == VIEW_SENT) {
                    Packet pk(pkt::s2c::UnloadChunk);
                    pk.w.i32(wx);
                    pk.w.i32(wz);
                    conn.send(pk);
                }
                // a chunk still being prepared that left the view is dropped when it is ready
            }
    }
    centerCx = cx;
    centerCz = cz;
    viewReady = true;
    Packet pk(pkt::s2c::UpdateViewPosition);
    pk.w.varint(cx);
    pk.w.varint(cz);
    conn.send(pk);
}

// Nearest chunks first. Missing chunks are loaded (generated / decoded) and packets
// prepared by the worker threads; this only snapshots chunks and starts jobs.
void Player::streamChunks(int budget, LoadBatch& want) {
    if (!viewReady) return;
    if (!s_orderReady) buildOrder();
    ChunkJobs& jobs = srv->chunkJobs;
    for (int i = 0; i < VIEW_SIDE * VIEW_SIDE && budget > 0; i++) {
        int dx = s_order[i][0], dz = s_order[i][1];
        if (abs(dx) > viewDist || abs(dz) > viewDist) continue;
        uint8_t& s = sent[viewIndex(dx, dz)];
        if (s != VIEW_NONE) continue;
        if (pendingSends >= ChunkJobs::MAX_SENDS_PER_PLAYER) return;
        if (conn.pendingOut() > MC_OUT_BUF / 2) {
            conn.flush();
            if (conn.pendingOut() > MC_OUT_BUF / 2) return;  // client is slow, try next tick
        }
        int wx = centerCx + dx, wz = centerCz + dz;
        if (srv->memoryLow() && !srv->world.isResident(wx, wz)) {
            srv->world.evictUnpinned(2);
            if (srv->memoryLow()) return;  // wait until memory is available again
        }
        Chunk* c = srv->world.get(wx, wz);
        if (!c) {
            jobs.want(want, wx, wz);  // loaded in the background; look at the next one meanwhile
            continue;
        }
        if (!jobs.sendChunk(*this, *c)) return;  // out of memory: retry next tick
        s = VIEW_PENDING;
        budget--;
    }
}

bool Server::resendLight(int cx, int cz) {
    Chunk* c = world.get(cx, cz);
    if (!c) return true;
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& p = players[i];
        if (p.inPlay() && p.hasChunk(cx, cz)) return chunkJobs.resendLight(*c);
    }
    return true;  // nobody has it: nothing to send
}

}  // namespace mc
