// ESP32 firmware entry point: WiFi, NBD world storage and the Minecraft server task.
#include <Arduino.h>
#if defined(MC_QEMU)
// Running in Espressif's QEMU (see tools/qemu): Ethernet instead of WiFi, fixed config.
#include "config_qemu.h"
#include "qemu_eth.h"
#else
#include <ESPmDNS.h>
#include <WiFi.h>
#include <config.h>
#endif
#include "mc/server/server.h"
#include "mc/storage/nbd_device.h"
#include "mc/storage/world_store.h"

// note: no `using namespace mc` -- Arduino defines its own `Server` class
static mc::Server* g_server = nullptr;

static void serverTask(void*) {
    for (;;) {
        g_server->loop();
        vTaskDelay(1);  // let WiFi / idle tasks run (and feed the watchdog)
    }
}

static void halt(const char* why) {
    for (;;) {
        Serial.printf("[FATAL] %s\n", why);
        delay(5000);
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("ESP32 Minecraft server (protocol 754 / 1.16.5)");
    // PSRAM is mandatory (supported boards have at least 8 MB)
    size_t psram = psramFound() ? ESP.getPsramSize() : 0;
    Serial.printf("PSRAM: %u KB, internal heap: %u KB\n", (unsigned)(psram / 1024), (unsigned)(ESP.getFreeHeap() / 1024));
    if (psram < (size_t)MC_MIN_PSRAM_MB * 1024 * 1024 * 9 / 10)
        halt("this firmware needs a board with at least 8 MB of PSRAM (see README)");

#if defined(MC_QEMU)
    char ip[16] = "?";
    Serial.println("QEMU build: starting emulated OpenCores Ethernet (DHCP from QEMU user networking)");
    if (qemu_eth_start(ip, sizeof(ip)) != 0) halt("no network: start QEMU with -nic user,model=open_eth");
    Serial.printf("IP address: %s (reach it through QEMU's hostfwd)\n", ip);
#else
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(MC_HOSTNAME);
    WiFi.setSleep(false);          // modem sleep adds 100+ ms latency
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.printf("\nIP address: %s\n", WiFi.localIP().toString().c_str());
    if (MDNS.begin(MC_HOSTNAME)) MDNS.addService("minecraft", "tcp", MC_PORT);
#endif

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
    if (strlen(NBD_HOST) > 0) {
        mc::NbdDevice* nbd = new mc::NbdDevice(NBD_HOST, NBD_PORT, NBD_EXPORT);
        while (!nbd->connect()) {
            Serial.printf("waiting for NBD server %s:%d ...\n", NBD_HOST, NBD_PORT);
            delay(3000);
        }
        store = new mc::WorldStore(nbd);
        mc::StoreParams sp;
        sp.radius = MC_WORLD_RADIUS;
        // never format an export that holds something other than a blank device or our world
        if (!store->open(sp, false)) halt("cannot open the world on the NBD export (see log above)");
    } else {
        Serial.println("no NBD_HOST configured: the world will not be saved");
    }

    g_server = new mc::Server();
    if (!g_server->begin(cfg, store)) halt("server failed to start");
    Serial.printf("free heap after start: %u KB\n", (unsigned)(ESP.getFreeHeap() / 1024));
    // dedicated task with a large stack, on the application core
    xTaskCreatePinnedToCore(serverTask, "minecraft", 24576, nullptr, 3, nullptr, 1);
}

void loop() {
#if defined(MC_QEMU)
    static uint32_t lastStat = 0;
    if (millis() - lastStat > 10000 && g_server) {
        lastStat = millis();
        char line[256];
        g_server->statusLine(line, sizeof(line));
        Serial.printf("[stat] %s | min free heap %u KB\n", line, (unsigned)(ESP.getMinFreeHeap() / 1024));
    }
    delay(500);
#else
    static uint32_t lastCheck = 0;
    if (millis() - lastCheck > 10000) {
        lastCheck = millis();
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("WiFi lost, reconnecting");
            WiFi.reconnect();
        }
    }
    delay(500);
#endif
}
