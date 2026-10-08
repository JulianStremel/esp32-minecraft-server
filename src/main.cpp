// ESP-IDF firmware entry point for ESP32-S3 (WiFi) and ESP32-P4 (Ethernet).
#include <cstdio>
#include <cstring>
#include "firmware_config.h"
#include "network.h"
#include "sd_storage.h"
#include "serial_console.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mc/bench.h"
#include "mc/server/server.h"
#include "mc/storage/nbd_device.h"
#include "mc/storage/world_store.h"

static mc::Server* g_server = nullptr;
// emulator and QEMU builds keep their serial ports untouched
#if !defined(MC_EMULATOR) && !defined(MC_QEMU_CAPTURE)
#define MC_SERIAL_CONSOLE 1
#endif
#ifdef MC_CPU_PROFILE
void startCpuProfile();
#endif

// The game loop. It sleeps in select() until a player's socket has data (or room for
// pending output), a worker finished urgent work, or the 50 ms tick timer fires.
static void serverTask(void*) {
    g_server->setTickSource(mc::plat::createTickTimer(mc::Server::TICK_MS));  // notifies this task
    uint32_t lastSleep = mc::plat::millis();
#if defined(MC_EMULATOR)
    uint32_t lastStat = mc::plat::millis();
#endif
    for (;;) {
        g_server->loop();
        if (!g_server->running() && g_server->restartRequested()) {   // a world reset
            printf("restarting\n");
            vTaskDelay(pdMS_TO_TICKS(500));   // let the kick messages go out
            esp_restart();
        }
#ifdef MC_SERIAL_CONSOLE
        char line[128];
        while (serialConsoleLine(line, sizeof(line))) g_server->runCommand(nullptr, line[0] == '/' ? line + 1 : line);
#endif
#if defined(MC_EMULATOR)
        if (mc::plat::millis() - lastStat > 10000) {
            lastStat = mc::plat::millis();
            char line[640];
            g_server->statusLine(line, sizeof(line));
            printf("[stat] %s | min free heap %u KB\n", line, (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT) / 1024));
        }
#endif
        uint32_t timeout = g_server->waitTimeoutMs();
        if (timeout > 0) {
            mc::plat::waitForWork(timeout);
            lastSleep = mc::plat::millis();
        } else if (mc::plat::millis() - lastSleep > 100) {
            // busy for 100 ms without a pause (catching up late ticks): let the worker
            // on this core run for a moment
            vTaskDelay(1);
            lastSleep = mc::plat::millis();
        } else {
            mc::plat::waitForWork(0);   // poll: input that arrived meanwhile
        }
    }
}

#if defined(MC_BENCH)
// Benchmark build (tools/emulator/run.sh --bench): measures the CPU cost of the chunk
// pipeline on this device, prints it and stops. Runs pinned to core 1 like the server.
static void benchTask(void*) {
    mc::runChunkBench(4, [](const char* line) { puts(line); });
    puts("[bench] done"); fflush(stdout);
    vTaskDelete(nullptr);
}
#endif

static void halt(const char* why) {
    for (;;) {
        printf("[FATAL] %s\n", why);
        mc::plat::delayMs(5000);
    }
}

extern "C" void app_main() {
    // Keep serial markers immediately visible to automated tests and instrumentation.
    setvbuf(stdout, nullptr, _IONBF, 0);
    puts("ESP-IDF Minecraft server (protocol 754 / 1.16.5)");
    size_t psram = esp_psram_is_initialized() ? esp_psram_get_size() : 0;
    printf("PSRAM: %u KB, internal heap: %u KB\n", (unsigned)(psram / 1024),
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    if (psram < (size_t)MC_MIN_PSRAM_MB * 1024 * 1024 * 9 / 10)
        halt("insufficient PSRAM for the selected board profile");
#if defined(MC_BENCH)
    if (xTaskCreatePinnedToCore(benchTask, "bench", 24576, nullptr, 3, nullptr, 1) != pdPASS)
        halt("cannot create benchmark task");
    return;
#endif
#ifdef MC_SERIAL_CONSOLE
    serialConsoleStart();
#endif
    networkStart();

    mc::ServerConfig cfg;
    cfg.port = MC_PORT;
    cfg.motd = MC_MOTD;
    cfg.maxPlayers = MC_MAX_ONLINE;
    cfg.viewDistance = MC_VIEW_DISTANCE;
    cfg.defaultGameMode = MC_GAMEMODE;
    cfg.difficulty = MC_DIFFICULTY;
    cfg.pvp = MC_PVP;
    cfg.spawnMobs = MC_SPAWN_MOBS;
    cfg.ops = MC_OPS;
    cfg.whitelist = MC_WHITELIST;
    cfg.seed = MC_SEED;
    cfg.worldType = (mc::WorldType)MC_WORLD_TYPE;
    cfg.worldRadiusChunks = MC_WORLD_RADIUS;
    // chunks live in PSRAM: generated terrain needs ~12 KB per chunk (flat worlds ~3 KB);
    // keep about half of the PSRAM for chunks and the rest for buffers and growth
    cfg.chunkCacheSize = (int)(psram / 2 / (20 * 1024));
    if (cfg.chunkCacheSize > 400) cfg.chunkCacheSize = 400;
    cfg.simulationDistance = 3;
    cfg.maxMobs = 24;
    cfg.chunksPerTick = 4;
    cfg.minFreeHeapKb = 512;  // free heap includes PSRAM

    mc::WorldStore* store = nullptr;
#if defined(SD_CARD) && SD_CARD
    {
        bool created = false;
        mc::BlockDevice* dev = sdStorageOpen(created);
        if (!dev) halt("SD_CARD is set but there is no usable card (see above); insert a FAT32 card or set SD_CARD 0");
        store = new mc::WorldStore(dev);
        mc::StoreParams sp;
        sp.radius = MC_WORLD_RADIUS;
        // a file this firmware just created may be formatted; an existing one only if blank
        if (!store->open(sp, created)) halt("cannot open the world on the SD card (see log above)");
    }
#else
    if (strlen(NBD_HOST) > 0) {
        mc::NbdDevice* nbd = new mc::NbdDevice(NBD_HOST, NBD_PORT, NBD_EXPORT);
        while (!nbd->connect()) {
            printf("waiting for NBD server %s:%d ...\n", NBD_HOST, NBD_PORT);
            mc::plat::delayMs(3000);
        }
        store = new mc::WorldStore(nbd);
        mc::StoreParams sp;
        sp.radius = MC_WORLD_RADIUS;
        // never format an export that holds something other than a blank device or our world
        if (!store->open(sp, false)) halt("cannot open the world on the NBD export (see log above)");
    } else {
        puts("no NBD_HOST configured: the world will not be saved");
    }
#endif

    g_server = new mc::Server();
    if (!g_server->begin(cfg, store)) halt("server failed to start");
    printf("free heap after start: %u KB\n", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024));
    // dedicated task with a large stack, on the application core
    if (xTaskCreatePinnedToCore(serverTask, "minecraft", 24576, nullptr, 3, nullptr, 1) != pdPASS)
        halt("cannot create server task");
#ifdef MC_CPU_PROFILE
    startCpuProfile();
#endif
}
