// The game server: owns the world, players and entities and runs the 20 Hz tick.
// Call loop() as often as possible from one task (the game loop); compute-heavy chunk
// work runs on worker threads (chunkJobs), which never touch this state directly.
#pragma once
#include <stdint.h>
#include "mc/limits.h"
#include "mc/net/connection.h"
#include "mc/server/chunk_jobs.h"
#include "mc/server/config.h"
#include "mc/server/entity.h"
#include "mc/server/player.h"
#include "mc/storage/storage.h"
#include "mc/world/generator.h"
#include "mc/world/world.h"

namespace mc {

enum DamageCause : uint8_t {
    DC_GENERIC = 0, DC_FALL, DC_VOID, DC_DROWN, DC_LAVA, DC_FIRE, DC_STARVE, DC_ATTACK, DC_ARROW,
    DC_EXPLOSION, DC_KILL, DC_CACTUS, DC_SUFFOCATE
};

// Where the game loop spent the slowest loop() call of the last ~2 s (see /lag).
struct LagProfile {
    enum Part {
        // (prefixed: Arduino #defines INPUT and OUTPUT)
        P_JOBS, P_INPUT, P_PLAYERS, P_STREAM, P_BLOCKS, P_ENTITIES, P_SPAWN, P_TRACK, P_LIGHT, P_SAVE, P_EVICT,
        P_OTHER, P_OUTPUT,
        P_SNAPSHOT, P_FETCH,   // parts of the above: chunk snapshots for jobs, storage lookups
        PARTS
    };
    uint16_t ms[PARTS];
    uint16_t total = 0;
    uint16_t socketWait = 0;     // part of the above spent waiting for full sockets
    uint16_t finishMs = 0;       // slowest job finish() in it
    const char* finishKind = "-";
    uint16_t lockMs = 0;         // longest wait for the job queue lock
    LagProfile() { clear(); }
    void clear() {
        for (auto& m : ms) m = 0;
        total = socketWait = finishMs = lockMs = 0;
        finishKind = "-";
    }
    static const char* name(int part);
    void format(char* buf, size_t cap) const;
};

struct ScheduledTick {
    int32_t x, z;
    int16_t y;
    uint32_t due;
};

class Server : public WorldListener, public ChunkPinner {
public:
    Server();
    ~Server();

    // storage may be nullptr (nothing is persisted then).
    bool begin(const ServerConfig& config, Storage* storage);
    void loop();
    void saveAll(bool flushStorage);
    void shutdown(const char* reason);
    bool running() const { return running_; }
    bool memoryLow() const;

    ServerConfig cfg;
    Storage* storage = nullptr;
    Generator gen;
    World world;
    ChunkJobs chunkJobs;
    WorldMeta meta;
    Player players[MC_MAX_PLAYERS];
    Entity entities[MC_MAX_ENTITIES];
    uint32_t ticks = 0;
    float tps = 20;
    float msptAvg = 0;
    uint32_t tickMaxMs = 0;      // longest tick in the last ~2 s
    uint32_t stallMaxMs = 0;     // longest loop() call in the last ~2 s (anything that blocks the loop)
    LagProfile lag;              // breakdown of that call

    LagProfile& lagNow() { return lagCur_; }   // the loop() call being measured

    // ---- world listener / pinning
    void onBlockChanged(int x, int y, int z, uint16_t oldState, uint16_t newState) override;
    void onChunkEvicted(Chunk& c) override;
    void onChunkLoaded(int cx, int cz) override { chunkJobs.onSyncLoad(cx, cz); }
    bool isChunkPinned(int cx, int cz) override;

    // ---- messaging (server.cpp)
    void broadcast(const Packet& p, const Player* except = nullptr);
    void broadcastNear(const Packet& p, int cx, int cz, const Player* except = nullptr);
    void broadcastSystem(const char* text, const char* color = nullptr);
    void broadcastChat(const char* json, uint8_t position = 0);
    Player* findPlayer(const char* name);
    Player* playerByEntity(int32_t id);
    int onlineCount() const;
    int32_t newEntityId() { return nextEntityId_++; }
    bool isOp(const char* name) const;
    bool isWhitelisted(const char* name) const;
    void sendPlayerInfoAdd(Player* to, const Player& p);   // to == nullptr: everyone
    void sendPlayerInfoRemove(const Player& p);
    void sendTabHeader(Player& p);
    void savePlayer(Player& p);
    void statusLine(char* buf, size_t cap);

    // ---- entities (entities.cpp)
    Entity* spawnEntity(uint8_t kind, uint16_t type, double x, double y, double z);
    Entity* findEntity(int32_t id);              // players included
    void removeEntity(Entity& e);
    Entity* dropItem(double x, double y, double z, const ItemStack& st, bool scatter = true);
    void throwItem(Player& p, const ItemStack& st);
    Entity* spawnMob(uint16_t type, double x, double y, double z);
    int mobCount() const;
    void tickEntities();
    void trackEntities();
    void forgetEntities(Player& p);
    void sendSpawn(Player& to, Entity& e);
    void sendDestroy(Player& to, int32_t id);
    void writeMetadata(Writer& w, const Entity& e, bool full);
    void broadcastMetadata(Entity& e);
    void broadcastEquipment(Player& p);
    void broadcastAnimation(Entity& e, uint8_t anim, const Player* except);
    void broadcastStatus(Entity& e, int8_t status);
    void attack(Player& attacker, Entity& target);
    void damageEntity(Entity& e, float amount, uint8_t cause, int32_t attackerId);
    void explode(double x, double y, double z, float power, int32_t source);
    void playSound(const char* name, double x, double y, double z, float volume = 1, float pitch = 1, int category = 0);

    // ---- blocks (blocks.cpp)
    uint16_t blockAt(int x, int y, int z) { return world.getBlock(x, y, z); }
    void setBlock(int x, int y, int z, uint16_t state);      // + neighbour updates
    void breakBlock(int x, int y, int z, Player* by, bool drops);
    void updateNeighbors(int x, int y, int z);
    void scheduleTick(int x, int y, int z, int delay);
    void tickBlocks();
    void randomTickBlock(int x, int y, int z, uint16_t state);
    void interactBlock(Player& p, int x, int y, int z, uint16_t state, bool& handled);
    uint16_t placementState(Player& p, uint16_t block, int x, int y, int z, int face, float cx, float cy, float cz);
    bool canSupport(uint16_t state, int x, int y, int z);
    float digTicks(Player& p, uint16_t state);

    // ---- survival (survival.cpp)
    void damagePlayer(Player& p, float amount, uint8_t cause, int32_t attacker);
    void killPlayer(Player& p, uint8_t cause, int32_t attacker);
    void respawnPlayer(Player& p);
    void tickSurvival(Player& p);
    void addExhaustion(Player& p, float amount);
    void giveXp(Player& p, int points);
    void heal(Player& p, float amount);
    void finishUsingItem(Player& p);

    // ---- inventory (inventory.cpp)
    int giveItem(Player& p, ItemStack st);     // returns count that did not fit
    void openContainer(Player& p, int x, int y, int z);
    void openCrafting(Player& p, int x, int y, int z);
    void openFurnace(Player& p, int x, int y, int z);
    void closeWindow(Player& p, bool sendClose);
    void tickFurnaces();
    void containerChanged(int x, int y, int z);
    void damageHeldItem(Player& p, int amount);
    void consumeHeld(Player& p, int amount = 1);

    // ---- commands (commands.cpp)
    void runCommand(Player* p, const char* line);
    void writeCommandTree(Writer& w);
    int completions(Player& p, const char* text, char out[][40], int max, int& start);

    // ---- chunk helpers (chunks.cpp)
    bool resendLight(int cx, int cz);   // false: workers busy, retry later

private:
    void acceptConnections();
    void pollPlayers();
    void tick();
    void tickPlayers();
    void tickTime();
    void tickWeather();
    void tickMobSpawning();
    void autosave();
    void flushLightQueue();
    void processScheduledTicks();
    void randomTicks();
    void tickFluid(int x, int y, int z, uint16_t state);

    Listener* listener_ = nullptr;
    bool running_ = false;
    int32_t nextEntityId_ = 1000;
    uint32_t nextTickMs_ = 0;
    uint32_t lastTpsMs_ = 0;
    uint32_t tpsTicks_ = 0;
    uint32_t tickMaxWin_ = 0, stallMaxWin_ = 0;
    LagProfile lagCur_, lagWin_;
    uint32_t lastSaveMs_ = 0;
    bool saving_ = false;
    uint32_t lastStatusMs_ = 0;

    // light resend queue (chunks whose lighting changed)
    int32_t lightQ_[32][2];
    int lightQLen_ = 0;
    // scheduled block ticks (fluids, buttons, ...)
    ScheduledTick sched_[MC_SCHED_TICKS];
    int schedLen_ = 0;
};

// JSON text helpers (text.cpp)
size_t jsonEscape(const char* in, char* out, size_t cap);
void textJson(char* out, size_t cap, const char* text, const char* color = nullptr);

}  // namespace mc
