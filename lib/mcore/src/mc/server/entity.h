// Entities: players, dropped items, mobs, falling blocks, arrows.
#pragma once
#include <stdint.h>
#include "mc/item.h"
#include "mc/server/path.h"

namespace mc {

enum EntityKind : uint8_t { EK_NONE = 0, EK_PLAYER, EK_ITEM, EK_MOB, EK_FALLING_BLOCK, EK_ARROW };

// entity metadata flag bits (index 0)
enum : uint8_t { EF_ON_FIRE = 0x01, EF_CROUCHING = 0x02, EF_SPRINTING = 0x08, EF_SWIMMING = 0x10, EF_INVISIBLE = 0x20 };
// poses (index 6)
enum : uint8_t { POSE_STANDING = 0, POSE_FALL_FLYING = 1, POSE_SLEEPING = 2, POSE_SWIMMING = 3, POSE_DYING = 6, POSE_CROUCHING = 5 };

struct Entity {
    int32_t id = 0;
    uint16_t type = 0;          // entity type registry id
    uint8_t kind = EK_NONE;
    uint8_t uuid[16] = {0};
    double x = 0, y = 0, z = 0;
    double vx = 0, vy = 0, vz = 0;
    float yaw = 0, pitch = 0, headYaw = 0;
    bool onGround = false;
    float width = 0.6f, height = 1.8f;

    // last state sent to clients (for delta encoding)
    double sx = 0, sy = 0, sz = 0;
    uint8_t syaw = 0, spitch = 0, shead = 0;
    uint16_t sinceTeleport = 0;
    bool metaDirty = false, equipDirty = false, velDirty = false;

    // living entity state
    uint8_t flags = 0;
    uint8_t pose = POSE_STANDING;
    float health = 20, maxHealth = 20;
    int16_t invuln = 0;         // ticks of damage immunity left
    int16_t hurtTicks = 0;
    int16_t deathTicks = 0;
    float fallDistance = 0;
    int16_t fireTicks = 0;
    int16_t air = 300;
    uint32_t age = 0;

    // items / projectiles / falling blocks
    ItemStack item;
    int16_t pickupDelay = 0;
    int32_t owner = -1;         // entity id of the shooter / dropper
    uint16_t blockState = 0;
    float damage = 0;           // projectile damage

    // mobs
    int32_t target = -1;        // entity id being chased
    int16_t aiTimer = 0;
    float goalX = 0, goalZ = 0;
    bool hasGoal = false;
    int16_t attackCooldown = 0;
    int16_t fuse = -1;          // creeper fuse
    uint8_t variant = 0;        // sheep colour etc.
    bool hostile = false;
    bool burnsInDay = false;
    int32_t lastAttacker = -1;
    // path finding (mob_paths.cpp): the waypoints of the current path
    static const int PATH_POINTS = 16;
    PathPoint path[PATH_POINTS];
    uint8_t pathLen = 0, pathIdx = 0;
    bool pathPending = false;         // a path job is in flight
    PathPoint pathGoal{0, 0, 0};      // where the target was when it was requested
    uint32_t pathVersions = 0;        // sum of the versions of the chunks it used
    int16_t stuckTicks = 0;
    float lastX = 0, lastZ = 0;

    int8_t playerSlot = -1;     // for EK_PLAYER
    uint8_t dim = 0;            // the dimension it is in (DIM_OVERWORLD, DIM_NETHER, DIM_END)
    bool removed = false;

    bool alive() const { return kind != EK_NONE && !removed && health > 0; }
};

}  // namespace mc
