// Chunk streaming: view tracking, chunk data and light packets.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc/nbt.h"
#include "mc/registry.h"
#include "mc/server/server.h"
#include "mc/world/light.h"

namespace mc {

static ChunkLight s_light;  // reused buffers (single-threaded)

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
    return sent[viewIndex(dx, dz)] != 0;
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
                if (!old[viewIndex(dx, dz)]) continue;
                int wx = ocx + dx, wz = ocz + dz;
                int ndx = wx - cx, ndz = wz - cz;
                if (abs(ndx) <= viewDist && abs(ndz) <= viewDist) {
                    sent[viewIndex(ndx, ndz)] = 1;
                } else {
                    Packet pk(pkt::s2c::UnloadChunk);
                    pk.w.i32(wx);
                    pk.w.i32(wz);
                    conn.send(pk);
                }
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

void Player::streamChunks(int budget) {
    if (!viewReady) return;
    if (!s_orderReady) buildOrder();
    for (int i = 0; i < VIEW_SIDE * VIEW_SIDE && budget > 0; i++) {
        int dx = s_order[i][0], dz = s_order[i][1];
        if (abs(dx) > viewDist || abs(dz) > viewDist) continue;
        uint8_t& s = sent[viewIndex(dx, dz)];
        if (s) continue;
        if (conn.pendingOut() > MC_OUT_BUF / 2) {
            conn.flush();
            if (conn.pendingOut() > MC_OUT_BUF / 2) return;  // client is slow, try next tick
        }
        if (srv->memoryLow() && !srv->world.isResident(centerCx + dx, centerCz + dz)) {
            srv->world.evictUnpinned(2);
            if (srv->memoryLow()) return;  // wait until memory is available again
        }
        sendChunk(centerCx + dx, centerCz + dz);
        s = 1;
        budget--;
    }
}

void Player::sendChunk(int cx, int cz) {
    Chunk* c = srv->world.load(cx, cz);
    srv->sendLight(*this, *c);
    conn.sendStreamed([&](Writer& w) { srv->writeChunkPacket(w, *c); });
}

// ------------------------------------------------------------------ packets
static void writeHeightmap(Writer& w, const Chunk& c) {
    NbtWriter n(w);
    n.beginRoot();
    n.longArrayHeader("MOTION_BLOCKING", 37);
    // 9 bits per entry, 7 entries per long, entries never span longs
    for (int l = 0; l < 37; l++) {
        uint64_t v = 0;
        for (int k = 0; k < 7; k++) {
            int idx = l * 7 + k;
            if (idx >= 256) break;
            v |= (uint64_t)(c.height(idx & 15, idx >> 4) & 0x1FF) << (k * 9);
        }
        w.u64(v);
    }
    n.end();
}

static void writeSignNbt(Writer& w, const TileEntity& t, int x, int z) {
    NbtWriter n(w);
    n.beginRoot();
    n.str("id", "minecraft:sign");
    n.i32("x", x);
    n.i32("y", t.y);
    n.i32("z", z);
    char key[8], json[200];
    for (int i = 0; i < 4; i++) {
        snprintf(key, sizeof(key), "Text%d", i + 1);
        textJson(json, sizeof(json), t.text[i], nullptr);
        n.str(key, json);
    }
    n.str("Color", "black");
    n.end();
}

void Server::writeChunkPacket(Writer& w, Chunk& c) {
    w.varint(pkt::s2c::MapChunk);
    w.i32(c.cx);
    w.i32(c.cz);
    w.boolean(true);
    int mask = 0;
    size_t dataSize = 0;
    for (int s = 0; s < NUM_SECTIONS; s++) {
        const Section* sec = c.section(s);
        if (sec && sec->nonAirCount() > 0) {
            mask |= 1 << s;
            dataSize += sec->wireSize();
        }
    }
    w.varint(mask);
    writeHeightmap(w, c);
    w.varint(1024);
    const uint8_t* cells = c.biomeCells();
    for (int i = 0; i < 1024; i++) w.varint(cells[i & 15]);
    w.varint((int32_t)dataSize);
    for (int s = 0; s < NUM_SECTIONS; s++)
        if (mask & (1 << s)) c.section(s)->writeWire(w);
    int signs = 0;
    for (TileEntity* t = c.tiles(); t; t = t->next)
        if (t->type == TILE_SIGN) signs++;
    w.varint(signs);
    for (TileEntity* t = c.tiles(); t; t = t->next)
        if (t->type == TILE_SIGN) writeSignNbt(w, *t, c.cx * 16 + t->lx, c.cz * 16 + t->lz);
}

// full: also send fully lit data for the sections above the computed range, which
// overwrites stale client data after the terrain got lower.
static void writeLight(Writer& w, const Chunk& c, const ChunkLight& L, bool full) {
    int n = L.sections();
    int skySections = full ? NUM_SECTIONS : n;
    uint32_t skyMask = 0, blockMask = 0, emptyBlock = 0;
    for (int s = 0; s < skySections; s++) skyMask |= 1u << (s + 1);
    for (int s = 0; s < NUM_SECTIONS; s++) {
        if (s < n && !L.blockSectionEmpty(s)) blockMask |= 1u << (s + 1);
        else emptyBlock |= 1u << (s + 1);
    }
    w.varint(pkt::s2c::UpdateLight);
    w.varint(c.cx);
    w.varint(c.cz);
    w.boolean(true);
    w.varint((int32_t)skyMask);
    w.varint((int32_t)blockMask);
    w.varint(0);
    w.varint((int32_t)emptyBlock);
    static uint8_t full15[2048];
    if (full15[0] != 0xFF) memset(full15, 0xFF, sizeof(full15));
    for (int s = 0; s < skySections; s++) {
        w.varint(2048);
        w.bytes(s < n ? L.sky(s) : full15, 2048);
    }
    for (int s = 0; s < n; s++) {
        if (!(blockMask & (1u << (s + 1)))) continue;
        w.varint(2048);
        w.bytes(L.block(s), 2048);
    }
}

void Server::sendLight(Player& p, Chunk& c) {
    if (!s_light.compute(c, &world)) {
        MC_LOGW("out of memory computing light for %d,%d", c.cx, c.cz);
        return;
    }
    p.conn.sendStreamed([&](Writer& w) { writeLight(w, c, s_light, false); });
}

void Server::resendLight(int cx, int cz) {
    Chunk* c = world.get(cx, cz);
    if (!c) return;
    bool computed = false;
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& p = players[i];
        if (!p.inPlay() || !p.hasChunk(cx, cz)) continue;
        if (!computed) {
            if (!s_light.compute(*c, &world)) return;
            computed = true;
        }
        p.conn.sendStreamed([&](Writer& w) { writeLight(w, *c, s_light, true); });
    }
}

}  // namespace mc
