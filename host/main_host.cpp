// PC build of the server: same core as the ESP32 firmware, POSIX sockets.
// Useful for development, testing and as a reference NBD storage client.
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "mc/server/server.h"

using namespace mc;

static volatile bool g_stop = false;
static void onSignal(int) { g_stop = true; }

static void usage() {
    fprintf(stderr,
            "usage: mcserver [options]\n"
            "  --port N            listen port (default 25565)\n"
            "  --seed N            world seed for a new world\n"
            "  --flat | --void     world type for a new world\n"
            "  --view N            view distance (chunks)\n"
            "  --radius N          world border radius in chunks (new worlds)\n"
            "  --ops a,b           operator names\n"
            "  --creative          default game mode creative\n"
            "  --peaceful          difficulty peaceful\n"
            "  --no-mobs           disable mob spawning\n"
            "  --compression N     packet compression threshold (-1 = off)\n"
            "  --max-players N\n");
}

int main(int argc, char** argv) {
    ServerConfig cfg;
    cfg.motd = "ESP32 Minecraft server (PC build)";
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) { usage(); exit(2); }
            return argv[++i];
        };
        if (!strcmp(a, "--port")) cfg.port = (uint16_t)atoi(next());
        else if (!strcmp(a, "--seed")) cfg.seed = strtoull(next(), nullptr, 10);
        else if (!strcmp(a, "--flat")) cfg.worldType = WORLD_FLAT;
        else if (!strcmp(a, "--void")) cfg.worldType = WORLD_VOID;
        else if (!strcmp(a, "--view")) cfg.viewDistance = atoi(next());
        else if (!strcmp(a, "--radius")) cfg.worldRadiusChunks = atoi(next());
        else if (!strcmp(a, "--ops")) cfg.ops = next();
        else if (!strcmp(a, "--creative")) cfg.defaultGameMode = GM_CREATIVE;
        else if (!strcmp(a, "--peaceful")) cfg.difficulty = 0;
        else if (!strcmp(a, "--no-mobs")) cfg.spawnMobs = false;
        else if (!strcmp(a, "--compression")) cfg.compressionThreshold = atoi(next());
        else if (!strcmp(a, "--max-players")) cfg.maxPlayers = atoi(next());
        else { usage(); return 2; }
    }
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    static Server server;
    if (!server.begin(cfg, nullptr)) return 1;
    char line[256];
    size_t lineLen = 0;
    while (!g_stop && server.running()) {
        server.loop();
        // console commands on stdin
        pollfd pfd = {0, POLLIN, 0};
        if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
            char ch;
            ssize_t r = read(0, &ch, 1);
            if (r == 1) {
                if (ch == '\n') {
                    line[lineLen] = 0;
                    if (lineLen) server.runCommand(nullptr, line[0] == '/' ? line + 1 : line);
                    lineLen = 0;
                } else if (lineLen < sizeof(line) - 1) {
                    line[lineLen++] = ch;
                }
            }
        }
        usleep(1000);
    }
    if (server.running()) server.shutdown("Server closed");
    return 0;
}
