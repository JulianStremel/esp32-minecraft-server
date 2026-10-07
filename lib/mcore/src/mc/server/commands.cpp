// Slash commands, the command tree sent to clients and tab completion.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc/registry.h"
#include "mc/server/server.h"

namespace mc {

struct CmdCtx {
    Server& s;
    Player* p;       // nullptr = console
    char** argv;
    int argc;
    void reply(const char* text, const char* color = nullptr) {
        if (p) p->sendSystem(text, color);
        else MC_LOGI("%s", text);
    }
    void replyf(const char* color, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
};

#include <stdarg.h>
void CmdCtx::replyf(const char* color, const char* fmt, ...) {
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    reply(buf, color);
}

typedef void (*CmdFn)(CmdCtx& c);

struct Cmd {
    const char* name;
    bool op;
    const char* usage;
    const char* hints;  // completion kind per argument: p player, i item, e entity, b block, g gamemode,
                        // t time, w weather, d difficulty, x coordinate, - none
    CmdFn fn;
};

// ---------------------------------------------------------------- argument helpers
static Player* targetOrSelf(CmdCtx& c, int idx) {
    if (idx < c.argc) {
        Player* t = c.s.findPlayer(c.argv[idx]);
        if (!t && !strcmp(c.argv[idx], "@s")) t = c.p;
        if (!t) c.replyf("red", "No player was found: %s", c.argv[idx]);
        return t;
    }
    if (!c.p) c.reply("A player is required when run from the console", "red");
    return c.p;
}

static bool parseCoord(const char* s, double base, double& out) {
    if (s[0] == '~') {
        out = base + (s[1] ? atof(s + 1) : 0.0);
        return true;
    }
    char* end;
    out = strtod(s, &end);
    return end != s && *end == 0;
}

static bool parseXYZ(CmdCtx& c, int idx, double& x, double& y, double& z, bool blockCenter) {
    if (idx + 2 >= c.argc) return false;
    double bx = c.p ? c.p->e.x : 0, by = c.p ? c.p->e.y : 64, bz = c.p ? c.p->e.z : 0;
    if (!parseCoord(c.argv[idx], bx, x) || !parseCoord(c.argv[idx + 1], by, y) || !parseCoord(c.argv[idx + 2], bz, z)) return false;
    if (blockCenter) {
        // integers given without decimals refer to the block centre
        if (!strchr(c.argv[idx], '.') && c.argv[idx][0] != '~') x += 0.5;
        if (!strchr(c.argv[idx + 2], '.') && c.argv[idx + 2][0] != '~') z += 0.5;
    }
    return true;
}

static int parseGameMode(const char* s) {
    if (!strcmp(s, "survival") || !strcmp(s, "s") || !strcmp(s, "0")) return GM_SURVIVAL;
    if (!strcmp(s, "creative") || !strcmp(s, "c") || !strcmp(s, "1")) return GM_CREATIVE;
    if (!strcmp(s, "adventure") || !strcmp(s, "a") || !strcmp(s, "2")) return GM_ADVENTURE;
    if (!strcmp(s, "spectator") || !strcmp(s, "sp") || !strcmp(s, "3")) return GM_SPECTATOR;
    return -1;
}

static const char* GM_NAMES[4] = {"Survival", "Creative", "Adventure", "Spectator"};

// Parses "stone", "minecraft:oak_stairs[facing=east,half=top]" into a block state.
static int parseBlockState(const char* spec) {
    char name[64];
    const char* br = strchr(spec, '[');
    size_t nl = br ? (size_t)(br - spec) : strlen(spec);
    if (nl >= sizeof(name)) return -1;
    memcpy(name, spec, nl);
    name[nl] = 0;
    int b = findBlock(name);
    if (b < 0) return -1;
    uint16_t st = BLOCKS[b].defState;
    if (br) {
        char props[128];
        snprintf(props, sizeof(props), "%s", br + 1);
        char* end = strchr(props, ']');
        if (end) *end = 0;
        for (char* tok = strtok(props, ","); tok; tok = strtok(nullptr, ",")) {
            char* eq = strchr(tok, '=');
            if (!eq) continue;
            *eq = 0;
            st = setPropStr(st, tok, eq + 1);
        }
    }
    return st;
}

// ---------------------------------------------------------------- commands
static void cmdHelp(CmdCtx& c);

static void cmdList(CmdCtx& c) {
    char buf[256];
    int n = 0;
    size_t len = 0;
    buf[0] = 0;
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& p = c.s.players[i];
        if (p.state != CS_PLAY) continue;
        len += (size_t)snprintf(buf + len, sizeof(buf) - len, "%s%s", n ? ", " : "", p.name);
        n++;
    }
    c.replyf(nullptr, "There are %d of a max of %d players online: %s", n, c.s.cfg.maxPlayers, buf);
}

static void cmdGamemode(CmdCtx& c) {
    if (c.argc < 1) { c.reply("Usage: /gamemode <survival|creative|adventure|spectator> [player]", "red"); return; }
    int gm = parseGameMode(c.argv[0]);
    if (gm < 0) { c.replyf("red", "Unknown game mode: %s", c.argv[0]); return; }
    Player* t = targetOrSelf(c, 1);
    if (!t) return;
    t->setGameMode((uint8_t)gm);
    char msg[96];
    snprintf(msg, sizeof(msg), "Set own game mode to %s Mode", GM_NAMES[gm]);
    t->sendSystem(msg, "gray");
    if (t != c.p) c.replyf("gray", "Set %s's game mode to %s Mode", t->name, GM_NAMES[gm]);
}

static void cmdTp(CmdCtx& c) {
    // /tp <x y z> | /tp <player> | /tp <player> <player> | /tp <player> <x y z>
    double x, y, z;
    Player* who = c.p;
    if (c.argc == 3 && parseXYZ(c, 0, x, y, z, true)) {
        if (!who) { c.reply("Console must name a player", "red"); return; }
    } else if (c.argc == 1) {
        Player* t = c.s.findPlayer(c.argv[0]);
        if (!t || !who) { c.replyf("red", "No player was found: %s", c.argv[0]); return; }
        x = t->e.x; y = t->e.y; z = t->e.z;
    } else if (c.argc == 2) {
        who = c.s.findPlayer(c.argv[0]);
        Player* t = c.s.findPlayer(c.argv[1]);
        if (!who || !t) { c.reply("No player was found", "red"); return; }
        x = t->e.x; y = t->e.y; z = t->e.z;
    } else if (c.argc == 4) {
        who = c.s.findPlayer(c.argv[0]);
        if (!who) { c.replyf("red", "No player was found: %s", c.argv[0]); return; }
        Player* saved = c.p;
        c.p = who;
        bool ok = parseXYZ(c, 1, x, y, z, true);
        c.p = saved;
        if (!ok) { c.reply("Invalid coordinates", "red"); return; }
    } else {
        c.reply("Usage: /tp <x> <y> <z> | /tp <player> [<player>|<x> <y> <z>]", "red");
        return;
    }
    if (!c.s.world.blockInBounds((int)floor(x), (int)floor(z))) { c.reply("Outside the world border", "red"); return; }
    who->teleport(x, y, z, who->e.yaw, who->e.pitch);
    c.replyf("gray", "Teleported %s to %.1f, %.1f, %.1f", who->name, x, y, z);
}

static void cmdGive(CmdCtx& c) {
    // vanilla order: /give <player> <item> [count]; also accept /give <item> [count]
    Player* t = nullptr;
    int itemArg = 0;
    if (c.argc >= 2 && c.s.findPlayer(c.argv[0])) { t = c.s.findPlayer(c.argv[0]); itemArg = 1; }
    else t = c.p;
    if (c.argc <= itemArg || !t) { c.reply("Usage: /give <player> <item> [count]", "red"); return; }
    int item = findItem(c.argv[itemArg]);
    if (item <= 0) { c.replyf("red", "Unknown item: %s", c.argv[itemArg]); return; }
    int count = c.argc > itemArg + 1 ? atoi(c.argv[itemArg + 1]) : 1;
    if (count < 1) count = 1;
    if (count > 64 * 36) count = 64 * 36;
    int given = 0;
    while (count > 0) {
        int n = count > maxStack((uint16_t)item) ? maxStack((uint16_t)item) : count;
        int left = c.s.giveItem(*t, ItemStack::of((uint16_t)item, n));
        if (left) c.s.dropItem(t->e.x, t->e.y + 0.5, t->e.z, ItemStack::of((uint16_t)item, left), false);
        given += n;
        count -= n;
    }
    c.replyf("gray", "Gave %d [%s] to %s", given, ITEMS[item].name, t->name);
}

static void cmdClear(CmdCtx& c) {
    Player* t = targetOrSelf(c, 0);
    if (!t) return;
    int n = 0;
    for (int i = 0; i < INV_SIZE; i++)
        if (!t->inv[i].empty()) { n += t->inv[i].count; t->inv[i].clear(); }
    t->sendInventory();
    c.s.broadcastEquipment(*t);
    c.replyf("gray", "Removed %d items from %s", n, t->name);
}

static void cmdTime(CmdCtx& c) {
    if (c.argc >= 1 && !strcmp(c.argv[0], "query")) {
        c.replyf(nullptr, "The time is %lld (day %lld)", (long long)(c.s.meta.timeOfDay % 24000), (long long)(c.s.meta.timeOfDay / 24000));
        return;
    }
    if (c.argc < 2) { c.reply("Usage: /time <set|add|query> <value>", "red"); return; }
    int64_t v;
    const char* a = c.argv[1];
    if (!strcmp(a, "day")) v = 1000;
    else if (!strcmp(a, "noon")) v = 6000;
    else if (!strcmp(a, "night")) v = 13000;
    else if (!strcmp(a, "midnight")) v = 18000;
    else v = atoll(a);
    if (!strcmp(c.argv[0], "set")) c.s.meta.timeOfDay = (c.s.meta.timeOfDay / 24000) * 24000 + v;
    else if (!strcmp(c.argv[0], "add")) c.s.meta.timeOfDay += v;
    else { c.reply("Usage: /time <set|add|query> <value>", "red"); return; }
    for (int i = 0; i < MC_MAX_PLAYERS; i++)
        if (c.s.players[i].inPlay()) c.s.players[i].sendTime();
    c.replyf("gray", "Set the time to %lld", (long long)(c.s.meta.timeOfDay % 24000));
}

static void cmdWeather(CmdCtx& c) {
    if (c.argc < 1) { c.reply("Usage: /weather <clear|rain|thunder>", "red"); return; }
    bool rain = strcmp(c.argv[0], "clear") != 0;
    c.s.meta.raining = rain;
    c.s.meta.weatherTimer = c.argc > 1 ? atoi(c.argv[1]) * 20 : 6000 + (int)(plat::random32() % 12000);
    Packet pk(pkt::s2c::GameStateChange);
    pk.w.u8(rain ? 1 : 2);
    pk.w.f32(0);
    c.s.broadcast(pk);
    if (!strcmp(c.argv[0], "thunder")) {
        Packet t(pkt::s2c::GameStateChange);
        t.w.u8(8);
        t.w.f32(1);
        c.s.broadcast(t);
    }
    c.replyf("gray", "Changed the weather to %s", c.argv[0]);
}

static void cmdKill(CmdCtx& c) {
    Player* t = targetOrSelf(c, 0);
    if (!t) return;
    c.s.damagePlayer(*t, 1000, DC_KILL, -1);
    if (!t->dead) c.s.killPlayer(*t, DC_KILL, -1);
}

static void cmdSpawn(CmdCtx& c) {
    if (!c.p) return;
    c.p->teleport(c.s.meta.spawnX + 0.5, c.s.meta.spawnY, c.s.meta.spawnZ + 0.5, c.p->e.yaw, c.p->e.pitch);
}

static void cmdSetWorldSpawn(CmdCtx& c) {
    double x, y, z;
    if (!parseXYZ(c, 0, x, y, z, false)) {
        if (!c.p) return;
        x = c.p->e.x; y = c.p->e.y; z = c.p->e.z;
    }
    c.s.meta.spawnX = (int)floor(x);
    c.s.meta.spawnY = (int)floor(y);
    c.s.meta.spawnZ = (int)floor(z);
    Packet pk(pkt::s2c::SpawnPosition);
    pk.w.u64(packPos(c.s.meta.spawnX, c.s.meta.spawnY, c.s.meta.spawnZ));
    c.s.broadcast(pk);
    if (c.s.storage) c.s.storage->saveMeta(c.s.meta);
    c.replyf("gray", "Set the world spawn point to %d, %d, %d", (int)c.s.meta.spawnX, (int)c.s.meta.spawnY, (int)c.s.meta.spawnZ);
}

static void cmdSpawnPoint(CmdCtx& c) {
    Player* t = targetOrSelf(c, 0);
    if (!t) return;
    t->hasSpawn = true;
    t->spawnX = (int)floor(t->e.x);
    t->spawnY = (int)floor(t->e.y) - 1;
    t->spawnZ = (int)floor(t->e.z);
    c.replyf("gray", "Set spawn point to %d, %d, %d for %s", t->spawnX, t->spawnY + 1, t->spawnZ, t->name);
}

static void cmdSay(CmdCtx& c) {
    char msg[256] = "", text[300];
    size_t n = 0;
    for (int i = 0; i < c.argc; i++) n += (size_t)snprintf(msg + n, sizeof(msg) - n, "%s%s", i ? " " : "", c.argv[i]);
    snprintf(text, sizeof(text), "[%s] %s", c.p ? c.p->name : "Server", msg);
    c.s.broadcastSystem(text, "light_purple");
}

static void cmdMe(CmdCtx& c) {
    char msg[256] = "", text[300];
    size_t n = 0;
    for (int i = 0; i < c.argc; i++) n += (size_t)snprintf(msg + n, sizeof(msg) - n, "%s%s", i ? " " : "", c.argv[i]);
    snprintf(text, sizeof(text), "* %s %s", c.p ? c.p->name : "Server", msg);
    c.s.broadcastSystem(text, nullptr);
}

static void cmdMsg(CmdCtx& c) {
    if (c.argc < 2) { c.reply("Usage: /msg <player> <message>", "red"); return; }
    Player* t = c.s.findPlayer(c.argv[0]);
    if (!t) { c.replyf("red", "No player was found: %s", c.argv[0]); return; }
    char msg[256] = "", text[320];
    size_t n = 0;
    for (int i = 1; i < c.argc; i++) n += (size_t)snprintf(msg + n, sizeof(msg) - n, "%s%s", i > 1 ? " " : "", c.argv[i]);
    snprintf(text, sizeof(text), "%s whispers to you: %s", c.p ? c.p->name : "Server", msg);
    t->sendSystem(text, "gray");
    snprintf(text, sizeof(text), "You whisper to %s: %s", t->name, msg);
    c.reply(text, "gray");
}

static void cmdSeed(CmdCtx& c) { c.replyf(nullptr, "Seed: [%lld]", (long long)c.s.meta.seed); }

static void cmdDifficulty(CmdCtx& c) {
    static const char* names[4] = {"peaceful", "easy", "normal", "hard"};
    if (c.argc < 1) { c.replyf(nullptr, "The difficulty is %s", names[c.s.cfg.difficulty]); return; }
    for (int i = 0; i < 4; i++)
        if (!strcmp(c.argv[0], names[i]) || atoi(c.argv[0]) == i + (c.argv[0][0] == '0' ? 0 : 100)) {
            c.s.cfg.difficulty = (uint8_t)i;
            Packet pk(pkt::s2c::Difficulty);
            pk.w.u8((uint8_t)i);
            pk.w.boolean(true);
            c.s.broadcast(pk);
            if (i == 0)
                for (int k = 0; k < MC_MAX_ENTITIES; k++)
                    if (c.s.entities[k].kind == EK_MOB && c.s.entities[k].hostile) c.s.removeEntity(c.s.entities[k]);
            c.replyf("gray", "The difficulty has been set to %s", names[i]);
            return;
        }
    c.reply("Usage: /difficulty <peaceful|easy|normal|hard>", "red");
}

static void cmdXp(CmdCtx& c) {
    // /xp add <player> <amount> [points|levels]   or   /xp <amount> [player]
    int idx = 0;
    if (c.argc >= 1 && !strcmp(c.argv[0], "add")) idx = 1;
    Player* t = c.p;
    int amountIdx = idx;
    if (idx == 1 && c.argc >= 3) { t = c.s.findPlayer(c.argv[1]); amountIdx = 2; }
    if (!t || c.argc <= amountIdx) { c.reply("Usage: /xp add <player> <amount> [points|levels]", "red"); return; }
    int amount = atoi(c.argv[amountIdx]);
    bool levels = c.argc > amountIdx + 1 && !strcmp(c.argv[amountIdx + 1], "levels");
    if (levels) {
        t->xpLevel += amount;
        if (t->xpLevel < 0) t->xpLevel = 0;
        t->sendXp();
    } else {
        c.s.giveXp(*t, amount);
    }
    c.replyf("gray", "Gave %d experience %s to %s", amount, levels ? "levels" : "points", t->name);
}

static void cmdHeal(CmdCtx& c) {
    Player* t = targetOrSelf(c, 0);
    if (!t) return;
    t->e.health = 20;
    t->food = 20;
    t->saturation = 20;
    t->e.fireTicks = 0;
    t->sendHealth();
    c.replyf("gray", "Healed %s", t->name);
}

static void cmdFeed(CmdCtx& c) {
    Player* t = targetOrSelf(c, 0);
    if (!t) return;
    t->food = 20;
    t->saturation = 20;
    t->sendHealth();
    c.replyf("gray", "Fed %s", t->name);
}

static void cmdSummon(CmdCtx& c) {
    if (c.argc < 1) { c.reply("Usage: /summon <entity> [x y z]", "red"); return; }
    int type = findEntityType(c.argv[0]);
    double x, y, z;
    if (!parseXYZ(c, 1, x, y, z, true)) {
        if (!c.p) { c.reply("Coordinates required from the console", "red"); return; }
        x = c.p->e.x; y = c.p->e.y; z = c.p->e.z;
    }
    if (type < 0) { c.replyf("red", "Unknown entity: %s", c.argv[0]); return; }
    Entity* e = nullptr;
    if (type == ent::Item) e = c.s.dropItem(x, y, z, ItemStack::of(itm::Stone), false);
    else if (type == ent::Tnt) { c.s.explode(x, y, z, 4.0f, -1); return; }
    else if (!isMobType(type)) { c.replyf("red", "Cannot summon %s (only mobs, items and tnt)", c.argv[0]); return; }
    else e = c.s.spawnMob((uint16_t)type, x, y, z);
    if (!e) { c.reply("Unable to summon entity (entity limit reached?)", "red"); return; }
    c.replyf("gray", "Summoned new %s", c.argv[0]);
}

static void cmdSetblock(CmdCtx& c) {
    double x, y, z;
    if (c.argc < 4 || !parseXYZ(c, 0, x, y, z, false)) { c.reply("Usage: /setblock <x> <y> <z> <block>", "red"); return; }
    int st = parseBlockState(c.argv[3]);
    if (st < 0) { c.replyf("red", "Unknown block: %s", c.argv[3]); return; }
    int bx = (int)floor(x), by = (int)floor(y), bz = (int)floor(z);
    if (by < 0 || by > 255 || !c.s.world.blockInBounds(bx, bz)) { c.reply("That position is out of the world", "red"); return; }
    c.s.setBlock(bx, by, bz, (uint16_t)st);
    c.reply("Changed the block", "gray");
}

static void cmdFill(CmdCtx& c) {
    double x1, y1, z1, x2, y2, z2;
    if (c.argc < 7 || !parseXYZ(c, 0, x1, y1, z1, false) || !parseXYZ(c, 3, x2, y2, z2, false)) {
        c.reply("Usage: /fill <x1> <y1> <z1> <x2> <y2> <z2> <block>", "red");
        return;
    }
    int st = parseBlockState(c.argv[6]);
    if (st < 0) { c.replyf("red", "Unknown block: %s", c.argv[6]); return; }
    int ax = (int)floor(fmin(x1, x2)), bx = (int)floor(fmax(x1, x2));
    int ay = (int)floor(fmin(y1, y2)), by = (int)floor(fmax(y1, y2));
    int az = (int)floor(fmin(z1, z2)), bz = (int)floor(fmax(z1, z2));
    if (ay < 0) ay = 0;
    if (by > 255) by = 255;
    long vol = (long)(bx - ax + 1) * (by - ay + 1) * (bz - az + 1);
    if (vol > 32768) { c.replyf("red", "Too many blocks in the specified area (maximum 32768, specified %ld)", vol); return; }
    long n = 0;
    for (int x = ax; x <= bx; x++)
        for (int z = az; z <= bz; z++) {
            if (!c.s.world.blockInBounds(x, z)) continue;
            for (int y = ay; y <= by; y++) {
                if (c.s.blockAt(x, y, z) != st) {
                    c.s.world.setBlock(x, y, z, (uint16_t)st);
                    n++;
                }
            }
        }
    c.replyf("gray", "Successfully filled %ld blocks", n);
}

static void cmdOp(CmdCtx& c) {
    if (c.argc < 1) { c.reply("Usage: /op <player>", "red"); return; }
    Player* t = c.s.findPlayer(c.argv[0]);
    if (!t) { c.replyf("red", "No player was found: %s", c.argv[0]); return; }
    t->op = true;
    Packet pk(pkt::s2c::EntityStatus);
    pk.w.i32(t->e.id);
    pk.w.i8(28);
    t->conn.send(pk);
    Packet tree(pkt::s2c::DeclareCommands);
    c.s.writeCommandTree(tree.w);
    t->conn.send(tree);
    t->sendSystem("You are now a server operator", "yellow");
    c.replyf("gray", "Made %s a server operator", t->name);
}

static void cmdDeop(CmdCtx& c) {
    if (c.argc < 1) { c.reply("Usage: /deop <player>", "red"); return; }
    Player* t = c.s.findPlayer(c.argv[0]);
    if (!t) { c.replyf("red", "No player was found: %s", c.argv[0]); return; }
    t->op = false;
    Packet pk(pkt::s2c::EntityStatus);
    pk.w.i32(t->e.id);
    pk.w.i8(24);
    t->conn.send(pk);
    c.replyf("gray", "Made %s no longer a server operator", t->name);
}

static void cmdKick(CmdCtx& c) {
    if (c.argc < 1) { c.reply("Usage: /kick <player> [reason]", "red"); return; }
    Player* t = c.s.findPlayer(c.argv[0]);
    if (!t) { c.replyf("red", "No player was found: %s", c.argv[0]); return; }
    char reason[200] = "Kicked by an operator";
    if (c.argc > 1) {
        size_t n = 0;
        for (int i = 1; i < c.argc; i++) n += (size_t)snprintf(reason + n, sizeof(reason) - n, "%s%s", i > 1 ? " " : "", c.argv[i]);
    }
    char name[17];
    snprintf(name, sizeof(name), "%s", t->name);
    t->kick(reason);
    c.replyf("gray", "Kicked %s: %s", name, reason);
}

static void cmdSave(CmdCtx& c) {
    c.reply("Saving the game (this may take a moment!)", "gray");
    c.s.saveAll(true);
    c.reply("Saved the game", "gray");
}

static void cmdStop(CmdCtx& c) {
    c.s.broadcastSystem("Stopping the server", "red");
    c.s.shutdown("Server closed");
}

static void cmdTps(CmdCtx& c) {
    char buf[400];
    c.s.statusLine(buf, sizeof(buf));
    c.reply(buf, "aqua");
}

static void cmdStorage(CmdCtx& c) {
    char st[160] = "no storage configured: the world lives in RAM only";
    if (c.s.storage) c.s.storage->statusLine(st, sizeof(st));
    c.replyf("aqua", "Storage: %s | dirty chunks: %d | loads %u, generated %u, saves %u, save errors %u", st,
             c.s.world.dirtyCount(), (unsigned)c.s.world.stats().loads, (unsigned)c.s.world.stats().generated,
             (unsigned)c.s.world.stats().saves, (unsigned)c.s.world.stats().saveErrors);
}

static void cmdFly(CmdCtx& c) {
    Player* t = targetOrSelf(c, 0);
    if (!t) return;
    // toggles flight permission in survival by switching the ability flags only
    bool allow = !(t->flying);
    t->flying = allow;
    Packet pk(pkt::s2c::Abilities);
    uint8_t flags = allow ? 0x04 : 0;
    if (t->gamemode == GM_CREATIVE) flags |= 0x01 | 0x04 | 0x08;
    pk.w.i8((int8_t)flags);
    pk.w.f32(0.05f);
    pk.w.f32(0.1f);
    t->conn.send(pk);
    c.replyf("gray", "Flight %s for %s", allow ? "enabled" : "disabled", t->name);
}

// /lag: what the game loop spent its slowest iteration of the last ~2 s on
static void cmdLag(CmdCtx& c) {
    char buf[300];
    c.s.lag.format(buf, sizeof(buf));
    c.replyf("aqua", "Slowest loop: %s", buf);
}

// /workers [n]: shows the background job pool, or resizes it (0 = run on the game loop)
static void cmdWorkers(CmdCtx& c) {
    if (c.argc >= 1) {
        int n = atoi(c.argv[0]);
        if (n < 0 || n > 8) {
            c.reply("Usage: /workers <0..8>", "red");
            return;
        }
        c.s.chunkJobs.setWorkers(n);
    }
    char buf[200];
    c.s.chunkJobs.statusLine(buf, sizeof(buf));
    const ChunkJobStats& st = c.s.chunkJobs.stats();
    c.replyf("aqua", "Jobs: %s | generated %u, decoded %u, sent %u (redone %u), light %u, saved %u, "
             "cancelled %u, promoted %u", buf, (unsigned)st.generated, (unsigned)st.decoded, (unsigned)st.sent,
             (unsigned)st.retried, (unsigned)st.lightResends, (unsigned)st.saved, (unsigned)st.cancelled,
             (unsigned)st.promoted);
}

static const Cmd COMMANDS[] = {
    {"help", false, "/help", "-", cmdHelp},
    {"list", false, "/list", "-", cmdList},
    {"msg", false, "/msg <player> <message>", "p", cmdMsg},
    {"tell", false, "/tell <player> <message>", "p", cmdMsg},
    {"w", false, "/w <player> <message>", "p", cmdMsg},
    {"me", false, "/me <action>", "-", cmdMe},
    {"seed", false, "/seed", "-", cmdSeed},
    {"spawn", false, "/spawn", "-", cmdSpawn},
    {"tps", false, "/tps", "-", cmdTps},
    {"storage", false, "/storage", "-", cmdStorage},
    {"workers", true, "/workers [count]", "-", cmdWorkers},
    {"lag", false, "/lag", "-", cmdLag},
    {"gamemode", true, "/gamemode <mode> [player]", "gp", cmdGamemode},
    {"tp", true, "/tp <x> <y> <z> | <player> [<player>]", "pxxx", cmdTp},
    {"teleport", true, "/teleport <x> <y> <z> | <player> [<player>]", "pxxx", cmdTp},
    {"give", true, "/give <player> <item> [count]", "pi", cmdGive},
    {"clear", true, "/clear [player]", "p", cmdClear},
    {"time", true, "/time <set|add|query> <value>", "t", cmdTime},
    {"weather", true, "/weather <clear|rain|thunder> [seconds]", "w", cmdWeather},
    {"kill", true, "/kill [player]", "p", cmdKill},
    {"setworldspawn", true, "/setworldspawn [x y z]", "xxx", cmdSetWorldSpawn},
    {"spawnpoint", true, "/spawnpoint [player]", "p", cmdSpawnPoint},
    {"say", true, "/say <message>", "-", cmdSay},
    {"difficulty", true, "/difficulty <peaceful|easy|normal|hard>", "d", cmdDifficulty},
    {"xp", true, "/xp add <player> <amount> [points|levels]", "-p", cmdXp},
    {"experience", true, "/experience add <player> <amount> [points|levels]", "-p", cmdXp},
    {"heal", true, "/heal [player]", "p", cmdHeal},
    {"feed", true, "/feed [player]", "p", cmdFeed},
    {"summon", true, "/summon <entity> [x y z]", "exxx", cmdSummon},
    {"setblock", true, "/setblock <x> <y> <z> <block>", "xxxb", cmdSetblock},
    {"fill", true, "/fill <x1> <y1> <z1> <x2> <y2> <z2> <block>", "xxxxxxb", cmdFill},
    {"op", true, "/op <player>", "p", cmdOp},
    {"deop", true, "/deop <player>", "p", cmdDeop},
    {"kick", true, "/kick <player> [reason]", "p", cmdKick},
    {"save-all", true, "/save-all", "-", cmdSave},
    {"stop", true, "/stop", "-", cmdStop},
    {"fly", true, "/fly [player]", "p", cmdFly},
};
static const int NUM_COMMANDS = sizeof(COMMANDS) / sizeof(COMMANDS[0]);

static void cmdHelp(CmdCtx& c) {
    c.reply("--- Commands ---", "gold");
    for (int i = 0; i < NUM_COMMANDS; i++) {
        if (COMMANDS[i].op && c.p && !c.p->op) continue;
        c.reply(COMMANDS[i].usage, "gray");
    }
}

void Server::runCommand(Player* p, const char* line) {
    char buf[257];
    snprintf(buf, sizeof(buf), "%s", line);
    char* argv[24];
    int argc = 0;
    for (char* tok = strtok(buf, " "); tok && argc < 24; tok = strtok(nullptr, " ")) argv[argc++] = tok;
    if (!argc) return;
    const char* name = argv[0];
    if (!strncmp(name, "minecraft:", 10)) name += 10;
    for (int i = 0; i < NUM_COMMANDS; i++) {
        if (strcmp(COMMANDS[i].name, name)) continue;
        if (COMMANDS[i].op && p && !p->op) {
            p->sendSystem("You do not have permission to use this command", "red");
            return;
        }
        CmdCtx c{*this, p, argv + 1, argc - 1};
        COMMANDS[i].fn(c);
        return;
    }
    if (p) p->sendSystem("Unknown command. Type \"/help\" for help.", "red");
    else MC_LOGW("unknown command: %s", name);
}

// Node layout: 0 root, then per command: literal (1+2i) and a greedy "args" argument (2+2i).
void Server::writeCommandTree(Writer& w) {
    w.varint(1 + NUM_COMMANDS * 2);
    w.u8(0);  // root
    w.varint(NUM_COMMANDS);
    for (int i = 0; i < NUM_COMMANDS; i++) w.varint(1 + 2 * i);
    for (int i = 0; i < NUM_COMMANDS; i++) {
        bool hasArgs = COMMANDS[i].hints[0] != '-' || strchr(COMMANDS[i].usage, '<') || strchr(COMMANDS[i].usage, '[');
        w.u8(0x01 | 0x04);  // literal, executable
        if (hasArgs) { w.varint(1); w.varint(2 + 2 * i); }
        else w.varint(0);
        w.string(COMMANDS[i].name);
        // argument node: brigadier:string greedy phrase, suggestions from the server
        w.u8(0x02 | 0x04 | 0x10);
        w.varint(0);
        w.string("args");
        w.string("brigadier:string");
        w.varint(2);
        w.string("minecraft:ask_server");
    }
    w.varint(0);
}

static void addMatch(char out[][40], int& n, int max, const char* s, const char* prefix) {
    if (n >= max) return;
    if (strncasecmp(s, prefix, strlen(prefix))) return;
    snprintf(out[n++], 40, "%s", s);
}

int Server::completions(Player& p, const char* text, char out[][40], int max, int& start) {
    const char* t = text[0] == '/' ? text + 1 : text;
    const char* lastSpace = strrchr(t, ' ');
    int n = 0;
    if (!lastSpace) {
        start = (int)(t - text);
        for (int i = 0; i < NUM_COMMANDS; i++)
            if (!COMMANDS[i].op || p.op) addMatch(out, n, max, COMMANDS[i].name, t);
        return n;
    }
    // which command / argument index
    char cmd[32];
    size_t cl = strcspn(t, " ");
    snprintf(cmd, sizeof(cmd), "%.*s", (int)(cl < 31 ? cl : 31), t);
    int argIdx = 0;
    for (const char* q = t + cl; q < lastSpace; q++)
        if (*q == ' ' && q[1] != ' ') argIdx++;
    const char* prefix = lastSpace + 1;
    start = (int)(prefix - text);
    const Cmd* c = nullptr;
    for (int i = 0; i < NUM_COMMANDS; i++)
        if (!strcmp(COMMANDS[i].name, cmd)) c = &COMMANDS[i];
    if (!c || (c->op && !p.op)) return 0;
    char hint = argIdx < (int)strlen(c->hints) ? c->hints[argIdx] : '-';
    if (c->fn == cmdGive && argIdx == 1) hint = 'i';
    if (c->fn == cmdXp && argIdx == 0) { addMatch(out, n, max, "add", prefix); return n; }
    switch (hint) {
        case 'p':
            for (int i = 0; i < MC_MAX_PLAYERS; i++)
                if (players[i].state == CS_PLAY) addMatch(out, n, max, players[i].name, prefix);
            if (c->fn == cmdGive) {  // "/give <item>" shorthand
                for (int i = 1; i < NUM_ITEMS && n < max; i++) addMatch(out, n, max, ITEMS[i].name, prefix);
            }
            break;
        case 'i':
            for (int i = 1; i < NUM_ITEMS && n < max; i++) addMatch(out, n, max, ITEMS[i].name, prefix);
            break;
        case 'b':
            for (int i = 0; i < NUM_BLOCKS && n < max; i++) addMatch(out, n, max, BLOCKS[i].name, prefix);
            break;
        case 'e': {
            static const char* mobs[] = {"pig", "cow", "sheep", "chicken", "zombie", "skeleton", "creeper", "spider"};
            for (const char* m : mobs) addMatch(out, n, max, m, prefix);
            break;
        }
        case 'g': {
            static const char* modes[] = {"survival", "creative", "adventure", "spectator"};
            for (const char* m : modes) addMatch(out, n, max, m, prefix);
            break;
        }
        case 't': {
            static const char* opts[] = {"set", "add", "query", "day", "noon", "night", "midnight"};
            for (const char* m : opts) addMatch(out, n, max, m, prefix);
            break;
        }
        case 'w': {
            static const char* opts[] = {"clear", "rain", "thunder"};
            for (const char* m : opts) addMatch(out, n, max, m, prefix);
            break;
        }
        case 'd': {
            static const char* opts[] = {"peaceful", "easy", "normal", "hard"};
            for (const char* m : opts) addMatch(out, n, max, m, prefix);
            break;
        }
        case 'x':
            addMatch(out, n, max, "~", prefix);
            break;
        default: break;
    }
    return n;
}

void Player::onTabComplete(Reader& r) {
    int32_t tid = r.varint();
    char text[257];
    r.string(text, sizeof(text));
    if (!r.ok()) return;
    char matches[32][40];
    int start = 0;
    int n = srv->completions(*this, text, matches, 32, start);
    Packet pk(pkt::s2c::TabComplete);
    pk.w.varint(tid);
    pk.w.varint(start);
    pk.w.varint((int32_t)strlen(text) - start);
    pk.w.varint(n);
    for (int i = 0; i < n; i++) {
        pk.w.string(matches[i]);
        pk.w.boolean(false);
    }
    conn.send(pk);
}

}  // namespace mc
