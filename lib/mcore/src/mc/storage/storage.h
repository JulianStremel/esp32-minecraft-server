// Storage interface used by the server: world metadata, player data and chunks.
// Implementations: WorldStore over any BlockDevice (NBD network block device,
// file, RAM) -- see world_store.h.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "mc/item.h"
#include "mc/world/world.h"

namespace mc {

struct WorldMeta {
    uint64_t seed = 0;
    uint8_t worldType = 0;
    int32_t radius = 64;
    int32_t spawnX = 0, spawnY = 64, spawnZ = 0;
    int64_t worldAge = 0;
    int64_t timeOfDay = 1000;
    uint8_t raining = 0;
    int32_t weatherTimer = 12000;
};

struct PlayerData {
    uint8_t uuid[16] = {0};
    char name[17] = {0};
    double x = 0, y = 0, z = 0;
    float yaw = 0, pitch = 0;
    uint8_t gamemode = 0;
    float health = 20;
    uint8_t food = 20;
    float saturation = 5;
    int32_t xpLevel = 0;
    float xpProgress = 0;
    int32_t xpTotal = 0;
    uint8_t held = 0;
    bool hasSpawn = false;
    int32_t spawnX = 0, spawnY = 0, spawnZ = 0;
    ItemStack inv[46];
};

class Storage : public ChunkStore {
public:
    // true if the storage holds a world (meta filled in); false for a blank device.
    virtual bool loadMeta(WorldMeta& m) = 0;
    virtual bool saveMeta(const WorldMeta& m) = 0;
    virtual bool loadPlayer(const uint8_t uuid[16], PlayerData& out) = 0;
    virtual bool savePlayer(const PlayerData& p) = 0;
    virtual bool flush() = 0;
    virtual bool flushLater() { return flush(); }   // without waiting (see BlockDevice)
    virtual void statusLine(char* buf, size_t cap) = 0;
    // World border radius imposed by the storage layout (-1: no constraint).
    virtual int worldRadius() const { return -1; }
};

}  // namespace mc
