// Connection setup: handshake, server list ping, login, configuration and the join sequence.
#include <stdio.h>
#include <string.h>
#include "mc/md5.h"
#include "mc/registry.h"
#include "mc/server/favicon.h"
#include "mc/server/server.h"
#include "mc/storage/storage.h"
#include "mc/text.h"

namespace mc {

void Player::reset(Server* s, int slotIndex) {
    srv = s;
    slot = slotIndex;
    session++;
    pendingSends = 0;
    batchChunks = -1;
    unackedBatches = 0;
    maxUnackedBatches = 1;
    chunksPerTick = 9.0f;
    batchQuota = 0;
    state = CS_FREE;
    protocol = 0;
    name[0] = 0;
    memset(uuid, 0, sizeof(uuid));
    e = Entity();
    gamemode = 0;
    op = false;
    flying = false;
    dead = false;
    viewDist = 4;
    clientViewDist = 8;
    centerCx = centerCz = 0;
    viewReady = false;
    memset(sent, 0, sizeof(sent));
    awaitTeleport = false;
    teleportId = 0;
    positionReady = false;
    kaPending = false;
    kaSentMs = 0;
    ping = 0;
    for (auto& it : inv) it.clear();
    held = 0;
    cursor.clear();
    winId = 0;
    winKind = WK_NONE;
    for (auto& it : craft) it.clear();
    nextWinId = 1;
    food = 20;
    saturation = 5;
    exhaustion = 0;
    foodTimer = 0;
    xpLevel = 0;
    xpProgress = 0;
    xpTotal = 0;
    usingTicks = 0;
    drawingBow = false;
    hasSpawn = false;
    digging = false;
    digStage = -1;
    skinParts = 0x7F;
    mainHand = 1;
    chatSpam = 0;
    knownPlayers = 0;
    memset(knownEntities, 0, sizeof(knownEntities));
    lastHeaderMs = 0;
    portalTicks = 0;
    portalCooldown = 0;
    travelTo = -1;
    travelPortal = false;
    bossBar = false;
    menuPage = 0;
    menuInput = 0;
    menuSeedSet = false;
    menuTarget = -1;
    windowState = 0;
    delete joinData;
    joinData = nullptr;
    inputs = 0;
    lastSequence = -1;
}

void Player::onPacket(int id, Reader& r) {
    switch (state) {
        case CS_HANDSHAKE: handleHandshake(id, r); break;
        case CS_STATUS: handleStatus(id, r); break;
        case CS_LOGIN:
        case CS_LOGIN_ACK: handleLogin(id, r); break;
        case CS_CONFIG: handleConfig(id, r); break;
        case CS_PLAY: handlePlay(id, r); break;
        default: break;
    }
}

void Player::handleHandshake(int id, Reader& r) {
    if (id != 0x00) { conn.close(); state = CS_FREE; return; }
    protocol = r.varint();
    char host[256];
    r.string(host, sizeof(host));
    r.u16();
    int next = r.varint();
    if (!r.ok()) { conn.close(); state = CS_FREE; return; }
    if (next == 1) state = CS_STATUS;
    else if (next == 2) state = CS_LOGIN;   // 3 (transfer) is not accepted: no transfers here
    else { conn.close(); state = CS_FREE; }
}

void Player::handleStatus(int id, Reader& r) {
    if (id == 0x00) {
        // build the status JSON
        char sample[MC_MAX_PLAYERS * 80 + 8];
        size_t n = 0;
        sample[0] = 0;
        int online = 0;
        for (int i = 0; i < MC_MAX_PLAYERS; i++) {
            Player& p = srv->players[i];
            if (p.state != CS_PLAY) continue;
            char id36[40];
            const uint8_t* u = p.uuid;
            snprintf(id36, sizeof(id36), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", u[0], u[1],
                     u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
            n += (size_t)snprintf(sample + n, sizeof(sample) - n, "%s{\"name\":\"%s\",\"id\":\"%s\"}", online ? "," : "",
                                  p.name, id36);
            online++;
        }
        char motd[200];
        jsonEscape(srv->cfg.motd, motd, sizeof(motd));
        Packet pk(0x00);
        // written in pieces to avoid one huge snprintf buffer
        char head[600];
        snprintf(head, sizeof(head),
                 "{\"version\":{\"name\":\"%s\",\"protocol\":%d},\"players\":{\"max\":%d,\"online\":%d,\"sample\":[%s]},"
                 "\"description\":{\"text\":\"%s\"},\"enforcesSecureChat\":false,\"favicon\":\"",
                 VERSION_NAME, PROTOCOL_VERSION, srv->cfg.maxPlayers, online, sample, motd);
        size_t hl = strlen(head), fl = strlen(FAVICON);
        pk.w.varint((int32_t)(hl + fl + 2));
        pk.w.bytes((const uint8_t*)head, hl);
        pk.w.bytes((const uint8_t*)FAVICON, fl);
        pk.w.bytes((const uint8_t*)"\"}", 2);
        conn.send(pk);
    } else if (id == 0x01) {
        int64_t payload = r.i64();
        Packet pk(0x01);
        pk.w.i64(payload);
        conn.send(pk);
        conn.flush();
        conn.close();
        state = CS_FREE;
    }
}

static bool validName(const char* n) {
    size_t l = strlen(n);
    if (l < 3 || l > 16) return false;
    for (const char* p = n; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

void Player::handleLogin(int id, Reader& r) {
    if (state == CS_LOGIN_ACK) {
        if (id == pkt::login_c2s::LoginAcknowledged) startConfiguration();
        return;
    }
    if (id != pkt::login_c2s::LoginStart) return;
    r.string(name, sizeof(name));
    if (!r.ok()) { conn.close(); state = CS_FREE; return; }
    if (protocol != PROTOCOL_VERSION) {
        char msg[80];
        snprintf(msg, sizeof(msg), protocol < PROTOCOL_VERSION ? "Outdated client! Please use %s" : "Outdated server! I'm still on %s",
                 VERSION_NAME);
        kick(msg);
        return;
    }
    if (!validName(name)) { kick("Invalid username"); return; }
    for (const auto& other : srv->players) {
        if (&other != this && other.state >= CS_LOADING && other.conn.open() && !strcmp(other.name, name)) {
            kick("You are already logged in"); return;
        }
    }
    if (!srv->isWhitelisted(name)) { kick("You are not white-listed on this server!"); return; }
    if (srv->onlineCount() >= srv->cfg.maxPlayers) { kick("The server is full!"); return; }
    offlineUuid(name, uuid);
    if (!srv->storage) { finishLogin(nullptr); return; }
    struct LoginData { PlayerData data; LoadResult result = LOAD_ERROR; };
    auto* saved = new LoginData();
    memcpy(saved->data.uuid, uuid, sizeof(uuid));
    uint32_t expectedSession = session;
    state = CS_LOADING;
    bool queued = srv->storageIo.post(
        [saved](Storage& s) { saved->result = s.fetchPlayer(saved->data.uuid, saved->data); },
        [this, saved, expectedSession]() {
            if (session == expectedSession && state == CS_LOADING && conn.open()) {
                if (saved->result == LOAD_ERROR) kick("Player storage unavailable; please retry");
                else finishLogin(saved->result == LOAD_OK ? &saved->data : nullptr);
            }
            delete saved;
        });
    if (!queued) { delete saved; kick("Storage busy; please retry"); }
}

void Player::finishLogin(const PlayerData* data) {
    if (srv->onlineCount() >= srv->cfg.maxPlayers) { kick("The server is full!"); return; }
    if (data) joinData = new PlayerData(*data);
    if (srv->cfg.compressionThreshold >= 0) {
        Packet pk(pkt::login_s2c::Compress);
        pk.w.varint(srv->cfg.compressionThreshold);
        conn.send(pk);
        conn.setCompression(srv->cfg.compressionThreshold);
    }
    Packet ok(pkt::login_s2c::Success);
    ok.w.uuid(uuid);
    ok.w.string(name);
    ok.w.varint(0);   // no profile properties (offline mode: default skin)
    conn.send(ok);
    state = CS_LOGIN_ACK;
}

// The configuration state: the registries the client needs (dimension types, biomes,
// damage types, ...), the tags, then play. Which data the registries carry depends on
// the client's answer to select_known_packs.
void Player::startConfiguration() {
    state = CS_CONFIG;
    {
        Packet pk(pkt::cfg_s2c::CustomPayload);
        pk.w.string("minecraft:brand");
        pk.w.string("esp32-mc");
        conn.send(pk);
    }
    {
        Packet pk(pkt::cfg_s2c::FeatureFlags);
        pk.w.varint(1);
        pk.w.string("minecraft:vanilla");
        conn.send(pk);
    }
    {   // the client answers with the packs it has
        Packet pk(pkt::cfg_s2c::SelectKnownPacks);
        pk.w.varint(1);
        pk.w.string("minecraft");
        pk.w.string("core");
        pk.w.string(VERSION_NAME);
        conn.send(pk);
    }
}

// A vanilla client has the minecraft:core pack: the entries go by name only (as from a
// vanilla server, a few KB), except the overworld's changed dimension types. Others
// (mineflayer) get the data of every entry, which the prebuilt firmware does not carry.
void Player::sendRegistries(bool knownPack) {
#ifdef MC_NO_REGISTRY_DATA
    if (!knownPack) {
        kick("This server needs the vanilla Minecraft 1.21.8 client (a build from source also serves other clients)");
        return;
    }
#endif
    // deflated at build time: no compression on the game loop
    for (int i = 0; i < NUM_SYNCED_REGISTRIES; i++) {
#ifndef MC_NO_REGISTRY_DATA
        if (!knownPack) {
            conn.sendPrebuilt(pkt::cfg_s2c::RegistryData, REGISTRY_PAYLOAD_FULL[i], REGISTRY_PAYLOAD_FULL_LEN[i],
                              REGISTRY_PAYLOAD_FULL_Z[i], REGISTRY_PAYLOAD_FULL_Z_LEN[i]);
            continue;
        }
#endif
        conn.sendPrebuilt(pkt::cfg_s2c::RegistryData, REGISTRY_PAYLOAD[i], REGISTRY_PAYLOAD_LEN[i], REGISTRY_PAYLOAD_Z[i],
                          REGISTRY_PAYLOAD_Z_LEN[i]);
    }
    conn.sendPrebuilt(pkt::cfg_s2c::Tags, TAGS_PAYLOAD, TAGS_PAYLOAD_LEN, TAGS_PAYLOAD_Z, TAGS_PAYLOAD_Z_LEN);
    Packet done(pkt::cfg_s2c::FinishConfiguration);
    conn.send(done);
}

void Player::handleConfig(int id, Reader& r) {
    using namespace pkt::cfg_c2s;
    switch (id) {
        case Settings: onSettings(r); break;
        case KeepAlive: onKeepAlive(r); break;
        case SelectKnownPacks: {
            bool core = false;
            int32_t n = r.varint();
            for (int32_t i = 0; i < n && i < 64 && r.ok(); i++) {
                char ns[64], id[64], version[32];
                r.string(ns, sizeof(ns));
                r.string(id, sizeof(id));
                r.string(version, sizeof(version));
                if (r.ok() && !strcmp(ns, "minecraft") && !strcmp(id, "core") && !strcmp(version, VERSION_NAME)) core = true;
            }
            if (r.ok()) sendRegistries(core);
            break;
        }
        case FinishConfiguration: {
            PlayerData* data = joinData;
            joinData = nullptr;
            state = CS_PLAY;
            joinGame(data);
            delete data;
            break;
        }
        default: break;   // known packs, brand, resource pack answers
    }
}

void Player::joinGame(const PlayerData* data) {
    Server& s = *srv;
    op = s.isOp(name);
    e.kind = EK_PLAYER;
    e.type = ent::Player;
    e.id = s.newEntityId();
    memcpy(e.uuid, uuid, 16);
    e.width = 0.6f;
    e.height = 1.8f;
    e.playerSlot = (int8_t)slot;
    gamemode = s.cfg.defaultGameMode;

    if (data) {
        fromData(*data);
    } else {
        e.x = s.meta.spawnX + 0.5;
        e.y = s.meta.spawnY;
        e.z = s.meta.spawnZ + 0.5;
        e.health = 20;
    }
    if (e.y < dimVoidY(e.dim) + 4 || e.health <= 0) {
        e.x = s.meta.spawnX + 0.5; e.y = s.meta.spawnY; e.z = s.meta.spawnZ + 0.5;
        e.dim = DIM_OVERWORLD;
        e.health = 20;
        food = 20;
    }
    e.sx = e.x; e.sy = e.y; e.sz = e.z;
    lastX = e.x; lastY = e.y; lastZ = e.z;
    viewDist = clientViewDist < s.cfg.viewDistance ? (clientViewDist < 2 ? 2 : clientViewDist) : s.cfg.viewDistance;
    MC_LOGI("%s joined (%s, entity %d) at %.1f %.1f %.1f", name, conn.peer(), (int)e.id, e.x, e.y, e.z);

    {   // Join Game
        Packet pk(pkt::s2c::Login);
        pk.w.i32(e.id);
        pk.w.boolean(false);   // hardcore
        pk.w.varint(NUM_DIMS);
        for (int d = 0; d < NUM_DIMS; d++) pk.w.string(DIMENSION_NAME[d]);
        pk.w.varint(s.cfg.maxPlayers);
        pk.w.varint(viewDist);
        pk.w.varint(viewDist);   // simulation distance
        pk.w.boolean(false);     // reduced debug info
        pk.w.boolean(true);      // respawn screen
        pk.w.boolean(false);     // limited crafting
        s.writeSpawnInfo(pk.w, *this);
        pk.w.boolean(false);     // secure chat not enforced
        conn.send(pk);
    }
    {
        Packet pk(pkt::s2c::Difficulty);
        pk.w.varint(s.cfg.difficulty);
        pk.w.boolean(true);
        conn.send(pk);
    }
    sendAbilities();
    {
        Packet pk(pkt::s2c::HeldItemSlot);
        pk.w.varint(held);
        conn.send(pk);
    }
    {   // no recipe property sets, no stonecutter recipes
        Packet pk(pkt::s2c::DeclareRecipes);
        pk.w.varint(0);
        pk.w.varint(0);
        conn.send(pk);
    }
    {   // permission level -> enables F3+F4 gamemode switcher etc. for ops
        Packet pk(pkt::s2c::EntityStatus);
        pk.w.i32(e.id);
        pk.w.i8(op ? 28 : 24);
        conn.send(pk);
    }
    conn.sendStreamed([&](Writer& w) {
        w.varint(pkt::s2c::DeclareCommands);
        s.writeCommandTree(w);
    });
    {   // world border
        Packet pk(pkt::s2c::InitializeWorldBorder);
        pk.w.f64(0.0);
        pk.w.f64(0.0);
        double diameter = (double)s.meta.radius * 32.0;
        pk.w.f64(diameter);
        pk.w.f64(diameter);
        pk.w.varlong(0);
        pk.w.varint(29999984);
        pk.w.varint(15);
        pk.w.varint(5);
        conn.send(pk);
    }
    {
        Packet pk(pkt::s2c::SpawnPosition);
        pk.w.u64(packPos(s.meta.spawnX, s.meta.spawnY, s.meta.spawnZ));
        pk.w.f32(0);
        conn.send(pk);
    }
    sendTime();
    if (s.meta.raining) s.sendWeather(this);
    // tab list: everybody to the new player, the new player to everybody
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& o = s.players[i];
        if (&o != this && o.state == CS_PLAY) s.sendPlayerInfoAdd(this, o);
    }
    s.sendPlayerInfoAdd(nullptr, *this);
    s.sendTabHeader(*this);
    s.sendPerfBarAdd(*this);

    teleport(e.x, e.y, e.z, e.yaw, e.pitch);
    {   // the client shows "loading terrain" until this and the chunks around it arrived
        Packet pk(pkt::s2c::GameStateChange);
        pk.w.u8(13);
        pk.w.f32(0);
        conn.send(pk);
    }
    updateView(true);
    sendInventory();
    sendHealth();
    sendXp();
    kaSentMs = plat::millis();

    char msg[64];
    snprintf(msg, sizeof(msg), "%s joined the game", name);
    s.broadcastSystem(msg, "yellow");
}

void Player::kick(const char* reason) {
    if (state == CS_FREE) return;
    if (reason && conn.open()) {
        char json[300];
        textJson(json, sizeof(json), reason, nullptr);
        if (state == CS_PLAY || state == CS_CONFIG) {   // text as NBT
            Packet pk(state == CS_PLAY ? pkt::s2c::KickDisconnect : pkt::cfg_s2c::Disconnect);
            writeTextNbt(pk.w, json);
            conn.send(pk);
        } else {   // login: JSON
            Packet pk(pkt::login_s2c::Disconnect);
            pk.w.string(json);
            conn.send(pk);
        }
        conn.flush();
    }
    if (state == CS_PLAY) {
        srv->closeWindow(*this, false);
        srv->savePlayer(*this);
        srv->sendPlayerInfoRemove(*this);
        // remove our entity from everyone else
        for (int i = 0; i < MC_MAX_PLAYERS; i++) {
            Player& o = srv->players[i];
            if (&o == this || !o.inPlay()) continue;
            if (o.knownPlayers & (1u << slot)) {
                srv->sendDestroy(o, e.id);
                o.knownPlayers &= ~(1u << slot);
            }
        }
        char msg[64];
        snprintf(msg, sizeof(msg), "%s left the game", name);
        state = CS_FREE;
        srv->broadcastSystem(msg, "yellow");
        if (reason) MC_LOGI("kicked %s: %s", name, reason);
    }
    conn.close();
    state = CS_FREE;
}

// position: 0 chat, 1 system, 2 action bar (all as system messages: no chat signing)
void Player::sendChat(const char* json, uint8_t position) {
    Packet pk(pkt::s2c::SystemChat);
    writeTextNbt(pk.w, json);
    pk.w.boolean(position == 2);
    conn.send(pk);
}

void Player::sendSystem(const char* text, const char* color) {
    char json[800];
    textJson(json, sizeof(json), text, color);
    sendChat(json, 1);
}

void Player::sendActionBar(const char* text) {
    char json[300];
    textJson(json, sizeof(json), text, nullptr);
    sendChat(json, 2);
}

// ------------------------------------------------------------------ state sync
void Player::teleport(double x, double y, double z, float yaw, float pitch) {
    e.x = x; e.y = y; e.z = z;
    e.yaw = yaw; e.pitch = pitch;
    lastX = x; lastY = y; lastZ = z;
    e.fallDistance = 0;
    Packet pk(pkt::s2c::Position);
    pk.w.varint(++teleportId);
    pk.w.f64(x);
    pk.w.f64(y);
    pk.w.f64(z);
    pk.w.f64(0);   // velocity
    pk.w.f64(0);
    pk.w.f64(0);
    pk.w.f32(yaw);
    pk.w.f32(pitch);
    pk.w.u32(0);   // nothing relative
    conn.send(pk);
    awaitTeleport = true;
    updateView(false);
}

void Player::sendHealth() {
    Packet pk(pkt::s2c::UpdateHealth);
    pk.w.f32(e.health);
    pk.w.varint(food);
    pk.w.f32(saturation);
    conn.send(pk);
    healthDirty = false;
}

void Player::sendXp() {
    Packet pk(pkt::s2c::Experience);
    pk.w.f32(xpProgress);
    pk.w.varint(xpLevel);
    pk.w.varint(xpTotal);
    conn.send(pk);
}

void Player::sendAbilities() {
    uint8_t flags = 0;
    if (gamemode == GM_CREATIVE || gamemode == GM_SPECTATOR) flags |= 0x01 | 0x04;
    if (gamemode == GM_CREATIVE) flags |= 0x08;
    if (gamemode == GM_SPECTATOR) { flags |= 0x02; flying = true; }
    if (flying && (flags & 0x04)) flags |= 0x02;
    Packet pk(pkt::s2c::Abilities);
    pk.w.i8((int8_t)flags);
    pk.w.f32(0.05f);
    pk.w.f32(0.1f);
    conn.send(pk);
}

void Player::sendInventory() {
    windowState++;
    conn.sendStreamed([&](Writer& w) {
        w.varint(pkt::s2c::WindowItems);
        w.varint(0);
        w.varint(windowState);
        w.varint(INV_SIZE);
        for (int i = 0; i < INV_SIZE; ++i) writeSlot(w, inv[i]);
        writeSlot(w, cursor);
    });
    invDirty = false;
}

void Player::sendSlot(int i) {
    conn.sendStreamed([&](Writer& w) {
        w.varint(pkt::s2c::SetSlot);
        w.varint(0);
        w.varint(windowState);
        w.i16((int16_t)i);
        writeSlot(w, inv[i]);
    });
}

void Player::sendGameMode() {
    Packet pk(pkt::s2c::GameStateChange);
    pk.w.u8(3);
    pk.w.f32((float)gamemode);
    conn.send(pk);
    sendAbilities();
}

void Player::setGameMode(uint8_t gm) {
    gamemode = gm;
    if (gm != GM_CREATIVE && gm != GM_SPECTATOR) flying = false;
    e.fallDistance = 0;   // a fall begun in another game mode does not count
    sendGameMode();
    Packet pk(pkt::s2c::PlayerInfo);
    pk.w.u8(0x04);   // update game mode
    pk.w.varint(1);
    pk.w.uuid(uuid);
    pk.w.varint(gm);
    srv->broadcast(pk);
    e.flags = (uint8_t)((e.flags & ~EF_INVISIBLE) | (gm == GM_SPECTATOR ? EF_INVISIBLE : 0));
    e.metaDirty = true;
}

void Player::sendTime() {
    Packet pk(pkt::s2c::UpdateTime);
    pk.w.i64(srv->meta.worldAge);
    pk.w.i64(srv->meta.timeOfDay % 24000);
    pk.w.boolean(true);   // the client advances the time between updates
    conn.send(pk);
}

// ------------------------------------------------------------------ persistence
void Player::toData(PlayerData& d) const {
    memcpy(d.uuid, uuid, 16);
    snprintf(d.name, sizeof(d.name), "%s", name);
    d.x = e.x; d.y = e.y; d.z = e.z;
    d.dim = e.dim;
    d.yaw = e.yaw; d.pitch = e.pitch;
    d.gamemode = gamemode;
    d.health = dead ? 20 : e.health;
    d.food = (uint8_t)food;
    d.saturation = saturation;
    d.xpLevel = xpLevel;
    d.xpProgress = xpProgress;
    d.xpTotal = xpTotal;
    d.held = held;
    d.hasSpawn = hasSpawn;
    d.spawnX = spawnX; d.spawnY = spawnY; d.spawnZ = spawnZ;
    for (int i = 0; i < INV_SIZE; i++) d.inv[i] = inv[i];
    d.inv[SLOT_CRAFT_RESULT].clear();
    if (dead) {   // died and disconnected before respawning
        const Server& s = *srv;
        d.x = hasSpawn ? spawnX + 0.5 : s.meta.spawnX + 0.5;
        d.y = hasSpawn ? spawnY : s.meta.spawnY;
        d.z = hasSpawn ? spawnZ + 0.5 : s.meta.spawnZ + 0.5;
        d.dim = DIM_OVERWORLD;
    }
}

void Player::fromData(const PlayerData& d) {
    e.x = d.x; e.y = d.y; e.z = d.z;
    e.dim = d.dim < NUM_DIMS ? d.dim : DIM_OVERWORLD;
    e.yaw = d.yaw; e.pitch = d.pitch;
    gamemode = d.gamemode <= 3 ? d.gamemode : 0;
    e.health = d.health;
    food = d.food;
    saturation = d.saturation;
    xpLevel = d.xpLevel;
    xpProgress = d.xpProgress;
    xpTotal = d.xpTotal;
    held = d.held < 9 ? d.held : 0;
    hasSpawn = d.hasSpawn;
    spawnX = d.spawnX; spawnY = d.spawnY; spawnZ = d.spawnZ;
    for (int i = 0; i < INV_SIZE; i++) inv[i] = d.inv[i];
}

}  // namespace mc
