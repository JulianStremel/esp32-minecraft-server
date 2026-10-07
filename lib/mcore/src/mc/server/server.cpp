#include "mc/server/server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc/registry.h"

namespace mc {

Server::Server() {}

Server::~Server() {
    chunkJobs.stop();  // finishes in-flight jobs while the world and players still exist
    delete listener_;
}

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
    if (!timers.init(MC_SCHED_TICKS)) {
        MC_LOGE("not enough memory for the timer wheel");
        return false;
    }
    timers.reset(worldTick() + 1);   // the next tick() runs world age + 1
    world.init(&gen, storage, cfg.chunkCacheSize, meta.radius);
    world.setListener(this);
    world.setPinner(this);
    chunkJobs.init(this, cfg.workerThreads);
    chunkJobs.queue().setUrgentHook(plat::wake);   // e.g. the ground under a player: apply it now
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
    clockTicks_.restart();
    lastTpsMs_ = plat::millis();
    lastSaveMs_ = lastTpsMs_;
    return true;
}

void Server::setTickSource(TickSource* src) { tickSource_ = src ? src : &clockTicks_; }

uint32_t Server::waitTimeoutMs() {
    if (!running_) return 0;
    if (pacer_.backlog() > 0) return 0;   // late ticks still to catch up
    return tickSource_->msUntilNext();
}

void Server::loop() {
    if (!running_) return;
    uint32_t loopStart = plat::millis();
    wakeWin_++;
    lagCur_.clear();
    Connection::takeBlockedMs();
    {
        uint32_t f, l;
        const char* k;
        chunkJobs.queue().takeLoopCost(f, k, l);
    }
    chunkJobs.poll();
    uint32_t t1 = plat::millis();
    lagCur_.ms[LagProfile::P_JOBS] = (uint16_t)(t1 - loopStart);
    acceptConnections();
    pollPlayers();
    uint32_t now = plat::millis();
    lagCur_.ms[LagProfile::P_INPUT] = (uint16_t)(now - t1);
    uint32_t due = pacer_.add(tickSource_->take());
    if (pacer_.skippedNow() && (now - behindLogMs_ > 15000 || behindLogMs_ == 0)) {
        MC_LOGW("Can't keep up! Is the server overloaded? Skipping %u ticks (%u ms)", (unsigned)pacer_.skippedNow(),
                (unsigned)(pacer_.skippedNow() * TICK_MS));
        behindLogMs_ = now ? now : 1;
    }
    uint32_t caught = 0;
    for (; caught < due; caught++) {
        pacer_.started();
        uint32_t t0 = plat::millis();
        tick();
        uint32_t dt = plat::millis() - t0;
        msptAvg = msptAvg * 0.95f + dt * 0.05f;
        if (dt > tickMaxWin_) tickMaxWin_ = dt;
    }
    uint32_t t3 = plat::millis();
    if (caught) chunkJobs.poll();  // results of jobs the tick started
    for (int i = 0; i < MC_MAX_PLAYERS; i++)
        if (players[i].state != CS_FREE) players[i].conn.flush();
    uint32_t end = plat::millis();
    lagCur_.ms[LagProfile::P_OUTPUT] = (uint16_t)(end - t3);
    uint32_t stall = end - loopStart;
    if (stall > stallMaxWin_) {
        stallMaxWin_ = stall;
        lagCur_.total = (uint16_t)stall;
        lagCur_.socketWait = (uint16_t)Connection::takeBlockedMs();
        uint32_t fUs, lUs;
        chunkJobs.queue().takeLoopCost(fUs, lagCur_.finishKind, lUs);
        lagCur_.finishMs = (uint16_t)(fUs / 1000);
        lagCur_.lockMs = (uint16_t)(lUs / 1000);
        lagWin_ = lagCur_;
    }
}

const char* LagProfile::name(int part) {
    static const char* names[PARTS] = {"jobs",     "input", "players", "stream", "blocks", "entities", "spawn",
                                       "tracking", "light", "save",    "evict",  "other",  "output",   "snapshots",
                                       "storage reads"};
    return names[part];
}

void LagProfile::format(char* buf, size_t cap) const {
    int n = snprintf(buf, cap, "%u ms:", (unsigned)total);
    for (int i = 0; i < P_SNAPSHOT && n > 0 && (size_t)n < cap; i++)
        if (ms[i]) n += snprintf(buf + n, cap - n, " %s %u", name(i), (unsigned)ms[i]);
    // sub-parts of "stream"
    if ((ms[P_SNAPSHOT] || ms[P_FETCH]) && n > 0 && (size_t)n < cap)
        n += snprintf(buf + n, cap - n, " (stream: snapshots %u, storage reads %u)", (unsigned)ms[P_SNAPSHOT],
                      (unsigned)ms[P_FETCH]);
    if (n > 0 && (size_t)n < cap)
        snprintf(buf + n, cap - n, " (waiting for sockets %u, slowest job finish %u [%s], queue lock %u)",
                 (unsigned)socketWait, (unsigned)finishMs, finishKind, (unsigned)lockMs);
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
    char jobs[224];
    chunkJobs.statusLine(jobs, sizeof(jobs));
    snprintf(buf, cap,
             "TPS %.1f, %.1f ms/tick (max %u), max loop stall %u ms, %.0f wakeups/s, %u overruns (%u ticks late, "
             "%u skipped), heap %u KB, %d chunks (%u KB), %d entities | %s | %s",
             tps, msptAvg, (unsigned)tickMaxMs, (unsigned)stallMaxMs, wakeupsPerS, (unsigned)overruns,
             (unsigned)lateTicks, (unsigned)skippedTicks, (unsigned)(plat::freeHeap() / 1024), world.residentCount(),
             (unsigned)(world.residentBytes() / 1024), mobCount(), jobs, st);
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

void Server::onChunkEvicted(Chunk& c) {
    // its block ticks and furnaces were stored with it (if it was saved); unloaded chunks
    // do not tick
    int x0 = c.cx * 16, z0 = c.cz * 16;
    timers.removeIf([x0, z0](const TimerEvent& ev) {
        return (ev.key.kind == TK_BLOCK || ev.key.kind == TK_FURNACE) && ev.key.x >= x0 && ev.key.x < x0 + 16 &&
               ev.key.z >= z0 && ev.key.z < z0 + 16;
    });
}

void Server::onChunkReady(Chunk& c) {
    // scheduled ticks stored with the chunk continue where they were
    uint32_t now = worldTick();
    for (int i = 0; i < c.tickCount; i++) {
        const ChunkTick& k = c.ticks[i];
        int32_t d = k.delay > 0 ? k.delay : 1;
        timers.schedule(TimerKey::block(c.cx * 16 + k.lx, k.y, c.cz * 16 + k.lz, k.block), now + (uint32_t)d, k.prio);
    }
    c.clearTicks();
    // furnaces resume from their stored state
    for (TileEntity* t = c.tiles(); t; t = t->next) {
        if (t->type != TILE_FURNACE) continue;
        t->updated = now;
        if (t->burnTime > 0 || !t->items[0].empty())
            timers.schedule(TimerKey::furnace(c.cx * 16 + t->lx, t->y, c.cz * 16 + t->lz), now + 1);
    }
}

void Server::prepareChunkSave(Chunk& live) {
    for (TileEntity* t = live.tiles(); t; t = t->next)
        if (t->type == TILE_FURNACE) updateFurnace(live.cx * 16 + t->lx, t->y, live.cz * 16 + t->lz, false);
}

void Server::attachTicks(Chunk& target) {
    if (!timers.size()) {
        target.clearTicks();
        return;
    }
    int x0 = target.cx * 16, z0 = target.cz * 16;
    int n = 0;
    timers.forEach([&](const TimerEvent& ev) {
        if (ev.key.kind == TK_BLOCK && ev.key.x >= x0 && ev.key.x < x0 + 16 && ev.key.z >= z0 && ev.key.z < z0 + 16) n++;
    });
    if (!n) {
        target.clearTicks();
        return;
    }
    ChunkTick* list = (ChunkTick*)plat::bigAlloc(sizeof(ChunkTick) * (size_t)n);
    if (!list) return;
    int k = 0;
    uint32_t now = worldTick();
    timers.forEach([&](const TimerEvent& ev) {
        if (ev.key.kind != TK_BLOCK || ev.key.x < x0 || ev.key.x >= x0 + 16 || ev.key.z < z0 || ev.key.z >= z0 + 16 || k >= n)
            return;
        ChunkTick& t = list[k++];
        t.lx = (uint8_t)(ev.key.x - x0);
        t.lz = (uint8_t)(ev.key.z - z0);
        t.y = (uint8_t)ev.key.y;
        t.block = ev.key.data;
        t.prio = ev.prio;
        int32_t d = (int32_t)(ev.due - now);
        t.delay = d > 0 ? d : 1;
    });
    target.setTicks(list, k);
    plat::bigFree(list);
}

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
    int n = 0;
    while (n < lightQLen_ && n < 4 && resendLight(lightQ_[n][0], lightQ_[n][1])) n++;
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
        tickMaxMs = tickMaxWin_;
        stallMaxMs = stallMaxWin_;
        wakeupsPerS = wakeWin_ * 1000.0f / (now - lastTpsMs_);
        TickPacer::Counts pc = pacer_.takeWindow();
        overruns = pc.overruns;
        lateTicks = pc.late;
        skippedTicks = pc.skipped;
        waits = plat::takeWaitStats();
        wakeWin_ = 0;
        lag = lagWin_;
        lagWin_.clear();
        tickMaxWin_ = stallMaxWin_ = 0;
        tpsTicks_ = 0;
        lastTpsMs_ = now;
    }
    uint32_t t = plat::millis();
    auto part = [&](int p) {
        uint32_t n = plat::millis();
        lagCur_.ms[p] = (uint16_t)(lagCur_.ms[p] + (n - t));
        t = n;
    };
    tickTime();
    tickWeather();
    part(LagProfile::P_OTHER);
    tickPlayers();   // accounts PLAYERS and STREAM itself
    t = plat::millis();
    tickBlocks();
    tickFurnaceViewers();
    part(LagProfile::P_BLOCKS);
    tickEntities();
    part(LagProfile::P_ENTITIES);
    tickMobSpawning();
    part(LagProfile::P_SPAWN);
    trackEntities();
    part(LagProfile::P_TRACK);
    flushLightQueue();
    part(LagProfile::P_LIGHT);
    autosave();
    part(LagProfile::P_SAVE);
    world.maintain();
    if (memoryLow()) world.evictUnpinned(4);
    part(LagProfile::P_EVICT);
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
        if (p.chatSpam > 0) p.chatSpam--;
        tickSurvival(p);
    }
    uint32_t streamStart = plat::millis();
    lagCur_.ms[LagProfile::P_PLAYERS] = (uint16_t)(lagCur_.ms[LagProfile::P_PLAYERS] + (streamStart - tickStart));
    // stream chunks round-robin within the tick budget; the chunks they lack are
    // collected and loaded together (one storage round trip)
    chunkJobs.cancelStale();  // drop queued work for chunks nobody sees, move up what got closer
    LoadBatch want;
    chunkJobs.beginBatch(want);
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& p = players[(i + ticks) % MC_MAX_PLAYERS];
        if (!p.inPlay()) continue;
        if ((int)(plat::millis() - tickStart) > cfg.tickBudgetMs) break;
        p.streamChunks(cfg.chunksPerTick, want);
    }
    chunkJobs.requestLoads(want);
    lagCur_.ms[LagProfile::P_STREAM] = (uint16_t)(lagCur_.ms[LagProfile::P_STREAM] + (plat::millis() - streamStart));
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
        // chunks are encoded on the workers and written as they finish
        if (chunkJobs.saveDirty(4) == 0 && chunkJobs.savesInFlight() == 0) {
            saving_ = false;
            if (storage) storage->flushLater();  // durable soon; shutdown waits for a real flush
        }
    }
}

void Server::saveAll(bool flushStorage) {
    chunkJobs.drain();  // in-flight saves first, then everything else synchronously
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
