// ESP32 firmware entry point: WiFi, NBD world storage and the Minecraft server task.
#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <config.h>
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
#if defined(BOARD_HAS_PSRAM)
    if (psramFound()) Serial.printf("PSRAM: %u KB\n", (unsigned)(ESP.getPsramSize() / 1024));
#endif

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
#if defined(BOARD_HAS_PSRAM)
    bool bigMemory = psramFound();
#else
    bool bigMemory = false;
#endif
    // resident chunk budget: generated terrain needs ~12 KB per chunk (flat worlds ~3 KB)
    cfg.chunkCacheSize = bigMemory ? 240 : 10;
    cfg.simulationDistance = bigMemory ? 3 : 1;
    cfg.maxMobs = bigMemory ? 24 : 6;
    cfg.chunksPerTick = bigMemory ? 4 : 2;
    cfg.minFreeHeapKb = bigMemory ? 64 : 40;  // keep headroom for WiFi/lwIP buffers

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
    static uint32_t lastCheck = 0;
    if (millis() - lastCheck > 10000) {
        lastCheck = millis();
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("WiFi lost, reconnecting");
            WiFi.reconnect();
        }
    }
    delay(500);
}
