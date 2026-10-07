#include "mc/server/server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc/registry.h"

namespace mc {

Server::Server() {}

Server::~Server() { delete listener_; }

// ------------------------------------------------------------------ lifecycle
bool Server::begin(const ServerConfig& config, Storage* st) {
    cfg = config;
    if (cfg.maxPlayers > MC_MAX_PLAYERS) cfg.maxPlayers = MC_MAX_PLAYERS;
    if (cfg.viewDistance > MC_MAX_VIEW_DISTANCE) cfg.viewDistance = MC_MAX_VIEW_DISTANCE;
    if (cfg.viewDistance < 2) cfg.viewDistance = 2;
    if (cfg.simulationDistance < 1) cfg.simulationDistance = 1;
    storage = st;

    bool haveWorld = storage && storage->loadMeta(meta);
    if (!haveWorld) {
        meta = WorldMeta();
        meta.seed = cfg.seed ? cfg.seed : ((uint64_t)plat::random32() << 32 | plat::random32());
        meta.worldType = cfg.worldType;
        meta.radius = cfg.worldRadiusChunks;
    }
    if (storage && storage->worldRadius() > 0) meta.radius = storage->worldRadius();
    gen.init(meta.seed, (WorldType)meta.worldType);
    if (!haveWorld) {
        int sx, sy, sz;
        gen.findSpawn(sx, sy, sz);
        meta.spawnX = sx; meta.spawnY = sy; meta.spawnZ = sz;
        if (storage) storage->saveMeta(meta);
    }
    world.init(&gen, storage, cfg.chunkCacheSize, meta.radius);
    world.setListener(this);
    world.setPinner(this);
    for (int i = 0; i < MC_MAX_PLAYERS; i++) players[i].reset(this, i);

    // make sure spawn is on the surface of the (possibly modified) world
    Chunk* sc = world.load(meta.spawnX >> 4, meta.spawnZ >> 4);
    int h = sc->height(meta.spawnX & 15, meta.spawnZ & 15);
    if (h > 0 && h + 1 > meta.spawnY) meta.spawnY = h;

    listener_ = plat::listen(cfg.port);
    if (!listener_) {
        MC_LOGE("cannot listen on port %u", cfg.port);
        return false;
    }
    MC_LOGI("world: %s seed=%lld type=%d radius=%d chunks, spawn %d %d %d", haveWorld ? "loaded" : "new",
            (long long)meta.seed, meta.worldType, (int)meta.radius, (int)meta.spawnX, (int)meta.spawnY, (int)meta.spawnZ);
    MC_LOGI("listening on port %u (max %d players, view distance %d)", cfg.port, cfg.maxPlayers, cfg.viewDistance);
    running_ = true;
    nextTickMs_ = plat::millis();
    lastTpsMs_ = nextTickMs_;
    lastSaveMs_ = nextTickMs_;
    return true;
}

void Server::loop() {
    if (!running_) return;
    acceptConnections();
    pollPlayers();
    uint32_t now = plat::millis();
    int caught = 0;
    while ((int32_t)(now - nextTickMs_) >= 0 && caught < 5) {
        uint32_t t0 = plat::millis();
        tick();
        uint32_t dt = plat::millis() - t0;
        msptAvg = msptAvg * 0.95f + dt * 0.05f;
        nextTickMs_ += 50;
        caught++;
        now = plat::millis();
    }
    if ((int32_t)(now - nextTickMs_) > 1000) nextTickMs_ = now;  // way behind: skip ticks
    for (int i = 0; i < MC_MAX_PLAYERS; i++)
        if (players[i].state != CS_FREE) players[i].conn.flush();
}

void Server::shutdown(const char* reason) {
    for (int i = 0; i < MC_MAX_PLAYERS; i++)
        if (players[i].state != CS_FREE && players[i].conn.open()) players[i].kick(reason);
    saveAll(true);
    running_ = false;
}

// ------------------------------------------------------------------ network
void Server::acceptConnections() {
    for (int n = 0; n < 4; n++) {
        Conn* c = listener_->accept();
        if (!c) return;
        int slot = -1;
        for (int i = 0; i < MC_MAX_PLAYERS; i++)
            if (players[i].state == CS_FREE) { slot = i; break; }
        if (slot < 0) {
            MC_LOGW("connection from %s refused: no free slot", c->peer());
            c->close();
            delete c;
            continue;
        }
        Player& p = players[slot];
        p.reset(this, slot);
        p.conn.attach(c);
        p.state = CS_HANDSHAKE;
        p.connectedAt = plat::millis();
        MC_LOGD("connection from %s (slot %d)", c->peer(), slot);
    }
}

void Server::pollPlayers() {
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& p = players[i];
        if (p.state == CS_FREE) continue;
        bool alive = p.conn.poll();
        int id;
        Reader r(nullptr, 0);
        int handled = 0;
        while (alive && handled < 64 && p.state != CS_FREE && p.conn.nextPacket(id, r)) {
            p.onPacket(id, r);
            handled++;
        }
        if (p.state == CS_FREE) continue;
        uint32_t now = plat::millis();
        bool timeout = p.state != CS_PLAY ? now - p.connectedAt > 10000
                                          : now - p.conn.lastReceiveMs() > (uint32_t)cfg.keepAliveTimeoutMs;
        if (!p.conn.open() || timeout) {
            if (p.state == CS_PLAY) {
                MC_LOGI("%s left the game%s", p.name, timeout ? " (timed out)" : "");
                p.kick(timeout ? "Timed out" : nullptr);
            } else {
                p.conn.close();
                p.state = CS_FREE;
            }
        }
    }
}

// ------------------------------------------------------------------ messaging
void Server::broadcast(const Packet& p, const Player* except) {
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& pl = players[i];
        if (&pl != except && pl.inPlay()) pl.conn.send(p);
    }
}

void Server::broadcastNear(const Packet& p, int cx, int cz, const Player* except) {
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& pl = players[i];
        if (&pl != except && pl.inPlay() && pl.hasChunk(cx, cz)) pl.conn.send(p);
    }
}

void Server::broadcastChat(const char* json, uint8_t position) {
    for (int i = 0; i < MC_MAX_PLAYERS; i++)
        if (players[i].inPlay()) players[i].sendChat(json, position);
}

void Server::broadcastSystem(const char* text, const char* color) {
    char json[512];
    textJson(json, sizeof(json), text, color);
    broadcastChat(json, 1);
    MC_LOGI("[chat] %s", text);
}

Player* Server::findPlayer(const char* name) {
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& p = players[i];
        if (p.state == CS_PLAY && !strcasecmp(p.name, name)) return &p;
    }
    return nullptr;
}

Player* Server::playerByEntity(int32_t id) {
    for (int i = 0; i < MC_MAX_PLAYERS; i++)
        if (players[i].state == CS_PLAY && players[i].e.id == id) return &players[i];
    return nullptr;
}

int Server::onlineCount() const {
    int n = 0;
    for (int i = 0; i < MC_MAX_PLAYERS; i++)
        if (players[i].state == CS_PLAY) n++;
    return n;
}

static bool nameInList(const char* list, const char* name) {
    if (!list || !*list) return false;
    size_t nl = strlen(name);
    const char* p = list;
    while (*p) {
        while (*p == ',' || *p == ' ') p++;
        const char* s = p;
        while (*p && *p != ',') p++;
        const char* e = p;
        while (e > s && e[-1] == ' ') e--;
        if ((size_t)(e - s) == nl && !strncasecmp(s, name, nl)) return true;
    }
    return false;
}

bool Server::isOp(const char* name) const { return nameInList(cfg.ops, name); }
bool Server::isWhitelisted(const char* name) const {
    return !cfg.whitelist || !*cfg.whitelist || nameInList(cfg.whitelist, name);
}

void Server::sendPlayerInfoAdd(Player* to, const Player& p) {
    Packet pk(pkt::s2c::PlayerInfo);
    pk.w.varint(0);
    pk.w.varint(1);
    pk.w.uuid(p.uuid);
    pk.w.string(p.name);
    pk.w.varint(0);  // no properties (offline mode: default skin)
    pk.w.varint(p.gamemode);
    pk.w.varint(p.ping);
    pk.w.boolean(false);
    if (to) to->conn.send(pk);
    else broadcast(pk);
}

void Server::sendPlayerInfoRemove(const Player& p) {
    Packet pk(pkt::s2c::PlayerInfo);
    pk.w.varint(4);
    pk.w.varint(1);
    pk.w.uuid(p.uuid);
    broadcast(pk, &p);
}

void Server::statusLine(char* buf, size_t cap) {
    char st[96] = "no storage (world is not persisted)";
    if (storage) storage->statusLine(st, sizeof(st));
    snprintf(buf, cap, "TPS %.1f, %.1f ms/tick, heap %u KB, %d chunks (%u KB), %d entities | %s", tps, msptAvg,
             (unsigned)(plat::freeHeap() / 1024), world.residentCount(), (unsigned)(world.residentBytes() / 1024),
             mobCount(), st);
}

void Server::sendTabHeader(Player& p) {
    char line[200], hdr[256], ftr[320];
    snprintf(line, sizeof(line), "%s", cfg.motd);
    textJson(hdr, sizeof(hdr), line, "gold");
    snprintf(line, sizeof(line), "TPS %.1f  |  %d/%d online  |  heap %u KB", tps, onlineCount(), cfg.maxPlayers,
             (unsigned)(plat::freeHeap() / 1024));
    textJson(ftr, sizeof(ftr), line, "gray");
    Packet pk(pkt::s2c::PlayerlistHeader);
    pk.w.string(hdr);
    pk.w.string(ftr);
    p.conn.send(pk);
}

// ------------------------------------------------------------------ world events
void Server::onBlockChanged(int x, int y, int z, uint16_t oldState, uint16_t newState) {
    Packet pk(pkt::s2c::BlockChange);
    pk.w.u64(packPos(x, y, z));
    pk.w.varint(newState);
    broadcastNear(pk, x >> 4, z >> 4);
    // light changes if the light-relevant properties differ
    const BlockDef& a = blockOf(oldState);
    const BlockDef& b = blockOf(newState);
    if (a.filterLight != b.filterLight || a.emitLight != b.emitLight) {
        int cx = x >> 4, cz = z >> 4;
        for (int i = 0; i < lightQLen_; i++)
            if (lightQ_[i][0] == cx && lightQ_[i][1] == cz) return;
        if (lightQLen_ < 32) {
            lightQ_[lightQLen_][0] = cx;
            lightQ_[lightQLen_][1] = cz;
            lightQLen_++;
        }
    }
}

void Server::onChunkEvicted(Chunk& c) {}

bool Server::isChunkPinned(int cx, int cz) {
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& p = players[i];
        if (p.state != CS_PLAY) continue;
        // only the simulation area must stay resident; farther chunks were sent to the
        // client already and are reloaded / regenerated when needed again
        int d = cfg.simulationDistance < p.viewDist ? cfg.simulationDistance : p.viewDist;
        if (cx >= p.centerCx - d && cx <= p.centerCx + d && cz >= p.centerCz - d && cz <= p.centerCz + d) return true;
    }
    return false;
}

void Server::flushLightQueue() {
    int n = lightQLen_ < 4 ? lightQLen_ : 4;
    for (int i = 0; i < n; i++) resendLight(lightQ_[i][0], lightQ_[i][1]);
    memmove(lightQ_, lightQ_ + n, (size_t)(lightQLen_ - n) * sizeof(lightQ_[0]));
    lightQLen_ -= n;
}

// ------------------------------------------------------------------ ticking
void Server::tick() {
    ticks++;
    tpsTicks_++;
    uint32_t now = plat::millis();
    if (now - lastTpsMs_ >= 2000) {
        tps = tpsTicks_ * 1000.0f / (now - lastTpsMs_);
        if (tps > 20) tps = 20;
        tpsTicks_ = 0;
        lastTpsMs_ = now;
    }
    tickTime();
    tickWeather();
    tickPlayers();
    tickBlocks();
    tickFurnaces();
    tickEntities();
    tickMobSpawning();
    trackEntities();
    flushLightQueue();
    autosave();
    world.maintain();
    if (memoryLow()) world.evictUnpinned(4);
}

bool Server::memoryLow() const {
    return cfg.minFreeHeapKb > 0 && plat::freeHeap() < (size_t)cfg.minFreeHeapKb * 1024;
}

void Server::tickTime() {
    meta.worldAge++;
    meta.timeOfDay++;
    if (ticks % 20 == 0)
        for (int i = 0; i < MC_MAX_PLAYERS; i++)
            if (players[i].inPlay()) players[i].sendTime();
}

void Server::tickWeather() {
    if (--meta.weatherTimer > 0) return;
    meta.raining = !meta.raining;
    meta.weatherTimer = meta.raining ? 6000 + (int)(plat::random32() % 6000) : 12000 + (int)(plat::random32() % 84000);
    Packet pk(pkt::s2c::GameStateChange);
    pk.w.u8(meta.raining ? 1 : 2);
    pk.w.f32(0);
    broadcast(pk);
}

void Server::tickPlayers() {
    uint32_t now = plat::millis();
    uint32_t tickStart = now;
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& p = players[i];
        if (!p.inPlay()) continue;
        // keep alive every 10 s
        if (!p.kaPending && now - p.kaSentMs > 10000) {
            p.kaId = (int64_t)now;
            p.kaPending = true;
            p.kaSentMs = now;
            Packet pk(pkt::s2c::KeepAlive);
            pk.w.i64(p.kaId);
            p.conn.send(pk);
        }
        if (now - p.lastHeaderMs > 3000) {
            p.lastHeaderMs = now;
            sendTabHeader(p);
            Packet pk(pkt::s2c::PlayerInfo);  // latency update for everyone
            pk.w.varint(2);
            pk.w.varint(1);
            pk.w.uuid(p.uuid);
            pk.w.varint(p.ping);
            broadcast(pk);
        }
        tickSurvival(p);
    }
    // stream chunks round-robin within the tick budget
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& p = players[(i + ticks) % MC_MAX_PLAYERS];
        if (!p.inPlay()) continue;
        if ((int)(plat::millis() - tickStart) > cfg.tickBudgetMs) break;
        p.streamChunks(cfg.chunksPerTick);
    }
}

void Server::savePlayer(Player& p) {
    if (!storage || p.state != CS_PLAY) return;
    PlayerData d;
    p.toData(d);
    if (!storage->savePlayer(d)) MC_LOGW("could not save player %s", p.name);
}

void Server::autosave() {
    uint32_t now = plat::millis();
    if (!saving_ && now - lastSaveMs_ > (uint32_t)cfg.autosaveSeconds * 1000) {
        saving_ = true;
        lastSaveMs_ = now;
        for (int i = 0; i < MC_MAX_PLAYERS; i++) savePlayer(players[i]);
        if (storage) storage->saveMeta(meta);
    }
    if (saving_) {
        // spread chunk writes over several ticks
        if (world.saveDirty(2) == 0) {
            saving_ = false;
            if (storage) storage->flush();
        }
    }
}

void Server::saveAll(bool flushStorage) {
    for (int i = 0; i < MC_MAX_PLAYERS; i++) savePlayer(players[i]);
    int n = world.saveAll();
    if (storage) {
        storage->saveMeta(meta);
        if (flushStorage) storage->flush();
    }
    MC_LOGI("saved world (%d chunks)", n);
}

// ------------------------------------------------------------------ text helpers
size_t jsonEscape(const char* in, char* out, size_t cap) {
    size_t n = 0;
    for (const unsigned char* p = (const unsigned char*)in; *p && n + 7 < cap; p++) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = (char)c; }
        else if (c == '\n') { out[n++] = '\\'; out[n++] = 'n'; }
        else if (c < 0x20) { n += (size_t)snprintf(out + n, cap - n, "\\u%04x", c); }
        else out[n++] = (char)c;
    }
    out[n] = 0;
    return n;
}

void textJson(char* out, size_t cap, const char* text, const char* color) {
    char esc[400];
    jsonEscape(text, esc, sizeof(esc));
    if (color) snprintf(out, cap, "{\"text\":\"%s\",\"color\":\"%s\"}", esc, color);
    else snprintf(out, cap, "{\"text\":\"%s\"}", esc);
}

}  // namespace mc
