// PC build of the server: same core as the ESP32 firmware, POSIX sockets.
// Useful for development, testing and as a reference NBD storage client.
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "mc/server/server.h"
#include "mc/storage/nbd_device.h"
#include "mc/storage/world_store.h"

using namespace mc;

static volatile bool g_stop = false;
static void onSignal(int) { g_stop = true; }

static void usage() {
    fprintf(stderr,
            "usage: mcserver [options]\n"
            "  --port N            listen port (default 25565)\n"
            "  --seed N            world seed for a new world\n"
            "  --flat | --void     world type for a new world\n"
            "  --generator N       terrain generator version for a new world (default: newest)\n"
            "  --view N            view distance (chunks)\n"
            "  --radius N          world border radius in chunks (new worlds)\n"
            "  --ops a,b           operator names\n"
            "  --creative          default game mode creative\n"
            "  --peaceful          difficulty peaceful\n"
            "  --no-mobs           disable mob spawning\n"
            "  --compression N     packet compression threshold (-1 = off)\n"
            "  --max-players N\n"
            "  --workers N         worker threads for chunk work (default 2, 0 = on the game loop)\n"
            "storage (pick one; default: none, the world is not saved):\n"
            "  --nbd HOST[:PORT][/EXPORT]   network block device (e.g. tools/nbd_server.py, nbdkit, qemu-nbd)\n"
            "  --file PATH [--size MB]      local file (sparse), default size 1024 MB\n"
            "  --mem MB                     RAM device (lost on exit; for testing)\n"
            "  --no-store-compression       store chunks uncompressed\n"
            "  --format                     allow formatting a device that holds unknown data\n");
}

int main(int argc, char** argv) {
    ServerConfig cfg;
    cfg.motd = "ESP32 Minecraft server (PC build)";
    const char* nbd = nullptr;
    const char* file = nullptr;
    long sizeMb = 1024, memMb = 0;
    bool storeCompress = true, allowFormat = false;
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
        else if (!strcmp(a, "--generator")) {
            int v = atoi(next());
            if (v < 1 || v > GENERATOR_LATEST) {
                fprintf(stderr, "--generator: version 1 to %d\n", GENERATOR_LATEST);
                return 2;
            }
            cfg.generatorVersion = (uint8_t)v;
        }
        else if (!strcmp(a, "--view")) cfg.viewDistance = atoi(next());
        else if (!strcmp(a, "--radius")) cfg.worldRadiusChunks = atoi(next());
        else if (!strcmp(a, "--ops")) cfg.ops = next();
        else if (!strcmp(a, "--creative")) cfg.defaultGameMode = GM_CREATIVE;
        else if (!strcmp(a, "--peaceful")) cfg.difficulty = 0;
        else if (!strcmp(a, "--no-mobs")) cfg.spawnMobs = false;
        else if (!strcmp(a, "--compression")) cfg.compressionThreshold = atoi(next());
        else if (!strcmp(a, "--max-players")) cfg.maxPlayers = atoi(next());
        else if (!strcmp(a, "--workers")) cfg.workerThreads = atoi(next());
        else if (!strcmp(a, "--nbd")) nbd = next();
        else if (!strcmp(a, "--file")) file = next();
        else if (!strcmp(a, "--size")) sizeMb = atol(next());
        else if (!strcmp(a, "--mem")) memMb = atol(next());
        else if (!strcmp(a, "--no-store-compression")) storeCompress = false;
        else if (!strcmp(a, "--format")) allowFormat = true;
        else { usage(); return 2; }
    }
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    BlockDevice* dev = nullptr;
    if (nbd) {
        char host[128];
        snprintf(host, sizeof(host), "%s", nbd);
        const char* exportName = "";
        char* slash = strchr(host, '/');
        if (slash) { *slash = 0; exportName = slash + 1; }
        uint16_t port = 10809;
        char* colon = strchr(host, ':');
        if (colon) { *colon = 0; port = (uint16_t)atoi(colon + 1); }
        NbdDevice* nd = new NbdDevice(host, port, exportName);
        for (int attempt = 0; !nd->connect(); attempt++) {
            if (attempt >= 30 || g_stop) { fprintf(stderr, "cannot reach the NBD server\n"); return 1; }
            sleep(1);
        }
        dev = nd;
    } else if (file) {
        FileDevice* fd = new FileDevice(file, (uint64_t)sizeMb << 20);
        if (!fd->ok()) { fprintf(stderr, "cannot open %s\n", file); return 1; }
        dev = fd;
    } else if (memMb) {
        dev = new MemDevice((size_t)memMb << 20);
    }
    WorldStore* store = nullptr;
    if (dev) {
        store = new WorldStore(dev);
        StoreParams sp;
        sp.radius = cfg.worldRadiusChunks;
        sp.compress = storeCompress;
        if (!store->open(sp, allowFormat)) { fprintf(stderr, "cannot open world storage\n"); return 1; }
    }
    static Server server;
    if (!server.begin(cfg, store)) return 1;
    char line[256];
    size_t lineLen = 0;
    bool console = true;   // until stdin closes
    while (!g_stop && server.running()) {
        server.loop();
        // sleep until a socket, a finished urgent job, the console or the next tick needs us
        plat::waitForWork(server.waitTimeoutMs(), console);
        // console commands on stdin
        pollfd pfd = {0, POLLIN, 0};
        while (console && poll(&pfd, 1, 0) > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
            char ch;
            if (read(0, &ch, 1) != 1) {
                console = false;
                break;
            }
            if (ch == '\n') {
                line[lineLen] = 0;
                if (lineLen) server.runCommand(nullptr, line[0] == '/' ? line + 1 : line);
                lineLen = 0;
            } else if (lineLen < sizeof(line) - 1) {
                line[lineLen++] = ch;
            }
        }
    }
    if (server.running()) server.shutdown("Server closed");
    if (server.restartRequested()) {   // a world reset: start again as this same program
        fprintf(stderr, "restarting\n");
        fflush(nullptr);
        for (int fd = 3; fd < 1024; fd++) close(fd);   // the listening socket, the storage
        execv("/proc/self/exe", argv);
        perror("restart");
        return 1;
    }
    return 0;
}
