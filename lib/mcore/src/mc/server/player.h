// One client connection / player.
#pragma once
#include <stdint.h>
#include "mc/limits.h"
#include "mc/net/connection.h"
#include "mc/server/entity.h"
#include "mc/server/types.h"

namespace mc {

class Server;
struct PlayerData;

enum ConnState : uint8_t { CS_FREE = 0, CS_HANDSHAKE, CS_STATUS, CS_LOGIN, CS_PLAY };
enum WindowKind : uint8_t { WK_NONE = 0, WK_CHEST, WK_LARGE_CHEST, WK_CRAFTING, WK_FURNACE };

constexpr int VIEW_SIDE = 2 * MC_MAX_VIEW_DISTANCE + 1;

// Player inventory layout (window 0)
enum : int {
    SLOT_CRAFT_RESULT = 0, SLOT_CRAFT_START = 1, SLOT_ARMOR_START = 5, SLOT_MAIN_START = 9,
    SLOT_HOTBAR_START = 36, SLOT_OFFHAND = 45, INV_SIZE = 46
};

class Player {
public:
    Server* srv = nullptr;
    int slot = 0;
    Connection conn;
    ConnState state = CS_FREE;
    uint32_t connectedAt = 0;
    int protocol = 0;

    char name[17] = {0};
    uint8_t uuid[16] = {0};
    Entity e;

    uint8_t gamemode = 0;
    bool op = false;
    bool flying = false;
    bool dead = false;

    // chunk view
    int viewDist = 4;
    int clientViewDist = 8;
    int centerCx = 0, centerCz = 0;
    bool viewReady = false;
    uint8_t sent[VIEW_SIDE * VIEW_SIDE];
    bool awaitTeleport = false;
    int32_t teleportId = 0;
    bool positionReady = false;     // first position packet arrived

    // keep alive
    uint32_t kaSentMs = 0;
    int64_t kaId = 0;
    bool kaPending = false;
    int ping = 0;

    // inventory and windows
    ItemStack inv[INV_SIZE];
    uint8_t held = 0;               // hotbar index 0..8
    ItemStack cursor;
    int8_t winId = 0;
    uint8_t winKind = WK_NONE;
    int winX = 0, winY = 0, winZ = 0;
    int winX2 = 0, winZ2 = 0;       // second half of a large chest
    ItemStack craft[10];            // crafting table: 0 result, 1..9 grid
    int8_t nextWinId = 1;
    bool invDirty = false;
    int8_t dragMode = -1;           // inventory drag in progress (0 left, 1 right, 2 middle)
    uint8_t dragCount = 0;
    uint8_t dragSlots[64];

    // survival
    int food = 20;
    float saturation = 5;
    float exhaustion = 0;
    int foodTimer = 0;
    int xpLevel = 0;
    float xpProgress = 0;
    int xpTotal = 0;
    bool healthDirty = false;
    int usingTicks = 0;             // > 0 while eating
    uint8_t usingHand = 0;
    bool drawingBow = false;
    uint32_t bowStart = 0;
    bool hasSpawn = false;
    int spawnX = 0, spawnY = 0, spawnZ = 0;

    // digging
    bool digging = false;
    int digX = 0, digY = 0, digZ = 0;
    uint32_t digStart = 0;
    int8_t digStage = -1;

    // misc
    uint8_t skinParts = 0x7F;
    uint8_t mainHand = 1;
    int chatTokens = 10;
    uint32_t lastAttackTick = 0;
    uint32_t knownPlayers = 0;      // bit per slot: entity spawned on this client
    uint8_t knownEntities[(MC_MAX_ENTITIES + 7) / 8];
    double lastX = 0, lastY = 0, lastZ = 0;
    uint32_t lastHeaderMs = 0;

    void reset(Server* s, int slotIndex);
    bool inPlay() const { return state == CS_PLAY && conn.open(); }
    bool isSurvivalLike() const { return gamemode == 0 || gamemode == 2; }
    ItemStack& heldItem() { return inv[SLOT_HOTBAR_START + held]; }

    // ---- networking (login.cpp / play.cpp)
    void onPacket(int id, Reader& r);
    void kick(const char* reason);
    void sendChat(const char* json, uint8_t position = 1);
    void sendSystem(const char* text, const char* color = nullptr);
    void sendActionBar(const char* text);

    // ---- chunks (chunks.cpp)
    void updateView(bool force);
    void streamChunks(int budget);
    void sendChunk(int cx, int cz);
    void resetView();
    bool hasChunk(int cx, int cz) const;

    // ---- state sync
    void teleport(double x, double y, double z, float yaw, float pitch);
    void sendHealth();
    void sendXp();
    void sendAbilities();
    void sendInventory();
    void sendSlot(int slotIndex);
    void sendGameMode();
    void setGameMode(uint8_t gm);
    void sendTime();

    // ---- persistence
    void toData(PlayerData& d) const;
    void fromData(const PlayerData& d);

private:
    // login.cpp
    void handleHandshake(int id, Reader& r);
    void handleStatus(int id, Reader& r);
    void handleLogin(int id, Reader& r);
    void joinGame();
    // play.cpp
    void handlePlay(int id, Reader& r);
    void onChat(Reader& r);
    void onSettings(Reader& r);
    void onMove(double x, double y, double z, bool hasPos, float yaw, float pitch, bool hasLook, bool onGround);
    void onEntityAction(Reader& r);
    void onKeepAlive(Reader& r);
    void onClientCommand(Reader& r);
    void onAbilities(Reader& r);
    void onUseEntity(Reader& r);
    void onUseItem(Reader& r);
    void onHeldItem(Reader& r);
    void onSwing(Reader& r);
    // blocks.cpp
    void onDig(Reader& r);
    void onPlace(Reader& r);
    void onUpdateSign(Reader& r);
    // inventory.cpp
    void onWindowClick(Reader& r);
    void onCloseWindow(Reader& r);
    void onCreativeSlot(Reader& r);
    void onPickItem(Reader& r);
    // commands.cpp
    void onTabComplete(Reader& r);
};

}  // namespace mc
