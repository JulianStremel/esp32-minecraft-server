// /menu: the operators' control panel as dialogs (1.21.6+): statistics, settings, the
// world (a new seed and world type, the dragon fight, dimensions, a full reset) and the
// players. Every page is an inline dialog (show_dialog); its buttons send
// custom_click_action with an id "esp32mc:<action>" and the form's values (and the
// button's own additions, such as the player a page is about) as an NBT payload, so the
// server keeps no menu state. Every action checks that the sender is an operator and
// runs the same code as the commands.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc/nbt.h"
#include "mc/platform.h"
#include "mc/registry.h"
#include "mc/server/server.h"

namespace mc {

static const char* const DIFFICULTY[] = {"peaceful", "easy", "normal", "hard"};
static const char* const WORLD_TYPES[] = {"normal", "flat", "void"};
static const char* const GAME_MODES[] = {"survival", "creative", "adventure", "spectator"};
static const char* const DIM_IDS[] = {"overworld", "the_nether", "the_end"};

namespace {

// Writes one inline dialog as network NBT (a compound without a name).
struct Dialog {
    NbtWriter n;
    explicit Dialog(Writer& w) : n(w) { w.u8(NBT_COMPOUND); }
    void text(const char* key, const char* s) { n.str(key, s); }   // a plain text component
    void coloured(const char* key, const char* s, const char* colour) {
        n.beginCompound(key);
        n.str("text", s);
        n.str("color", colour);
        n.end();
    }
    // body: plain messages
    void body(const char* const* lines, int count) {
        n.listHeader("body", NBT_COMPOUND, count);
        for (int i = 0; i < count; i++) {
            n.str("type", "minecraft:plain_message");
            n.str("contents", lines[i]);
            n.i32("width", 300);
            n.end();
        }
    }
    // a button that sends custom_click_action <id> with the form's values and additions
    // (pairs of key, value strings)
    void button(const char* label, const char* id, const char* tooltip = nullptr, int width = 150,
                const char* const* additions = nullptr, int nAdd = 0) {
        n.str("label", label);
        if (tooltip) n.str("tooltip", tooltip);
        n.i32("width", width);
        n.beginCompound("action");
        n.str("type", "minecraft:dynamic/custom");
        char full[64];
        snprintf(full, sizeof(full), "esp32mc:%s", id);
        n.str("id", full);
        if (nAdd) {
            n.beginCompound("additions");
            for (int i = 0; i + 1 < 2 * nAdd; i += 2) n.str(additions[i], additions[i + 1]);
            n.end();
        }
        n.end();
    }
    void end() { n.end(); }
};

// The values of a custom_click_action payload (a compound of strings, bytes, numbers).
struct Payload {
    struct Entry { char key[24]; char str[72]; double num; bool isNum; };
    Entry e[16];
    int n = 0;
    const Entry* find(const char* k) const {
        for (int i = 0; i < n; i++)
            if (!strcmp(e[i].key, k)) return &e[i];
        return nullptr;
    }
    const char* str(const char* k, const char* def = "") const {
        const Entry* x = find(k);
        return x && !x->isNum ? x->str : def;
    }
    double num(const char* k, double def = 0) const {
        const Entry* x = find(k);
        if (!x) return def;
        return x->isNum ? x->num : atof(x->str);
    }
    bool flag(const char* k) const {
        const Entry* x = find(k);
        if (!x) return false;
        return x->isNum ? x->num != 0 : !strcmp(x->str, "true");
    }
    // the payload tag (type byte, then a compound's entries); false if malformed
    bool parse(Reader& r) {
        uint8_t t = r.u8();
        if (!r.ok()) return false;
        if (t == NBT_END) return true;   // no payload
        if (t != NBT_COMPOUND) return nbtSkipPayload(r, t, 0) && r.ok();
        while (r.ok()) {
            uint8_t et = r.u8();
            if (et == NBT_END) break;
            uint16_t kl = r.u16();
            char key[64];
            if (kl >= sizeof(key)) return false;
            r.bytes((uint8_t*)key, kl);
            key[kl] = 0;
            Entry* x = n < 16 ? &e[n] : nullptr;
            if (x) {
                snprintf(x->key, sizeof(x->key), "%s", key);
                x->str[0] = 0;
                x->num = 0;
                x->isNum = true;
            }
            switch (et) {
                case NBT_BYTE: if (x) x->num = r.i8(); else r.i8(); break;
                case NBT_SHORT: if (x) x->num = r.i16(); else r.i16(); break;
                case NBT_INT: if (x) x->num = r.i32(); else r.i32(); break;
                case NBT_LONG: if (x) x->num = (double)r.i64(); else r.i64(); break;
                case NBT_FLOAT: if (x) x->num = r.f32(); else r.f32(); break;
                case NBT_DOUBLE: if (x) x->num = r.f64(); else r.f64(); break;
                case NBT_STRING: {
                    uint16_t l = r.u16();
                    char buf[256];
                    if (l >= sizeof(buf)) return false;
                    r.bytes((uint8_t*)buf, l);
                    buf[l] = 0;
                    if (x) {
                        x->isNum = false;
                        snprintf(x->str, sizeof(x->str), "%s", buf);
                    }
                    break;
                }
                default:
                    if (!nbtSkipPayload(r, et, 0)) return false;
                    x = nullptr;   // not kept
                    continue;
            }
            if (x) n++;
        }
        return r.ok();
    }
};

int indexOf(const char* const* list, int count, const char* s, int def) {
    for (int i = 0; i < count; i++)
        if (!strcmp(list[i], s)) return i;
    return def;
}

// Java's String#hashCode: vanilla's seed for a text that is not a number
int64_t textSeed(const char* s) {
    int32_t h = 0;
    for (const char* c = s; *c; c++) h = (int32_t)((uint32_t)h * 31u + (uint8_t)*c);
    return h;
}

uint64_t parseSeed(const char* s, uint64_t current) {
    while (*s == ' ') s++;
    if (!*s) return current;
    char* end = nullptr;
    long long v = strtoll(s, &end, 10);
    return (end && !*end) ? (uint64_t)v : (uint64_t)textSeed(s);
}

}  // namespace

// the server's state in a few lines
static int statsLines(Server& s, char (*out)[100], int max) {
    char status[640];
    s.statusLine(status, sizeof(status));
    int n = 0;
    for (const char* p = status; *p && n < max - 1;) {
        const char* bar = strstr(p, " | ");
        size_t len = bar ? (size_t)(bar - p) : strlen(p);
        if (len > 98) len = 98;
        memcpy(out[n], p, len);
        out[n][len] = 0;
        n++;
        p = bar ? bar + 3 : p + strlen(p);
    }
    int online = 0;
    for (int i = 0; i < MC_MAX_PLAYERS; i++) online += s.players[i].inPlay();
    snprintf(out[n++], 100, "%d players online, %d mobs, up %u min", online, s.mobCount(), (unsigned)(plat::millis() / 60000));
    return n;
}

void Server::showDialog(Player& p, const char* page, const char* arg) {
    if (!p.op) return;
    p.conn.sendStreamed([&](Writer& w) {
        w.varint(pkt::s2c::ShowDialog);
        w.varint(0);   // inline, not from the dialog registry
        Dialog d(w);
        d.n.i8("pause", 0);
        d.n.str("after_action", "close");
        if (!strcmp(page, "settings")) {
            d.text("type", "minecraft:multi_action");
            d.coloured("title", "Settings", "gold");
            d.n.listHeader("inputs", NBT_COMPOUND, 6);
            {   // difficulty
                d.n.str("type", "minecraft:single_option");
                d.n.str("key", "difficulty");
                d.text("label", "Difficulty");
                d.n.listHeader("options", NBT_COMPOUND, 4);
                for (int i = 0; i < 4; i++) {
                    d.n.str("id", DIFFICULTY[i]);
                    d.n.str("display", DIFFICULTY[i]);
                    d.n.i8("initial", i == (cfg.difficulty & 3));
                    d.n.end();
                }
                d.n.end();
            }
            const struct { const char* key; const char* label; bool on; } flags[] = {
                {"mobs", "Mob spawning", cfg.spawnMobs}, {"pvp", "PvP", cfg.pvp}, {"perfbar", "Performance banner", perfBar_}};
            for (const auto& f : flags) {
                d.n.str("type", "minecraft:boolean");
                d.n.str("key", f.key);
                d.text("label", f.label);
                d.n.i8("initial", f.on);
                d.n.end();
            }
            {   // time and weather: one-off changes
                static const char* const TIMES[] = {"keep", "day", "noon", "night", "midnight"};
                d.n.str("type", "minecraft:single_option");
                d.n.str("key", "time");
                char label[48];
                snprintf(label, sizeof(label), "Time (now %lld)", (long long)(meta.timeOfDay % 24000));
                d.text("label", label);
                d.n.listHeader("options", NBT_COMPOUND, 5);
                for (const char* t : TIMES) {
                    d.n.str("id", t);
                    d.n.str("display", t);
                    d.n.end();
                }
                d.n.end();
                static const char* const WEATHER[] = {"keep", "clear", "rain", "thunder"};
                d.n.str("type", "minecraft:single_option");
                d.n.str("key", "weather");
                d.text("label", meta.raining == 2 ? "Weather (now thunder)" : meta.raining ? "Weather (now rain)" : "Weather (now clear)");
                d.n.listHeader("options", NBT_COMPOUND, 4);
                for (const char* t : WEATHER) {
                    d.n.str("id", t);
                    d.n.str("display", t);
                    d.n.end();
                }
                d.n.end();
            }
            d.n.listHeader("actions", NBT_COMPOUND, 2);
            d.button("Apply", "settings_apply"); d.n.end();
            d.button("Back", "menu"); d.n.end();
            d.n.i32("columns", 2);
        } else if (!strcmp(page, "world")) {
            d.text("type", "minecraft:multi_action");
            d.coloured("title", "World", "gold");
            char l0[100], l1[100];
            snprintf(l0, sizeof(l0), "Seed %lld, %s world, spawn %d %d %d", (long long)meta.seed,
                     WORLD_TYPES[meta.worldType < 3 ? meta.worldType : 0], (int)meta.spawnX, (int)meta.spawnY, (int)meta.spawnZ);
            const DragonFight& f = wstate.dragon;
            int crystals = 0;
            for (int i = 0; i < 10; i++) crystals += (f.crystals >> i) & 1;
            snprintf(l1, sizeof(l1), "The dragon fight: %s, %d of 10 crystals",
                     f.state == DragonFight::DRAGON_ALIVE ? "the dragon is alive" : f.state == DragonFight::KILLED ? "the dragon was killed" : "not started",
                     crystals);
            const char* lines[] = {l0, l1};
            d.body(lines, 2);
            d.n.listHeader("inputs", NBT_COMPOUND, 2);
            {
                char seed[24];
                snprintf(seed, sizeof(seed), "%lld", (long long)meta.seed);
                d.n.str("type", "minecraft:text");
                d.n.str("key", "seed");
                d.text("label", "Seed of a new world (a number or any text)");
                d.n.str("initial", seed);
                d.n.i32("max_length", 64);
                d.n.i32("width", 300);
                d.n.end();
                d.n.str("type", "minecraft:single_option");
                d.n.str("key", "type");
                d.text("label", "World type");
                d.n.listHeader("options", NBT_COMPOUND, 3);
                for (int i = 0; i < 3; i++) {
                    d.n.str("id", WORLD_TYPES[i]);
                    d.n.str("display", WORLD_TYPES[i]);
                    d.n.i8("initial", i == meta.worldType);
                    d.n.end();
                }
                d.n.end();
            }
            d.n.listHeader("actions", NBT_COMPOUND, 8);
            d.button("Reset the world...", "world_reset_ask", "a new world with the seed and type above"); d.n.end();
            d.button("Set the world spawn here", "world_spawn"); d.n.end();
            d.button("Respawn the dragon", "dragon_respawn"); d.n.end();
            d.button("Reset the dragon fight", "dragon_reset", "as if never fought"); d.n.end();
            for (int k = 0; k < 3; k++) {
                static const char* const LABEL[] = {"Go to the overworld", "Go to the Nether", "Go to the End"};
                const char* add[] = {"dim", DIM_IDS[k]};
                d.button(LABEL[k], "goto", nullptr, 150, add, 1);
                d.n.end();
            }
            d.button("Back", "menu"); d.n.end();
            d.n.i32("columns", 2);
        } else if (!strcmp(page, "world_reset_ask")) {
            // arg: "<seed> <type>"
            char seed[72] = "", type[16] = "normal";
            if (arg) sscanf(arg, "%71s %15s", seed, type);
            d.text("type", "minecraft:confirmation");
            d.coloured("title", "Delete this world?", "red");
            char l0[160];
            snprintf(l0, sizeof(l0), "A new %s world with seed %s takes its place. All chunks, players and portals are deleted, and the server restarts.",
                     type, seed);
            const char* lines[] = {l0};
            d.body(lines, 1);
            const char* add[] = {"seed", seed, "type", type};
            d.n.beginCompound("yes");
            d.button("Yes, delete it", "world_reset", nullptr, 150, add, 2);
            d.n.end();
            d.n.beginCompound("no");
            d.button("No", "world");
            d.n.end();
        } else if (!strcmp(page, "players")) {
            d.text("type", "minecraft:multi_action");
            d.coloured("title", "Players", "gold");
            int count = 0;
            for (int i = 0; i < MC_MAX_PLAYERS; i++) count += players[i].inPlay();
            d.n.listHeader("actions", NBT_COMPOUND, count + 1);
            for (int i = 0; i < MC_MAX_PLAYERS; i++) {
                const Player& o = players[i];
                if (!o.inPlay()) continue;
                char tip[128];
                snprintf(tip, sizeof(tip), "%s%s, %.0f %.0f %.0f in %s, ping %d ms", GAME_MODES[o.gamemode & 3], o.op ? ", operator" : "",
                         o.e.x, o.e.y, o.e.z, dimensionName(o.e.dim), o.ping);
                const char* add[] = {"name", o.name};
                d.button(o.name, "player", tip, 150, add, 1);
                d.n.end();
            }
            d.button("Back", "menu"); d.n.end();
            d.n.i32("columns", 2);
        } else if (!strcmp(page, "player")) {
            Player* o = arg ? findPlayer(arg) : nullptr;
            d.text("type", "minecraft:multi_action");
            d.coloured("title", o ? o->name : "Player", "gold");
            if (o) {
                char l0[120];
                snprintf(l0, sizeof(l0), "%s, health %.0f, level %d, %.0f %.0f %.0f in %s", GAME_MODES[o->gamemode & 3], o->e.health,
                         o->xpLevel, o->e.x, o->e.y, o->e.z, dimensionName(o->e.dim));
                const char* lines[] = {l0};
                d.body(lines, 1);
                d.n.listHeader("inputs", NBT_COMPOUND, 1);
                d.n.str("type", "minecraft:single_option");
                d.n.str("key", "mode");
                d.text("label", "Game mode");
                d.n.listHeader("options", NBT_COMPOUND, 4);
                for (int i = 0; i < 4; i++) {
                    d.n.str("id", GAME_MODES[i]);
                    d.n.str("display", GAME_MODES[i]);
                    d.n.i8("initial", i == (o->gamemode & 3));
                    d.n.end();
                }
                d.n.end();   // the input
                const char* add[] = {"name", o->name};
                d.n.listHeader("actions", NBT_COMPOUND, 7);
                d.button("Set the game mode", "player_mode", nullptr, 150, add, 1); d.n.end();
                d.button("Teleport to them", "player_tp", nullptr, 150, add, 1); d.n.end();
                d.button("Bring them here", "player_bring", nullptr, 150, add, 1); d.n.end();
                d.button("Heal and feed", "player_heal", nullptr, 150, add, 1); d.n.end();
                d.button(o->op ? "Remove operator" : "Make operator", "player_op", nullptr, 150, add, 1); d.n.end();
                d.button("Kick", "player_kick", nullptr, 150, add, 1); d.n.end();
                d.button("Back", "players"); d.n.end();
            } else {
                const char* lines[] = {"That player is no longer online."};
                d.body(lines, 1);
                d.n.listHeader("actions", NBT_COMPOUND, 1);
                d.button("Back", "players"); d.n.end();
            }
            d.n.i32("columns", 2);
        } else {   // the main page
            d.text("type", "minecraft:multi_action");
            d.coloured("title", "Server menu", "gold");
            char stats[12][100];
            int n = statsLines(*this, stats, 12);
            const char* lines[12];
            for (int i = 0; i < n; i++) lines[i] = stats[i];
            d.body(lines, n);
            d.n.listHeader("actions", NBT_COMPOUND, 5);
            d.button("Settings", "settings", "difficulty, spawning, PvP, time, weather"); d.n.end();
            d.button("World", "world", "a new world, the dragon fight, dimensions"); d.n.end();
            d.button("Players", "players", "game mode, teleport, operator, kick"); d.n.end();
            d.button("Save the world now", "save"); d.n.end();
            d.button("Refresh", "menu"); d.n.end();
            d.n.i32("columns", 2);
        }
        d.n.beginCompound("exit_action");
        d.text("label", "Close");
        d.n.i32("width", 200);
        d.n.end();
        d.end();
    });
}

// custom_click_action: an identifier, then the payload (an optional NBT tag, length-
// prefixed in 1.21.8; read either way).
void Server::onCustomClickAction(Player& p, Reader& r) {
    char id[64];
    r.string(id, sizeof(id));
    if (!r.ok()) return;
    if (strncmp(id, "esp32mc:", 8)) return;   // not ours
    const char* action = id + 8;
    Payload pl;
    if (r.remaining() > 0) {
        Reader copy = r;   // try the length prefix; without one the tag starts right here
        int32_t len = copy.varint();
        if (copy.ok() && len >= 0 && (size_t)len == copy.remaining()) r = copy;
        if (!pl.parse(r)) return;
    }
    if (!p.op) {
        p.sendSystem("Only operators can use the menu", "red");
        return;
    }
    char cmd[128];
    auto run = [&](const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(cmd, sizeof(cmd), fmt, ap);
        va_end(ap);
        runCommand(&p, cmd);
    };
    const char* name = pl.str("name");
    if (!strcmp(action, "menu") || !strcmp(action, "settings") || !strcmp(action, "world") || !strcmp(action, "players")) {
        showDialog(p, action);
    } else if (!strcmp(action, "save")) {
        run("save-all");
        showDialog(p, "menu");
    } else if (!strcmp(action, "settings_apply")) {
        int diff = indexOf(DIFFICULTY, 4, pl.str("difficulty"), cfg.difficulty);
        if (diff != cfg.difficulty) run("difficulty %s", DIFFICULTY[diff]);
        cfg.spawnMobs = pl.flag("mobs");
        cfg.pvp = pl.flag("pvp");
        if (pl.flag("perfbar") != perfBar_) run(pl.flag("perfbar") ? "perfbar on" : "perfbar off");
        const char* t = pl.str("time", "keep");
        if (strcmp(t, "keep")) run("time set %s", t);
        const char* wth = pl.str("weather", "keep");
        if (strcmp(wth, "keep")) run("weather %s", wth);
        showDialog(p, "settings");
    } else if (!strcmp(action, "world_reset_ask")) {
        char arg[96];
        uint64_t seed = parseSeed(pl.str("seed"), meta.seed);
        snprintf(arg, sizeof(arg), "%lld %s", (long long)seed, WORLD_TYPES[indexOf(WORLD_TYPES, 3, pl.str("type"), 0)]);
        showDialog(p, "world_reset_ask", arg);
    } else if (!strcmp(action, "world_reset")) {
        uint64_t seed = parseSeed(pl.str("seed"), meta.seed);
        uint8_t type = (uint8_t)indexOf(WORLD_TYPES, 3, pl.str("type"), 0);
        char msg[160];
        snprintf(msg, sizeof(msg), "%s is resetting the world (seed %lld): the server restarts", p.name, (long long)seed);
        broadcastSystem(msg, "red");
        resetWorld(seed, type);
    } else if (!strcmp(action, "world_spawn")) {
        run("setworldspawn");
        showDialog(p, "world");
    } else if (!strcmp(action, "dragon_respawn") || !strcmp(action, "dragon_reset")) {
        run(!strcmp(action, "dragon_reset") ? "dragon reset" : "dragon respawn");
        showDialog(p, "world");
    } else if (!strcmp(action, "goto")) {
        int k = indexOf(DIM_IDS, 3, pl.str("dim"), -1);
        if (k >= 0) run("dimension %s", dimensionName((uint8_t)k));
    } else if (!strcmp(action, "player")) {
        showDialog(p, "player", name);
    } else if (!strncmp(action, "player_", 7)) {
        Player* o = findPlayer(name);
        if (!o) {
            showDialog(p, "players");
            return;
        }
        const char* what = action + 7;
        if (!strcmp(what, "mode")) {
            int gm = indexOf(GAME_MODES, 4, pl.str("mode"), o->gamemode);
            run("gamemode %s %s", GAME_MODES[gm], o->name);
        } else if (!strcmp(what, "tp")) {
            run("tp %s", o->name);
            return;   // the dialog closes; we are on our way
        } else if (!strcmp(what, "bring")) {
            run("tp %s %s", o->name, p.name);
        } else if (!strcmp(what, "heal")) {
            run("heal %s", o->name);
            run("feed %s", o->name);
        } else if (!strcmp(what, "op")) {
            run(o->op ? "deop %s" : "op %s", o->name);
        } else if (!strcmp(what, "kick")) {
            run("kick %s Kicked by an operator", o->name);
            showDialog(p, "players");
            return;
        }
        showDialog(p, "player", o->name);
    }
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
