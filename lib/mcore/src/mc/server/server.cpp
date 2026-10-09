#include "mc/server/server.h"
#include "mc/text.h"
#include "mc/server/piston.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc/registry.h"
#if MC_DASHBOARD
#include "mc/server/dashboard.h"
#endif

namespace mc {

Server::Server() {}

Server::~Server() {
    chunkJobs.stop();  // finishes in-flight jobs while the world and players still exist
    storageIo.stop();
#if MC_DASHBOARD
    delete dashboard;
#endif
    plat::bigFree(timerOut_);
    plat::bigFree(tileTickList_);
    delete listener_;
}

// ------------------------------------------------------------------ lifecycle
bool Server::begin(const ServerConfig& config, Storage* st) {
    cfg = config;
    if (cfg.maxPlayers > MC_MAX_PLAYERS) cfg.maxPlayers = MC_MAX_PLAYERS;
    if (cfg.viewDistance > MC_MAX_VIEW_DISTANCE) cfg.viewDistance = MC_MAX_VIEW_DISTANCE;
    if (cfg.viewDistance < 2) cfg.viewDistance = 2;
    if (cfg.simulationDistance < 1) cfg.simulationDistance = 1;
    if (st && !storageIo.start(st)) {
        MC_LOGE("could not start storage I/O thread");
        return false;
    }
    storage = st ? &storageIo : nullptr;

    bool haveWorld = storage && storage->loadMeta(meta);
    if (!haveWorld) {
        meta.reset();
        meta.seed = cfg.seed ? cfg.seed : ((uint64_t)plat::random32() << 32 | plat::random32());
        meta.worldType = cfg.worldType;
        meta.generatorVersion = cfg.generatorVersion;
        meta.radius = cfg.worldRadiusChunks;
    } else if (meta.generatorVersion == 0) {
        meta.generatorVersion = 1;   // stored before versions existed
    }
    if (haveWorld && meta.extraLen && !wstate.decode(meta.extra, meta.extraLen))
        MC_LOGW("world state (portals, dragon fight) unreadable: starting without it");
    if (haveWorld)
        MC_LOGI("world state: %d known portals, dragon fight %d", wstate.nPortals, (int)wstate.dragon.state);
    if (storage && storage->worldRadius() > 0) meta.radius = storage->worldRadius();
    // never generate a world with another version than its own: its unmodified chunks
    // would change
    if (!gen.init(meta.seed, (WorldType)meta.worldType, meta.generatorVersion)) {
        if (haveWorld)
            MC_LOGE("the world needs terrain generator v%d, this build has v1 to v%d: update it", meta.generatorVersion,
                    GENERATOR_LATEST);
        else
            MC_LOGE("unknown terrain generator version %d (1 to %d)", meta.generatorVersion, GENERATOR_LATEST);
        return false;
    }
    if (!haveWorld || meta.spawnY == SPAWN_PENDING) {
        if (haveWorld) MC_LOGI("a new world after a reset: seed %lld", (long long)meta.seed);
        int sx, sy, sz;
        gen.findSpawn(sx, sy, sz);
        meta.spawnX = sx; meta.spawnY = sy; meta.spawnZ = sz;
        packWorldState();
        if (storage) storage->saveMeta(meta);
    }
    if (!timers.init(MC_SCHED_TICKS)) {
        MC_LOGE("not enough memory for the timer wheel");
        return false;
    }
    timerOut_ = (TimerEvent*)plat::bigAlloc(sizeof(TimerEvent) * MC_SCHED_TICKS);
    if (!timerOut_) { MC_LOGE("not enough memory for scheduled tick batch"); return false; }
    timers.reset(worldTick() + 1);   // the next tick() runs world age + 1
    netherGen.init(meta.seed, (WorldType)meta.worldType, meta.generatorVersion, DIM_NETHER);
    endGen.init(meta.seed, (WorldType)meta.worldType, meta.generatorVersion, DIM_END);
    Generator* const gens[NUM_DIMS] = {&gen, &netherGen, &endGen};
    world.init(gens, storage, cfg.chunkCacheSize, meta.radius);
    world.setListener(this);
    world.setPinner(this);
    chunkJobs.init(this, cfg.workerThreads);
    chunkJobs.queue().setUrgentHook(plat::wake);   // e.g. the ground under a player: apply it now
    for (int i = 0; i < MC_MAX_PLAYERS; i++) players[i].reset(this, i);

    // make sure spawn is on the surface of the (possibly modified) world
    Chunk* sc = world.load(DIM_OVERWORLD, meta.spawnX >> 4, meta.spawnZ >> 4);
    int h = sc->height(meta.spawnX & 15, meta.spawnZ & 15);
    if (h > 0 && h + 1 > meta.spawnY) meta.spawnY = h;

    listener_ = plat::listen(cfg.port);
    if (!listener_) {
        MC_LOGE("cannot listen on port %u", cfg.port);
        return false;
    }
    MC_LOGI("world: %s seed=%lld type=%d generator v%d radius=%d chunks, spawn %d %d %d", haveWorld ? "loaded" : "new",
            (long long)meta.seed, meta.worldType, gen.version(), (int)meta.radius, (int)meta.spawnX, (int)meta.spawnY,
            (int)meta.spawnZ);
    MC_LOGI("listening on port %u (max %d players, view distance %d)", cfg.port, cfg.maxPlayers, cfg.viewDistance);
#if MC_DASHBOARD
    if (cfg.dashboardPort) {   // optional: the game runs without it
        dashboard = new Dashboard(*this);
        if (!dashboard->begin(cfg.dashboardPort)) {
            delete dashboard;
            dashboard = nullptr;
        }
    }
#endif
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
#if MC_DASHBOARD
    if (dashboard) dashboard->poll();   // counted as input in /lag
#endif
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
#if MC_DASHBOARD
    if (dashboard) dashboard->afterLoop(waitTimeoutMs());   // a snapshot for open pages
#endif
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
            InDim in(*this, p.e.dim);   // a dimension change takes effect with the next packet
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

void Server::broadcastNearIn(uint8_t dim, const Packet& p, int cx, int cz, const Player* except) {
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& pl = players[i];
        if (&pl != except && pl.inPlay() && pl.hasChunk(dim, cx, cz)) pl.conn.send(p);
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
    pk.w.u8(0x01 | 0x04 | 0x08 | 0x10);   // add, game mode, listed, latency
    pk.w.varint(1);
    pk.w.uuid(p.uuid);
    pk.w.string(p.name);
    pk.w.varint(0);  // no properties (offline mode: default skin)
    pk.w.varint(p.gamemode);
    pk.w.boolean(true);
    pk.w.varint(p.ping);
    if (to) to->conn.send(pk);
    else broadcast(pk);
}

void Server::sendPlayerInfoRemove(const Player& p) {
    Packet pk(pkt::s2c::PlayerRemove);
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
             "%u skipped), heap %u KB, internal %u KB (min %u KB), %d chunks (%u KB), %d entities | %s | %s",
             tps, msptAvg, (unsigned)tickMaxMs, (unsigned)stallMaxMs, wakeupsPerS, (unsigned)overruns,
             (unsigned)lateTicks, (unsigned)skippedTicks, (unsigned)(plat::freeHeap() / 1024),
             (unsigned)(plat::freeInternalHeap() / 1024), (unsigned)(plat::minFreeInternalHeap() / 1024),
             world.residentCount(),
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
    writeTextNbt(pk.w, hdr);
    writeTextNbt(pk.w, ftr);
    p.conn.send(pk);
}

// The performance banner is two boss bars shared by all players, stacked so that each
// line fits the screen (a boss bar title neither wraps nor takes a smaller font):
//  1. TPS and tick times; the bar shows TPS out of 20 and turns yellow and red as the
//     server falls behind;
//  2. memory, chunks, mobs and players; the bar shows the free heap against the most
//     seen since the banner was turned on.
static const uint8_t PERF_BAR_UUID[Server::PERF_BARS][16] = {
    {0x6d, 0x63, 0x2d, 0x70, 0x65, 0x72, 0x66, 0x62, 0x61, 0x72, 0x40, 0x00, 0x80, 0x00, 0x00, 0x01},
    {0x6d, 0x63, 0x2d, 0x70, 0x65, 0x72, 0x66, 0x62, 0x61, 0x72, 0x40, 0x00, 0x80, 0x00, 0x00, 0x02},
};
enum { BAR_BLUE = 1, BAR_RED = 2, BAR_GREEN = 3, BAR_YELLOW = 4 };
enum { BAR_ADD = 0, BAR_REMOVE = 1, BAR_HEALTH = 2, BAR_TITLE = 3, BAR_STYLE = 4 };

void Server::perfBarState(PerfBarLine out[PERF_BARS]) {
    char line[120];
    int color = tps >= 19.5f ? BAR_GREEN : tps >= 15 ? BAR_YELLOW : BAR_RED;
    snprintf(line, sizeof(line), "TPS %.1f | %.1f ms/tick (max %u) | stall %u ms", tps, msptAvg, (unsigned)tickMaxMs,
             (unsigned)stallMaxMs);
    textJson(out[0].json, sizeof(out[0].json), line, color == BAR_GREEN ? "green" : color == BAR_YELLOW ? "yellow" : "red");
    out[0].health = tps / 20;
    out[0].color = color;

    size_t heap = plat::freeHeap();
    if (heap > perfHeapMax_) perfHeapMax_ = heap;
    snprintf(line, sizeof(line), "heap %u KB | %d chunks | %d mobs | %d/%d online", (unsigned)(heap / 1024),
             world.residentCount(), mobCount(), onlineCount(), cfg.maxPlayers);
    textJson(out[1].json, sizeof(out[1].json), line, "aqua");
    out[1].health = perfHeapMax_ ? (float)heap / (float)perfHeapMax_ : 1;
    out[1].color = BAR_BLUE;
    for (int i = 0; i < PERF_BARS; i++) {
        if (out[i].health < 0) out[i].health = 0;
        if (out[i].health > 1) out[i].health = 1;
    }
}

void Server::sendPerfBarAdd(Player& p) {
    if (!perfBar_) return;
    PerfBarLine bars[PERF_BARS];
    perfBarState(bars);
    for (int i = 0; i < PERF_BARS; i++) {
        Packet pk(pkt::s2c::BossBar);
        pk.w.uuid(PERF_BAR_UUID[i]);
        pk.w.varint(BAR_ADD);
        writeTextNbt(pk.w, bars[i].json);
        pk.w.f32(bars[i].health);
        pk.w.varint(bars[i].color);
        pk.w.varint(0);   // no notches
        pk.w.u8(0);       // no flags (sky darkening, music, fog)
        p.conn.send(pk);
    }
}

void Server::setPerfBar(bool on) {
    if (on == perfBar_) return;
    perfBar_ = on;
    perfHeapMax_ = 0;
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& p = players[i];
        if (!p.inPlay()) continue;
        if (on) {
            sendPerfBarAdd(p);
            continue;
        }
        for (int b = 0; b < PERF_BARS; b++) {
            Packet pk(pkt::s2c::BossBar);
            pk.w.uuid(PERF_BAR_UUID[b]);
            pk.w.varint(BAR_REMOVE);
            p.conn.send(pk);
        }
    }
}

// once a second while the banner is on
void Server::tickPerfBar() {
    if (!perfBar_ || ticks % 20 != 0) return;
    PerfBarLine bars[PERF_BARS];
    perfBarState(bars);
    for (int i = 0; i < PERF_BARS; i++) {
        {
            Packet pk(pkt::s2c::BossBar);
            pk.w.uuid(PERF_BAR_UUID[i]);
            pk.w.varint(BAR_TITLE);
            writeTextNbt(pk.w, bars[i].json);
            broadcast(pk);
        }
        {
            Packet pk(pkt::s2c::BossBar);
            pk.w.uuid(PERF_BAR_UUID[i]);
            pk.w.varint(BAR_HEALTH);
            pk.w.f32(bars[i].health);
            broadcast(pk);
        }
        {
            Packet pk(pkt::s2c::BossBar);
            pk.w.uuid(PERF_BAR_UUID[i]);
            pk.w.varint(BAR_STYLE);
            pk.w.varint(bars[i].color);
            pk.w.varint(0);
            broadcast(pk);
        }
    }
}

// ------------------------------------------------------------------ world events
void Server::onBlockChanged(uint8_t dim, int x, int y, int z, uint16_t oldState, uint16_t newState) {
    onBlockUpdated(dim, x, y, z, oldState, newState, 3);
}
void Server::onBlockUpdated(uint8_t dim, int x, int y, int z, uint16_t oldState, uint16_t newState, uint8_t flags) {
    if (flags & 2) {
        Packet pk(pkt::s2c::BlockChange);
        pk.w.u64(packPos(x, y, z));
        pk.w.varint(newState);
        broadcastNearIn(dim, pk, x >> 4, z >> 4);
    }
    // light changes if the light-relevant properties differ
    const BlockDef& a = blockOf(oldState);
    const BlockDef& b = blockOf(newState);
    if (a.filterLight != b.filterLight || stateEmission(oldState) != stateEmission(newState)) {
        // light reaches 15 blocks: the neighbours' exact light can change too
        int cx = x >> 4, cz = z >> 4;
        queueLight(dim, cx, cz);
        for (int dz = -1; dz <= 1; dz++)
            for (int dx = -1; dx <= 1; dx++)
                if ((dx || dz) && exactLightWanted(dim, cx + dx, cz + dz)) queueLight(dim, cx + dx, cz + dz);
    }
    redstone.changed(*this, dim, x, y, z, oldState, newState, flags);
}

void Server::queueLight(uint8_t dim, int cx, int cz) {
    for (int i = 0; i < lightQLen_; i++)
        if (lightQ_[i][0] == dim && lightQ_[i][1] == cx && lightQ_[i][2] == cz) return;
    if (lightQLen_ < LIGHT_QUEUE) {
        lightQ_[lightQLen_][0] = dim;
        lightQ_[lightQLen_][1] = cx;
        lightQ_[lightQLen_][2] = cz;
        lightQLen_++;
    }
}

// Does a player who has chunk (cx, cz) get exact light for it?
bool Server::exactLightWanted(uint8_t dim, int cx, int cz) {
    if (cfg.exactLightDistance < 0) return false;
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        const Player& p = players[i];
        if (!p.inPlay() || !p.hasChunk(dim, cx, cz)) continue;
        int dx = abs(cx - p.centerCx), dz = abs(cz - p.centerCz);
        if ((dx > dz ? dx : dz) <= cfg.exactLightDistance) return true;
    }
    return false;
}

void Server::onChunkEvicted(Chunk& c) {
    // its block ticks and furnaces were stored with it (if it was saved); unloaded chunks
    // do not tick
    int x0 = c.cx * 16, z0 = c.cz * 16;
    uint8_t dim = c.dim;
    timers.removeIf([x0, z0, dim](const TimerEvent& ev) {
        return (ev.key.kind == TK_BLOCK || ev.key.kind == TK_FURNACE) && ev.key.dim == dim && ev.key.x >= x0 &&
               ev.key.x < x0 + 16 && ev.key.z >= z0 && ev.key.z < z0 + 16;
    });
}

void Server::onChunkReady(Chunk& c) {
    // scheduled ticks stored with the chunk continue where they were
    uint32_t now = worldTick();
    for (int i = 0; i < c.tickCount; i++) {
        const ChunkTick& k = c.ticks[i];
        int32_t d = k.delay > 0 ? k.delay : 1;
        timers.schedule(TimerKey::block(c.cx * 16 + k.lx, k.y, c.cz * 16 + k.lz, k.block, c.dim), now + (uint32_t)d,
                        k.prio);
    }
    c.clearTicks();
    // furnaces resume from their stored state
    for (TileEntity* t = c.tiles(); t; t = t->next) {
        if (t->type != TILE_FURNACE) continue;
        t->updated = now;
        if (t->burnTime > 0 || !t->items[0].empty())
            timers.schedule(TimerKey::furnace(c.cx * 16 + t->lx, t->y, c.cz * 16 + t->lz, c.dim), now + 1);
    }
}

void Server::prepareChunkSave(Chunk& live) {
    InDim in(*this, live.dim);
    for (TileEntity* t = live.tiles(); t; t = t->next)
        if (t->type == TILE_FURNACE) updateFurnace(live.cx * 16 + t->lx, t->y, live.cz * 16 + t->lz, false);
}

void Server::attachTicks(Chunk& target) {
    if (!timers.size()) {
        target.clearTicks();
        return;
    }
    int x0 = target.cx * 16, z0 = target.cz * 16;
    uint8_t dim = target.dim;
    int n = 0;
    timers.forEach([&](const TimerEvent& ev) {
        if (ev.key.kind == TK_BLOCK && ev.key.dim == dim && ev.key.x >= x0 && ev.key.x < x0 + 16 && ev.key.z >= z0 &&
            ev.key.z < z0 + 16)
            n++;
    });
    if (!n) {
        target.clearTicks();
        return;
    }
    ChunkTick* list = (ChunkTick*)plat::bigAlloc(sizeof(ChunkTick) * (size_t)n);
    if (!list) return;
    int k = 0;
    uint32_t now = worldTick();
    timers.forEachOrdered([&](const TimerEvent& ev) {
        if (ev.key.kind != TK_BLOCK || ev.key.dim != dim || ev.key.x < x0 || ev.key.x >= x0 + 16 || ev.key.z < z0 ||
            ev.key.z >= z0 + 16 || k >= n)
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

bool Server::isChunkPinned(uint8_t dim, int cx, int cz) {
    if (redstone.pinsChunk(dim, cx, cz)) return true;
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& p = players[i];
        if (p.state != CS_PLAY || p.e.dim != dim) continue;
        // only the simulation area must stay resident; farther chunks were sent to the
        // client already and are reloaded / regenerated when needed again
        int d = cfg.simulationDistance < p.viewDist ? cfg.simulationDistance : p.viewDist;
        if (cx >= p.centerCx - d && cx <= p.centerCx + d && cz >= p.centerCz - d && cz <= p.centerCz + d) return true;
    }
    return false;
}

// A chunk that was sent near a player with per-chunk light (a neighbour was missing, or
// exact light was busy) gets exact light once that is possible.
void Server::upgradePartialLight() {
    if (!chunkJobs.regionLightFree() || lightQLen_ > 0) return;
    for (int i = 0; i < world.tableSize(); i++) {
        Chunk* c = world.slot(i);
        if (!c || !c->lightPartial) continue;
        if (!exactLightWanted(c->dim, c->cx, c->cz)) {
            c->lightPartial = false;   // nobody near it any more
            continue;
        }
        bool all = true;
        for (int k = 0; k < 9 && all; k++)
            if (k != 4 && !world.peek(c->dim, c->cx + k % 3 - 1, c->cz + k / 3 - 1)) all = false;
        if (!all) continue;
        c->lightPartial = false;
        queueLight(c->dim, c->cx, c->cz);
        return;   // one at a time: exact light runs one at a time anyway
    }
}

void Server::flushLightQueue() {
    int n = 0;
    while (n < lightQLen_ && n < 4 && resendLight((uint8_t)lightQ_[n][0], lightQ_[n][1], lightQ_[n][2])) n++;
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
    tickPerfBar();
    part(LagProfile::P_OTHER);
    tickPlayers();   // accounts PLAYERS and STREAM itself
    for (Player& p : players) if (p.inPlay() && !p.dead) {
        InDim in(*this, p.e.dim);
        redstone.entityInside(*this, p.e);
    }
    t = plat::millis();
    tickBlocks();
    tickFurnaceViewers();
    redstone.runBlockEvents(*this);
    part(LagProfile::P_BLOCKS);
    tickEntities();
    tickDragonFight();
    tickBlockEntities();
    part(LagProfile::P_ENTITIES);
    tickMobSpawning();
    part(LagProfile::P_SPAWN);
    trackEntities();
    part(LagProfile::P_TRACK);
    if (ticks % 5 == 0) upgradePartialLight();
    flushLightQueue();
    part(LagProfile::P_LIGHT);
    autosave();
    part(LagProfile::P_SAVE);
    world.maintain();
    world.trimSnapshots();   // unused copies go at once: sharing is within a tick and while jobs hold them
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
    meta.raining = meta.raining ? 0 : 1;
    meta.weatherTimer = meta.raining ? 6000 + (int)(plat::random32() % 6000) : 12000 + (int)(plat::random32() % 84000);
    sendWeather(nullptr);
}

// The client starts rain at level 0 and stops it at level 1 (vanilla then fades the level
// with further packets), so the event alone shows the opposite weather: the levels follow.
void Server::sendWeather(Player* to) {
    const struct { uint8_t reason; float value; } events[] = {
        {(uint8_t)(meta.raining ? 1 : 2), 0},          // start / stop raining
        {7, meta.raining ? 1.0f : 0.0f},               // rain level
        {8, meta.raining == 2 ? 1.0f : 0.0f},          // thunder level
    };
    for (const auto& e : events) {
        Packet pk(pkt::s2c::GameStateChange);
        pk.w.u8(e.reason);
        pk.w.f32(e.value);
        if (to) to->conn.send(pk);
        else broadcast(pk);
    }
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
            pk.w.u8(0x10);
            pk.w.varint(1);
            pk.w.uuid(p.uuid);
            pk.w.varint(p.ping);
            broadcast(pk);
        }
        if (p.chatSpam > 0) p.chatSpam--;
        InDim in(*this, p.e.dim);
        if (p.winKind == WK_LECTERN) {
            double dx = p.e.x - (p.winX + .5), dy = p.e.y - (p.winY + .5), dz = p.e.z - (p.winZ + .5);
            uint16_t state = blockAt(p.winX, p.winY, p.winZ);
            if (dx * dx + dy * dy + dz * dz > 64 || blockIdOf(state) != blk::Lectern || !getBool(state, "has_book"))
                closeWindow(p, true);
        }
        tickSurvival(p);
        tickPortal(p);
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
    struct Save { PlayerData data; bool ok = false; };
    auto* saved = new Save();
    p.toData(saved->data);
    if (!storageIo.post([saved](Storage& s) { saved->ok = s.savePlayer(saved->data) && s.flush(); },
                        [this, saved]() {
                            if (!saved->ok) { ++storageErrors_; MC_LOGW("could not save player %s", saved->data.name); }
                            delete saved;
                        })) {
        // Logout snapshots must not disappear under backpressure. The reserved
        // synchronous slot preserves FIFO ordering, even if the player slot is reused.
        if (!storage->savePlayer(saved->data)) { ++storageErrors_; MC_LOGW("could not save player %s", p.name); }
        delete saved;
    }
}

void Server::packWorldState() {
    if (Entity* d = dragon()) wstate.dragon.dragonHealth = d->health > 0 ? d->health : 200;
    size_t n = wstate.encode(meta.extra, WorldMeta::EXTRA_CAP);
    meta.extraLen = (uint16_t)n;
    wstate.dirty = false;
}

void Server::saveMetaLater() {
    if (!storage) return;
    packWorldState();
    struct Save { WorldMeta data; bool ok = false; };
    auto* saved = new Save{meta};
    if (!storageIo.post([saved](Storage& s) { saved->ok = s.saveMeta(saved->data); },
                        [this, saved]() { if (!saved->ok) ++storageErrors_; delete saved; })) {
        if (!storage->saveMeta(meta)) ++storageErrors_;
        delete saved;
    }
}

bool Server::requestSave(Player* requester) {
    if (manualSave_) return false;
    manualSave_ = true;
    ++saveGeneration_;
    saveRequester_ = requester ? requester->slot : -1;
    saveSession_ = requester ? requester->session : 0;
    saving_ = true;
    saveErrorsAtStart_ = storageErrors_;
    chunkErrorsAtStart_ = world.stats().saveErrors;
    for (auto& p : players) savePlayer(p);
    saveMetaLater();
    return true;
}

void Server::finishSave(bool ok) {
    if (!manualSave_) return;
    const char* msg = ok ? "Saved the game" : "Save failed; dirty chunks retained for retry";
    if (saveRequester_ >= 0) {
        auto& p = players[saveRequester_];
        if (p.inPlay() && p.session == saveSession_) p.sendSystem(msg, ok ? "gray" : "red");
    } else MC_LOGI("%s", msg);
    manualSave_ = false;
}

void Server::autosave() {
    uint32_t now = plat::millis();
    if (!saving_ && !manualSave_ && now - lastSaveMs_ > (uint32_t)cfg.autosaveSeconds * 1000) {
        saving_ = true;
        ++saveGeneration_;
        lastSaveMs_ = now;
        saveErrorsAtStart_ = storageErrors_;
        chunkErrorsAtStart_ = world.stats().saveErrors;
        for (auto& p : players) savePlayer(p);
        saveMetaLater();
    }
    if (!saving_) return;
    bool failed = storageErrors_ != saveErrorsAtStart_ || world.stats().saveErrors != chunkErrorsAtStart_;
    if (failed) { saving_ = false; finishSave(false); return; }
    if (chunkJobs.saveDirty(4) || chunkJobs.savesInFlight()) return;
    if (storage && !storageIo.available()) return;
    saving_ = false;
    if (world.dirtyCount()) {
        MC_LOGW("storage: could not prepare all dirty chunks for saving");
        finishSave(false);
        return;
    }
    if (!storage) { finishSave(true); return; }
    auto* ok = new bool(false);
    const uint32_t generation = saveGeneration_, errors = saveErrorsAtStart_, chunkErrors = chunkErrorsAtStart_;
    storageIo.post([ok](Storage& s) { *ok = s.flush(); }, [this, ok, generation, errors, chunkErrors]() {
        bool success = *ok && storageErrors_ == errors && world.stats().saveErrors == chunkErrors;
        if (!success) MC_LOGW("storage: save/flush failed");
        if (generation == saveGeneration_) finishSave(success);
        delete ok;
    });
}

void Server::saveAll(bool flushStorage) {
    chunkJobs.drain();  // in-flight saves first, then everything else synchronously
    const uint32_t errors = storageErrors_, chunkErrors = world.stats().saveErrors;
    for (int i = 0; i < MC_MAX_PLAYERS; i++) savePlayer(players[i]);
    int n = world.saveAll();
    bool ok = true;
    if (storage) {
        packWorldState();
        ok = storage->saveMeta(meta);
        if (flushStorage && !storage->flush()) ok = false;
    }
    storageIo.drain();
    if (ok && errors == storageErrors_ && chunkErrors == world.stats().saveErrors)
        MC_LOGI("saved world (%d chunks)", n);
    else MC_LOGW("world save failed (%d chunks saved, %d dirty)", n, world.dirtyCount());
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
