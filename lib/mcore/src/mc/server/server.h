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
#include "mc/server/world_state.h"
#include "mc/server/redstone.h"
#include "mc/storage/storage_io.h"
#include "mc/tick_pacer.h"
#include "mc/timer_wheel.h"
#include "mc/world/generator.h"
#include "mc/world/world.h"

namespace mc {

// Vibration frequencies of vanilla's game events (VibrationSystem#getGameEventFrequency).
enum : uint8_t {
    GE_STEP = 1, GE_PROJECTILE_LAND = 2, GE_SHOOT = 3, GE_ENTITY_ACTION = 4, GE_ENTITY_DAMAGE = 7, GE_EAT = 8,
    GE_CLOSE = 9,   // containers, doors, levers and plates switching off
    GE_OPEN = 10,   // ... opening, switching on; note blocks, fuses
    GE_BLOCK_CHANGE = 11, GE_BLOCK_DESTROY = 12, GE_BLOCK_PLACE = 13, GE_ENTITY_PLACE = 14,
    GE_EXPLODE = 15   // also an entity dying
};

enum DamageCause : uint8_t {
    DC_GENERIC = 0, DC_FALL, DC_VOID, DC_DROWN, DC_LAVA, DC_FIRE, DC_STARVE, DC_ATTACK, DC_ARROW,
    DC_EXPLOSION, DC_KILL, DC_CACTUS, DC_SUFFOCATE, DC_FIREBALL, DC_MAGIC
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

// Mob timers in the timer wheel (TimerKey::entity(id, timer)).
enum EntityTimer : uint16_t {
    ET_DESPAWN = 1,   // items, arrows, falling blocks reach their age limit
    ET_FUSE = 2,      // a creeper's fuse burns down
    ET_CORPSE = 3,    // a dead mob's body disappears
    ET_CALM = 4,      // a passive mob stops fleeing
    ET_WANDER = 5,    // an idle mob picks a new place to walk to
};

class SpawnJob;
class PathJob;
class Dashboard;

// Mob spawning rules after vanilla 1.16.5 (spawning.cpp), exposed for the unit tests.
int skyDarkening(int64_t timeOfDay, bool raining, bool thundering);
bool darkEnoughForMonster(int sky, int block, int darkening, bool thundering, Rng& r);
bool brightEnoughForAnimal(int sky, int block);

// "overworld", "the_nether", "the_end"; and back (also "nether", "end"), -1 if unknown
const char* dimensionName(uint8_t dim);
int parseDimension(const char* s);

// newer_blocks.cpp: which of them take random ticks, their drops (true: handled) and
// support (-1: not one of them, else whether the block may stay)
bool newerRandomTicking(uint16_t blockId);
class Server;
class Player;
bool newerBlockDrops(Server& s, Player* by, int x, int y, int z, uint16_t st);
int newerBlockSupported(Server& s, uint16_t st, int x, int y, int z);

class Server : public WorldListener, public ChunkPinner {
public:
    static constexpr uint32_t TICK_MS = 50;
    Server();
    ~Server();

    // storage may be nullptr (nothing is persisted then).
    bool begin(const ServerConfig& config, Storage* storage);
    // One pass of the game loop: packets, due ticks, job results, output. Call it again
    // after waiting (plat::waitForWork) for at most waitTimeoutMs().
    void loop();
    uint32_t waitTimeoutMs();
    // Where tick periods come from (default: plat::millis()); not owned.
    void setTickSource(TickSource* src);
    void saveAll(bool flushStorage); // blocking shutdown barrier
    bool requestSave(Player* requester); // asynchronous /save-all
    bool deferEvictionSave(Chunk& c) override {
        if (!storage || !storage->splitIo() || c.readOnly || !storage->chunkInRange(c.cx,c.cz)) return false;
        chunkJobs.saveChunk(c);
        return true;
    }
    void shutdown(const char* reason);
    bool running() const { return running_; }
    bool memoryLow() const;

    ServerConfig cfg;
    Storage* storage = nullptr;
    StorageIo storageIo;
    Generator gen;                 // the overworld's
    Generator netherGen, endGen;
    Generator& generatorOf(uint8_t d) { return d == DIM_NETHER ? netherGen : d == DIM_END ? endGen : gen; }
    World world;
    uint64_t blockEntitySequence = 0;
    // The dimension the game loop works in at the moment: a player's actions, an
    // entity's tick, a scheduled block tick. The block wrappers (blockAt, setBlock, ...),
    // broadcastNear and new entities use it. InDim sets it for a scope.
    uint8_t curDim = DIM_OVERWORLD;
    struct InDim {
        Server& s;
        uint8_t prev;
        InDim(Server& srv, uint8_t d) : s(srv), prev(srv.curDim) { s.curDim = d; }
        ~InDim() { s.curDim = prev; }
    };
    ChunkJobs chunkJobs;
    Dashboard* dashboard = nullptr;   // MC_DASHBOARD builds with cfg.dashboardPort set
    WorldMeta meta;
    Player players[MC_MAX_PLAYERS];
    Entity entities[MC_MAX_ENTITIES];
    uint32_t ticks = 0;
    float tps = 20;
    float msptAvg = 0;
    uint32_t tickMaxMs = 0;      // longest tick in the last ~2 s
    uint32_t stallMaxMs = 0;     // longest loop() call in the last ~2 s (anything that blocks the loop)
    LagProfile lag;              // breakdown of that call
    // game loop health in the last ~2 s window
    float wakeupsPerS = 0;       // loop() calls per second
    uint32_t overruns = 0;       // times a tick was due while an earlier one was still due
    uint32_t lateTicks = 0;      // ticks that ran late (caught up)
    uint32_t skippedTicks = 0;   // ticks dropped because the loop fell too far behind
    plat::WaitStats waits;       // why the loop's waits ended (main loops that wait)

    LagProfile& lagNow() { return lagCur_; }   // the loop() call being measured

    // ---- world listener / pinning
    void onBlockChanged(uint8_t dim, int x, int y, int z, uint16_t oldState, uint16_t newState) override;
    void onBlockUpdated(uint8_t dim, int x, int y, int z, uint16_t oldState, uint16_t newState, uint8_t flags) override;
    void onChunkEvicted(Chunk& c) override;
    void onChunkLoaded(uint8_t dim, int cx, int cz) override { chunkJobs.onSyncLoad(dim, cx, cz); }
    void onChunkReady(Chunk& c) override;
    void onChunkSaving(Chunk& c) override {
        prepareChunkSave(c);
        attachTicks(c);
        c.hadEntities = attachEntities(c) > 0;
    }
    void beforeEviction(Chunk& c) override;   // its entities go into it (saved_entities.cpp)
    // before a chunk is snapshotted for saving: furnace progress brought up to date
    void prepareChunkSave(Chunk& live);
    // the chunk's pending block ticks (delays relative to now) into `target`
    void attachTicks(Chunk& target);
    // ---- saved entities (saved_entities.cpp)
    // copies of the entities in the game that are in `target`'s chunk; returns how many
    // the stored copy will hold (with the stashed ones)
    int attachEntities(Chunk& target);
    bool stash(Entity& e);                   // into its chunk (false: not resident)
    void stashFarEntities();                 // and back near players (every second)
    Entity* restoreEntity(const SavedEntity& s, uint8_t dim);
    void markEntityChunksDirty();            // before a full save
    struct { uint32_t stashed = 0, unstashed = 0; } entityStats;
    bool isChunkPinned(uint8_t dim, int cx, int cz) override;

    // ---- messaging (server.cpp)
    void broadcast(const Packet& p, const Player* except = nullptr);
    // to the players in curDim (or dim) who have chunk (cx, cz)
    void broadcastNear(const Packet& p, int cx, int cz, const Player* except = nullptr) {
        broadcastNearIn(curDim, p, cx, cz, except);
    }
    void broadcastNearIn(uint8_t dim, const Packet& p, int cx, int cz, const Player* except = nullptr);
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
    void sendWeather(Player* to);   // meta.raining: 0 clear, 1 rain, 2 thunder; to == nullptr: everyone
    // Live performance banner: boss bars with TPS, tick time and memory (/perfbar).
    static constexpr int PERF_BARS = 2;
    bool perfBar() const { return perfBar_; }
    void setPerfBar(bool on);
    void sendPerfBarAdd(Player& p);   // to a joining player while it is on
    void savePlayer(Player& p);
    void statusLine(char* buf, size_t cap);

    // ---- entities (entities.cpp)
    Entity* spawnEntity(uint8_t kind, uint16_t type, double x, double y, double z);
    Entity* findEntity(int32_t id);              // players included
    void removeEntity(Entity& e);
    Entity* dropItem(double x, double y, double z, const ItemStack& st, bool scatter = true);
    void throwItem(Player& p, const ItemStack& st);
    Entity* spawnMob(uint16_t type, double x, double y, double z);
    void spawnFinished(SpawnJob& j);   // spawning.cpp
    // path finding for mobs (mob_paths.cpp): a direction towards (tx, ty, tz) along a
    // path, or false while there is none yet (then the mob steers straight)
    bool pathDirection(Entity& e, double tx, double ty, double tz, double& mx, double& mz, bool& jump);
    void pathFinished(PathJob& j);
    uint32_t pathVersionsAround(int cx, int cz);
    struct { uint32_t jobs = 0, reached = 0, nodes = 0; uint64_t us = 0; int inFlight = 0; } pathStats;
    struct { uint32_t jobs = 0, spawned = 0; uint64_t us = 0; } spawnStats;
    int mobCount() const;
    void tickEntities();
    void tickBlockEntities();
    void trackEntities();
    void forgetEntities(Player& p);
    void sendSpawn(Player& to, Entity& e);
    void sendDestroy(Player& to, int32_t id);
    void writeMetadata(Writer& w, const Entity& e, bool full);
    void broadcastMetadata(Entity& e);
    void broadcastEquipment(Player& p);
    void broadcastAnimation(Entity& e, uint8_t anim, const Player* except);
    void broadcastHurt(Entity& e);
    void broadcastStatus(Entity& e, int8_t status);
    void attack(Player& attacker, Entity& target);
    void damageEntity(Entity& e, float amount, uint8_t cause, int32_t attackerId);
    // fire: as vanilla's explosions with fire (ghast fireballs): a third of the spots it
    // cleared that have ground below catch fire
    void explode(double x, double y, double z, float power, int32_t source, bool fire = false);
    Entity* primeTnt(int x, int y, int z, int32_t owner = -1, bool chain = false);
    // ---- Nether mobs (nether_mobs.cpp)
    void spawnInNether();   // spawning.cpp
    void tickGhast(Entity& e);
    void tickMagmaCube(Entity& e);
    void tickFireball(Entity& f);
    void setMagmaCubeSize(Entity& e, uint8_t size);
    void splitMagmaCube(Entity& e);
    void angerPiglins(Entity& victim, int32_t attackerId);
    Entity* shootFireball(Entity& shooter, double x, double y, double z, double dx, double dy, double dz);
    void deflectFireball(Entity& f, Player& p);
    // ---- the End's dragon fight (dragon.cpp)
    void tickDragonFight();
    void startDragonFight();
    void resetDragonFight(bool asNew);   // /dragon respawn | reset
    // ---- the operator menu (menu.cpp): dialogs
    // page: "menu" (statistics and the sections), "settings", "world", "world_reset_ask"
    // (arg: "<seed> <type>"), "players", "player" (arg: the name)
    void showDialog(Player& p, const char* page, const char* arg = nullptr);
    // ---- sleeping (sleep.cpp)
    const char* trySleep(Player& p, int headX, int y, int headZ);   // nullptr: asleep, else why not
    void wakeUp(Player& p);
    void announceSleepers();
    void tickSleep();                    // the night passes once everyone slept
    // ---- blocks of 1.17 to 1.21 (newer_blocks.cpp): copper, candles, amethyst
    bool useItemOnNewerBlock(Player& p, int x, int y, int z, uint16_t st, ItemStack& it);   // true: done
    bool interactNewerBlock(Player& p, int x, int y, int z, uint16_t st);                  // true: done
    void randomTickNewerBlock(int x, int y, int z, uint16_t st);
    void onCustomClickAction(Player& p, Reader& r);   // a dialog's button
    // Deletes the world and restarts the server into a new one with this seed and type.
    void resetWorld(uint64_t seed, uint8_t type);
    bool restartRequested() const { return restartRequested_; }
    void finishDragonFight(Entity& d);
    void placeExitPortal(bool active);
    void tickDragon(Entity& d);
    float dragonDamage(Entity& d, float amount, uint8_t cause);
    void dragonHurt(Entity& d, float healthBefore);
    Entity* dragon();
    Entity* dragonByPart(int32_t id, int& part);
    int crystalsAlive() const;
    void hitCrystal(Entity& c, int32_t by);
    void tickCrystal(Entity& c);
    Entity* breathCloud(double x, double y, double z, float radius, int duration, int32_t owner);
    void tickCloud(Entity& c);
    void writeCloudMetadata(Writer& w, const Entity& c);
    void sendBossBar(Player& p, int action);   // 0 show, 1 remove, 2 health
    int dragonPart_ = -1;            // the body part a player's hit landed on (onUseEntity)
    void reserveEntityIds(int n) { nextEntityId_ += n; }
    void playSound(const char* name, double x, double y, double z, float volume = 1, float pitch = 1, int category = 0);

    // ---- blocks (blocks.cpp)
    uint16_t blockAt(int x, int y, int z) { return world.getBlock(curDim, x, y, z); }
    void setBlock(int x, int y, int z, uint16_t state);      // + neighbour updates
    // ticks between fluid flow steps (lava is faster in the Nether)
    int fluidDelay(uint16_t blockId) const;
    void breakBlock(int x, int y, int z, Player* by, bool drops);
    void updateNeighbors(int x, int y, int z);
    // Schedules a tick for the block now at (x, y, z) (vanilla: Level#getBlockTicks().scheduleTick).
    // Ignored if one is already pending for that block there.
    void scheduleTick(int x, int y, int z, int delay, int8_t prio = 0);
    bool willTickThisTick(int x, int y, int z, uint16_t id) const;
    bool handlingBlockTicks() const { return timerIndex_ < timerCount_; }
    void scheduleEntityTimer(const Entity& e, uint16_t timer, int delay);
    void cancelEntityTimers(const Entity& e);
    uint32_t worldTick() const { return (uint32_t)meta.worldAge; }
    Redstone redstone;
    TimerWheel timers;   // scheduled block ticks, furnaces and mob timers, keyed by world age
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
    // ---- dimensions (dimensions.cpp)
    // Respawn packet for p.e.dim, then the view and the known entities start over
    void sendRespawn(Player& p);
    void writeSpawnInfo(Writer& w, const Player& p);   // Join Game / Respawn: the player's dimension
    void resendPlayerState(Player& p);   // what a Respawn packet resets, after the teleport
    void changeDimension(Player& p, uint8_t dim, double x, double y, double z, float yaw, float pitch);
    // where p arrives in dim (builds a platform when there is no room); false if the
    // chunks are unavailable
    bool arrivalSpot(Player& p, uint8_t dim, double& x, double& y, double& z, float& yaw);
    // Moves p to dim: at once if the arrival's chunks are resident, else once they are
    // (p.travelTo). false if the destination is unavailable.
    bool travel(Player& p, uint8_t dim, bool viaPortal = false);
    // ---- nether portals (portals.cpp)
    WorldState wstate;               // known portals, the dragon fight (saved with meta)
    bool lightPortal(int x, int y, int z);          // fills a complete obsidian frame (curDim)
    void checkPortalsAround(int x, int y, int z);   // (x, y, z) changed: portals losing their frame break
    int nearestPortal(uint8_t dim, int bx, int bz) const;
    bool portalArrivalReady(const Player& p, uint8_t dim);
    bool portalArrival(Player& p, uint8_t dim, double& x, double& y, double& z, float& yaw);
    void packWorldState();           // wstate -> meta.extra (before saving meta)
    void arrivalCentre(const Player& p, uint8_t dim, int& bx, int& bz) const;
    bool arrivalReady(const Player& p, uint8_t dim);
    void tickTravel(Player& p);
    void tickPortal(Player& p);
    void tickSurvival(Player& p);
    void addExhaustion(Player& p, float amount);
    void giveXp(Player& p, int points);
    void heal(Player& p, float amount);
    void finishUsingItem(Player& p);

    // ---- inventory (inventory.cpp)
    int giveItem(Player& p, ItemStack st);     // returns count that did not fit
    void openLectern(Player& p, int x, int y, int z);
    void openContainer(Player& p, int x, int y, int z);
    void openCrafting(Player& p, int x, int y, int z);
    void openFurnace(Player& p, int x, int y, int z);
    void closeWindow(Player& p, bool sendClose);
    void tickFurnaceViewers();
    // furnace at (x, y, z): progress up to now; reschedule its next event
    void updateFurnace(int x, int y, int z, bool reschedule);
    void containerChanged(int x, int y, int z);
    // a click on a chiseled bookshelf: a book in or out; false when the click does nothing
    bool useBookshelf(Player& p, int x, int y, int z, uint16_t state);
    // A game event at (x, y, z) with its vibration frequency (GE_*): sculk sensors in
    // range hear it (sculk.cpp).
    void vibration(double x, double y, double z, int frequency);
    void damageHeldItem(Player& p, int amount);
    void consumeHeld(Player& p, int amount = 1);

    // ---- commands (commands.cpp)
    void runCommand(Player* p, const char* line);
    void writeCommandTree(Writer& w);
    int completions(Player& p, const char* text, char out[][40], int max, int& start);

    // ---- chunk helpers (chunks.cpp)
    bool resendLight(uint8_t dim, int cx, int cz);   // false: workers busy, retry later

private:
    void acceptConnections();
    void pollPlayers();
    void tick();
    void tickPlayers();
    void tickTime();
    void tickWeather();
    void tickMobSpawning();
    void tickPerfBar();
    struct PerfBarLine { char json[200]; float health; int color; };
    void perfBarState(PerfBarLine out[PERF_BARS]);
    void autosave();
    void saveMetaLater();
    void finishSave(bool ok);
    void flushLightQueue();
    void queueLight(uint8_t dim, int cx, int cz);
    void upgradePartialLight();
    bool exactLightWanted(uint8_t dim, int cx, int cz);
    void runTimers();
    void runTimerStep();
    void runBlockTick(const TimerEvent& ev);
    void runEntityTimer(const TimerEvent& ev);
    void randomTicks();
    void tickFluid(int x, int y, int z, uint16_t state);

    Listener* listener_ = nullptr;
    bool running_ = false;
    int32_t nextEntityId_ = 1000;
    int32_t dragonId_ = -1;
    bool restartRequested_ = false;
    ClockTickSource clockTicks_{TICK_MS};
    TickSource* tickSource_ = &clockTicks_;
    TickPacer pacer_;
    uint32_t lastTpsMs_ = 0;
    uint32_t tpsTicks_ = 0;
    uint32_t tickMaxWin_ = 0, stallMaxWin_ = 0;
    uint32_t wakeWin_ = 0;
    uint32_t behindLogMs_ = 0;
    LagProfile lagCur_, lagWin_;
    uint32_t lastSaveMs_ = 0;
    bool saving_ = false;
    bool manualSave_ = false;
    int saveRequester_ = -1;
    uint32_t saveSession_ = 0, saveGeneration_ = 0;
    uint32_t storageErrors_ = 0, saveErrorsAtStart_ = 0, chunkErrorsAtStart_ = 0;
    uint32_t lastStatusMs_ = 0;
    bool perfBar_ = false;
    bool spawnInFlight_ = false;
    size_t perfHeapMax_ = 0;   // most free heap seen while the banner is on

    // light resend queue (chunks whose lighting changed)
    static constexpr int LIGHT_QUEUE = 64;
    int32_t lightQ_[LIGHT_QUEUE][3];   // dim, cx, cz
    int lightQLen_ = 0;
    TimerEvent* timerOut_ = nullptr; // entire due batch, in PSRAM; no 256-tick spill
    int timerCount_ = 0, timerIndex_ = 0;
    void* tileTickList_ = nullptr; // reusable PSRAM scratch for ordered block-entity ticks
    int tileTickCapacity_ = 0;
};

// JSON text helpers (text.cpp)
size_t jsonEscape(const char* in, char* out, size_t cap);
void textJson(char* out, size_t cap, const char* text, const char* color = nullptr);

}  // namespace mc
