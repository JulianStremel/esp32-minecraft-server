// Runtime configuration of the server (filled in by the platform's main()).
#pragma once
#include <stdint.h>
#include "mc/world/generator.h"

namespace mc {

enum GameMode : uint8_t { GM_SURVIVAL = 0, GM_CREATIVE = 1, GM_ADVENTURE = 2, GM_SPECTATOR = 3 };

struct ServerConfig {
    uint16_t port = 25565;
    const char* motd = "A Minecraft server running on an ESP32";
    int maxPlayers = 8;               // <= MC_MAX_PLAYERS
    int viewDistance = 6;             // chunks, <= MC_MAX_VIEW_DISTANCE
    int simulationDistance = 3;       // chunks kept resident around players (entities, ticks)
    int chunkCacheSize = 160;         // resident chunks (soft limit)
    int chunksPerTick = 6;            // chunk packets per tick and player
    int tickBudgetMs = 40;            // stop streaming chunks when a tick took this long
    int exactLightDistance = 2;       // chunks this close to a player get light computed with
                                      // their neighbours' blocks (exact across borders); -1 = never
    int compressionThreshold = 256;   // -1 disables packet compression
    int workerThreads = 2;            // threads for chunk generation / light / compression
                                      // (one per core on the ESP32); 0 = all on the game loop

    // world (used when the storage holds no world yet)
    uint64_t seed = 0;                // 0 = pick a random seed
    WorldType worldType = WORLD_NORMAL;
    uint8_t generatorVersion = GENERATOR_LATEST;   // for a new world; a stored world keeps its own
    int worldRadiusChunks = 64;       // world border radius (also sizes the storage layout)

    uint8_t defaultGameMode = GM_SURVIVAL;
    uint8_t difficulty = 2;           // 0 peaceful .. 3 hard
    bool pvp = true;
    bool spawnMobs = true;
    int maxMobs = 24;
    const char* ops = "";             // comma separated player names with operator rights
    const char* whitelist = "";       // comma separated names; empty = everyone may join

    int minFreeHeapKb = 0;            // > 0: stop loading chunks / evict when free heap drops below
    int autosaveSeconds = 60;
    int keepAliveTimeoutMs = 30000;
};

}  // namespace mc
