// /menu: the operators' control panel, a chest window whose items are buttons (named,
// with their values in the tooltip): statistics, settings, players, the world (new
// seed, world type, the dragon fight, a full reset). On 1.16.5 this is the only UI a
// server can open; with 1.21.6+ dialogs it becomes a form (docs/MIGRATION_1_21_8.md).
// The actions run the same code as the commands.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc/platform.h"
#include "mc/registry.h"
#include "mc/server/server.h"

namespace mc {

enum MenuPage : uint8_t { MP_MAIN = 0, MP_SETTINGS, MP_WORLD, MP_PLAYERS, MP_PLAYER, MP_CONFIRM_RESET };
static const int MENU_SLOTS = 54;   // 6 rows
static const int SLOT_BACK = 45, SLOT_CLOSE = 53;

struct MenuEntry {   // plain data: cleared with memset
    uint16_t item;
    uint8_t count;
    char name[64];
    char lore[384];   // lines separated by '\n'
};

static void jsonText(char* out, size_t cap, const char* text, const char* color) {
    char esc[256];
    jsonEscape(text, esc, sizeof(esc));
    snprintf(out, cap, "{\"text\":\"%s\",\"italic\":false,\"color\":\"%s\"}", esc, color);
}

static void nbtString(Writer& w, const char* s) {
    size_t n = strlen(s);
    w.u16((uint16_t)n);
    w.bytes((const uint8_t*)s, n);
}

// A 1.16.5 slot with a display name and lore, its attributes hidden.
static void writeMenuSlot(Writer& w, const MenuEntry& e) {
    if (!e.item) {
        w.boolean(false);
        return;
    }
    w.boolean(true);
    w.varint(e.item);
    w.i8((int8_t)e.count);
    char json[512];
    w.u8(10); w.u16(0);                      // the root compound
    w.u8(10); nbtString(w, "display");
    jsonText(json, sizeof(json), e.name, "gold");
    w.u8(8); nbtString(w, "Name"); nbtString(w, json);
    int lines = 0;
    for (const char* p = e.lore; *p; p++) lines += *p == '\n';
    if (e.lore[0]) lines++;
    if (lines) {
        w.u8(9); nbtString(w, "Lore"); w.u8(8); w.i32(lines);
        const char* p = e.lore;
        while (*p) {
            const char* nl = strchr(p, '\n');
            char line[200];
            size_t n = nl ? (size_t)(nl - p) : strlen(p);
            if (n >= sizeof(line)) n = sizeof(line) - 1;
            memcpy(line, p, n);
            line[n] = 0;
            jsonText(json, sizeof(json), line, "gray");
            nbtString(w, json);
            p += n;
            if (*p == '\n') p++;
        }
    }
    w.u8(0);                                 // end of display
    w.u8(3); nbtString(w, "HideFlags"); w.i32(63);
    w.u8(0);                                 // end of root
}

static void entry(MenuEntry* m, int slot, uint16_t item, const char* name, const char* fmt, ...) {
    if (slot < 0 || slot >= MENU_SLOTS) return;
    MenuEntry& e = m[slot];
    e.item = item;
    e.count = 1;
    snprintf(e.name, sizeof(e.name), "%s", name);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.lore, sizeof(e.lore), fmt, ap);
    va_end(ap);
}

static const char* const DIFFICULTY[] = {"peaceful", "easy", "normal", "hard"};
static const char* const WORLD_TYPES[] = {"normal", "flat", "void"};
static const char* const GAME_MODES[] = {"survival", "creative", "adventure", "spectator"};

// the server's state in a few tooltip lines
static void statsLore(Server& s, char* out, size_t cap) {
    char status[640];
    s.statusLine(status, sizeof(status));
    // one line per " | " part, shortened
    size_t n = 0;
    for (const char* p = status; *p && n + 2 < cap;) {
        const char* bar = strstr(p, " | ");
        size_t len = bar ? (size_t)(bar - p) : strlen(p);
        if (len > 90) len = 90;
        if (n + len + 2 >= cap) break;
        memcpy(out + n, p, len);
        n += len;
        out[n++] = '\n';
        p = bar ? bar + 3 : p + strlen(p);
    }
    int online = 0;
    for (int i = 0; i < MC_MAX_PLAYERS; i++) online += s.players[i].inPlay();
    n += (size_t)snprintf(out + n, cap - n, "%d players online, %d mobs, uptime %u min", online, s.mobCount(),
                          (unsigned)(plat::millis() / 60000));
    out[n < cap ? n : cap - 1] = 0;
}

void Server::openMenu(Player& p, uint8_t page) {
    if (!p.op) return;
    MenuEntry* m = (MenuEntry*)plat::bigAlloc(sizeof(MenuEntry) * MENU_SLOTS);
    if (!m) return;
    memset(m, 0, sizeof(MenuEntry) * MENU_SLOTS);
    p.menuPage = page;
    char lore[384];
    switch (page) {
        case MP_MAIN:
            statsLore(*this, lore, sizeof(lore));
            entry(m, 13, itm::Clock, "Server statistics", "%s\n(click to refresh)", lore);
            entry(m, 29, itm::Comparator, "Settings", "difficulty, time, weather,\nspawning, the performance banner");
            entry(m, 31, itm::GrassBlock, "World", "seed %lld, %s\nnew world, the dragon fight, dimensions", (long long)meta.seed,
                  WORLD_TYPES[meta.worldType < 3 ? meta.worldType : 0]);
            entry(m, 33, itm::PlayerHead, "Players", "kick, op, game mode, teleport");
            entry(m, 49, itm::EnderChest, "Save the world now", "players, chunks and world data");
            break;
        case MP_SETTINGS:
            entry(m, 10, itm::IronSword, "Difficulty", "now: %s\nclick: next", DIFFICULTY[cfg.difficulty & 3]);
            entry(m, 11, itm::Spawner, "Mob spawning", "now: %s\nclick: switch", cfg.spawnMobs ? "on" : "off");
            entry(m, 12, itm::Shield, "PvP", "now: %s\nclick: switch", cfg.pvp ? "on" : "off");
            entry(m, 13, itm::Redstone, "Performance banner", "now: %s\nclick: switch", perfBar_ ? "on" : "off");
            entry(m, 19, itm::Sunflower, "Set day", "time %lld", (long long)(meta.timeOfDay % 24000));
            entry(m, 20, itm::BlackDye, "Set night", "time %lld", (long long)(meta.timeOfDay % 24000));
            entry(m, 22, itm::Feather, "Clear weather", "");
            entry(m, 23, itm::WaterBucket, "Rain", "");
            entry(m, 24, itm::Trident, "Thunderstorm", "");
            break;
        case MP_WORLD: {
            uint64_t seed = p.menuSeedSet ? p.menuSeed : meta.seed;
            entry(m, 10, itm::WheatSeeds, "Type a new seed", "in the chat (a number or any text)\nnow: %lld%s", (long long)seed,
                  p.menuSeedSet ? " (new)" : " (this world's)");
            entry(m, 11, itm::Bone, "Random seed", "");
            entry(m, 12, itm::Map, "World type", "now: %s\nclick: next", WORLD_TYPES[p.menuType < 3 ? p.menuType : 0]);
            entry(m, 14, itm::Tnt, "Reset the world...", "a new world with seed %lld (%s)\neveryone is disconnected,\nthis world is deleted",
                  (long long)seed, WORLD_TYPES[p.menuType < 3 ? p.menuType : 0]);
            static const char* const FIGHT[] = {"not started", "the dragon is alive", "the dragon was killed"};
            entry(m, 16, itm::DragonHead, "The dragon fight", "%s, %d of 10 crystals\nleft click: respawn the dragon\nright click: reset (as never fought)",
                  FIGHT[wstate.dragon.state < 3 ? wstate.dragon.state : 0], crystalsAlive());
            entry(m, 28, itm::GrassBlock, "Go to the overworld", "");
            entry(m, 29, itm::Netherrack, "Go to the Nether", "");
            entry(m, 30, itm::EndStone, "Go to the End", "");
            entry(m, 32, itm::Compass, "Set the world spawn here", "now %d %d %d", (int)meta.spawnX, (int)meta.spawnY, (int)meta.spawnZ);
            break;
        }
        case MP_PLAYERS: {
            int slot = 10;
            for (int i = 0; i < MC_MAX_PLAYERS && slot < 44; i++) {
                Player& o = players[i];
                if (!o.inPlay()) continue;
                entry(m, slot, itm::PlayerHead, o.name, "%s, %s%s\n%.0f %.0f %.0f in %s\nping %d ms", GAME_MODES[o.gamemode & 3],
                      o.op ? "operator, " : "", o.dead ? "dead" : "alive", o.e.x, o.e.y, o.e.z, dimensionName(o.e.dim), o.ping);
                slot++;
                if (slot % 9 == 8) slot += 2;
            }
            break;
        }
        case MP_PLAYER: {
            Player* o = menuTarget(p);
            if (!o) {
                plat::bigFree(m);
                openMenu(p, MP_PLAYERS);
                return;
            }
            snprintf(lore, sizeof(lore), "%s", o->name);
            entry(m, 4, itm::PlayerHead, lore, "%s, health %.0f, level %d", GAME_MODES[o->gamemode & 3], o->e.health, o->xpLevel);
            entry(m, 19, itm::EnderPearl, "Teleport to them", "");
            entry(m, 20, itm::Lead, "Bring them here", "");
            entry(m, 21, itm::DiamondSword, "Game mode", "now: %s\nclick: next", GAME_MODES[o->gamemode & 3]);
            entry(m, 22, itm::GoldenApple, "Heal and feed", "");
            entry(m, 23, o->op ? itm::GoldIngot : itm::Paper, o->op ? "Remove operator" : "Make operator", "");
            entry(m, 25, itm::Barrier, "Kick", "");
            break;
        }
        case MP_CONFIRM_RESET: {
            uint64_t seed = p.menuSeedSet ? p.menuSeed : meta.seed;
            entry(m, 4, itm::Tnt, "Delete this world?", "a new world: seed %lld, %s\nall chunks, players and portals are deleted;\nthe server restarts",
                  (long long)seed, WORLD_TYPES[p.menuType < 3 ? p.menuType : 0]);
            entry(m, 20, itm::LimeWool, "Yes, delete it and start the new world", "");
            entry(m, 24, itm::RedWool, "No", "");
            break;
        }
    }
    if (page != MP_MAIN) entry(m, SLOT_BACK, itm::Arrow, "Back", "");
    entry(m, SLOT_CLOSE, itm::Barrier, "Close", "");
    // the window: open it once, then refresh its contents
    if (p.winKind != WK_MENU) {
        closeWindow(p, true);
        p.winKind = WK_MENU;
        p.winId = p.nextWinId;
        p.nextWinId = (int8_t)(p.nextWinId % 100 + 1);
        Packet pk(pkt::s2c::OpenWindow);
        pk.w.varint(p.winId);
        pk.w.varint(5);   // generic_9x6
        pk.w.string("{\"text\":\"Server menu\",\"color\":\"dark_blue\"}");
        p.conn.send(pk);
    }
    Packet pk(pkt::s2c::WindowItems);
    pk.w.u8((uint8_t)p.winId);
    pk.w.i16((int16_t)(MENU_SLOTS + 36));
    for (int i = 0; i < MENU_SLOTS; i++) writeMenuSlot(pk.w, m[i]);
    for (int i = 0; i < 36; i++) writeSlot(pk.w, p.inv[SLOT_MAIN_START + i]);
    p.conn.send(pk);
    Packet cur(pkt::s2c::SetSlot);   // nothing on the cursor
    cur.w.i8(-1);
    cur.w.i16(-1);
    writeSlot(cur.w, ItemStack());
    p.conn.send(cur);
    plat::bigFree(m);
}

Player* Server::menuTarget(Player& p) {
    if (p.menuTarget < 0 || p.menuTarget >= MC_MAX_PLAYERS) return nullptr;
    Player& o = players[p.menuTarget];
    return o.inPlay() && o.session == p.menuTargetSession ? &o : nullptr;
}

// Java's String#hashCode: vanilla's seed for a text that is not a number
static int64_t textSeed(const char* s) {
    int32_t h = 0;
    for (const char* c = s; *c; c++) h = (int32_t)((uint32_t)h * 31u + (uint8_t)*c);
    return h;
}

bool Server::menuChat(Player& p, const char* msg) {
    if (!p.menuInput) return false;
    p.menuInput = 0;
    if (!strcmp(msg, "cancel")) {
        p.sendSystem("No new seed", "gray");
        return true;
    }
    char* end = nullptr;
    long long v = strtoll(msg, &end, 10);
    p.menuSeed = (end && !*end && end != msg) ? (uint64_t)v : (uint64_t)textSeed(msg);
    p.menuSeedSet = true;
    char line[96];
    snprintf(line, sizeof(line), "New seed: %lld", (long long)p.menuSeed);
    p.sendSystem(line, "gray");
    openMenu(p, MP_WORLD);
    return true;
}

void Server::menuClick(Player& p, int slot, int button) {
    if (!p.op) {
        closeWindow(p, true);
        return;
    }
    char cmd[96];
    auto run = [&](const char* text) { runCommand(&p, text); };
    auto run1 = [&](const char* fmt, const char* a) {
        snprintf(cmd, sizeof(cmd), fmt, a);
        runCommand(&p, cmd);
    };
    auto run2 = [&](const char* fmt, const char* a, const char* b) {
        snprintf(cmd, sizeof(cmd), fmt, a, b);
        runCommand(&p, cmd);
    };
    uint8_t page = p.menuPage;
    if (slot == SLOT_CLOSE) {
        closeWindow(p, true);
        return;
    }
    if (slot == SLOT_BACK && page != MP_MAIN) {
        openMenu(p, page == MP_PLAYER ? MP_PLAYERS : (page == MP_CONFIRM_RESET ? MP_WORLD : MP_MAIN));
        return;
    }
    switch (page) {
        case MP_MAIN:
            if (slot == 29) page = MP_SETTINGS;
            else if (slot == 31) { page = MP_WORLD; if (!p.menuSeedSet) p.menuType = meta.worldType; }
            else if (slot == 33) page = MP_PLAYERS;
            else if (slot == 49) run("save-all");
            break;
        case MP_SETTINGS:
            if (slot == 10) run1("difficulty %s", DIFFICULTY[(cfg.difficulty + 1) & 3]);
            else if (slot == 11) cfg.spawnMobs = !cfg.spawnMobs;
            else if (slot == 12) cfg.pvp = !cfg.pvp;
            else if (slot == 13) run(perfBar_ ? "perfbar off" : "perfbar on");
            else if (slot == 19) run("time set day");
            else if (slot == 20) run("time set night");
            else if (slot == 22) run("weather clear");
            else if (slot == 23) run("weather rain");
            else if (slot == 24) run("weather thunder");
            break;
        case MP_WORLD:
            if (slot == 10) {
                p.menuInput = 1;
                closeWindow(p, true);
                p.sendSystem("Type the new world's seed in the chat (a number or any text), or 'cancel'", "yellow");
                return;
            }
            if (slot == 11) {
                p.menuSeed = (uint64_t)plat::random32() << 32 | plat::random32();
                p.menuSeedSet = true;
            } else if (slot == 12) {
                p.menuType = (uint8_t)((p.menuType + 1) % 3);
            } else if (slot == 14) {
                page = MP_CONFIRM_RESET;
            } else if (slot == 16) {
                run(button == 1 ? "dragon reset" : "dragon respawn");
            } else if (slot >= 28 && slot <= 30) {
                closeWindow(p, true);
                run1("dimension %s", dimensionName((uint8_t)(slot - 28)));
                return;
            } else if (slot == 32) {
                run("setworldspawn");
            }
            break;
        case MP_PLAYERS: {
            if (slot < 10 || slot > 43 || slot % 9 == 0 || slot % 9 == 8) break;
            int index = (slot / 9 - 1) * 7 + (slot % 9 - 1);
            for (int i = 0, k = 0; i < MC_MAX_PLAYERS; i++) {
                if (!players[i].inPlay()) continue;
                if (k++ == index) {
                    p.menuTarget = (int8_t)i;
                    p.menuTargetSession = players[i].session;
                    page = MP_PLAYER;
                }
            }
            break;
        }
        case MP_PLAYER: {
            Player* o = menuTarget(p);
            if (!o) { page = MP_PLAYERS; break; }
            if (slot == 19) { closeWindow(p, true); run1("tp %s", o->name); return; }
            if (slot == 20) run2("tp %s %s", o->name, p.name);
            else if (slot == 21) run2("gamemode %s %s", GAME_MODES[(o->gamemode + 1) & 3], o->name);
            else if (slot == 22) { run1("heal %s", o->name); run1("feed %s", o->name); }
            else if (slot == 23) run1(o->op ? "deop %s" : "op %s", o->name);
            else if (slot == 25) { run1("kick %s Kicked by an operator", o->name); page = MP_PLAYERS; }
            break;
        }
        case MP_CONFIRM_RESET:
            if (slot == 20) {
                uint64_t seed = p.menuSeedSet ? p.menuSeed : meta.seed;
                char msg[160];
                snprintf(msg, sizeof(msg), "%s is resetting the world (seed %lld): the server restarts", p.name, (long long)seed);
                broadcastSystem(msg, "red");
                resetWorld(seed, p.menuType);
                return;
            }
            if (slot == 24) page = MP_WORLD;
            break;
    }
    if (p.winKind == WK_MENU && p.inPlay()) openMenu(p, page);
}

// Deletes this world and restarts the server into a new one (the storage keeps the
// new seed and type; the spawn is found when it starts).
void Server::resetWorld(uint64_t seed, uint8_t type) {
    MC_LOGW("resetting the world: seed %lld, type %d", (long long)seed, (int)type);
    for (int i = 0; i < MC_MAX_PLAYERS; i++)
        if (players[i].state != CS_FREE && players[i].conn.open()) players[i].kick("The world is being reset; join again in a moment");
    chunkJobs.drain();
    storageIo.drain();
    WorldMeta& m = meta;
    int32_t radius = m.radius;
    m.reset();
    m.seed = seed;
    m.worldType = type <= WORLD_VOID ? type : (uint8_t)WORLD_NORMAL;
    m.generatorVersion = GENERATOR_LATEST;
    m.radius = radius;
    m.spawnY = SPAWN_PENDING;   // found when the new world starts
    if (storage && !storage->resetWorld(m)) MC_LOGE("the world could not be reset (storage)");
    restartRequested_ = true;
    running_ = false;
}

}  // namespace mc
