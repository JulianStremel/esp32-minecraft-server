// Static registries for Minecraft 1.16.5 (protocol 754): blocks, block states,
// items, recipes and entity types. The tables are generated (see tools/gen_data.js).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "mc/data/biome_ids_gen.h"
#include "mc/data/entity_ids_gen.h"
#include "mc/data/ids_gen.h"
#include "mc/data/packet_ids_gen.h"

namespace mc {

enum BlockFlag : uint8_t {
    BF_COLLIDES = 1,       // has a collision box (solid for entities)
    BF_TRANSPARENT = 2,
    BF_DIGGABLE = 4,
    BF_FLUID = 8,
    BF_REPLACEABLE = 16,   // can be built over (air, water, grass, ...)
    BF_GRAVITY = 32,       // falls when unsupported (sand, gravel, ...)
    BF_NEEDS_SUPPORT = 64, // plants that break when the block below is not solid
    BF_TOOL_REQUIRED = 128 // drops nothing without the right tool tier
};

enum ToolClass : uint8_t { TC_HAND = 0, TC_PICKAXE = 1, TC_AXE = 2, TC_SHOVEL = 3, TC_HOE = 4, TC_SHEARS = 5 };

struct PropDef {
    const char* name;
    uint8_t type;  // 0 bool (values: true,false), 1 int (0..n-1), 2 enum
    uint8_t n;
    const char* const* values;
};

struct BlockDef {
    const char* name;
    uint16_t minState;
    uint16_t defState;
    uint16_t numStates;
    uint16_t propStart;   // index into BLOCK_PROPS
    uint8_t propCount;
    uint8_t flags;
    uint8_t filterLight;  // how much light is absorbed (15 = opaque)
    uint8_t emitLight;
    uint8_t toolClass;
    uint8_t minTier;      // 0 none, 1 wood/gold, 2 stone, 3 iron, 4 diamond
    uint16_t item;        // item id of this block (0 = none)
    float hardness;       // < 0 not diggable in survival
    uint16_t dropItem;
    uint8_t dropMin;
    uint8_t dropMax;
};

enum ItemKind : uint8_t {
    IK_NONE = 0, IK_PICKAXE, IK_AXE, IK_SHOVEL, IK_HOE, IK_SWORD, IK_SHEARS, IK_ARMOR, IK_FOOD, IK_BOW
};

struct ItemDef {
    const char* name;
    uint8_t stack;
    uint16_t durability;
    uint16_t block;       // block id if placeable, 0xFFFF otherwise
    uint8_t kind;
    uint8_t tier;         // 0 wood 1 stone 2 iron 3 diamond 4 netherite 5 gold
    uint8_t attack;       // attack damage (half hearts)
    uint8_t armor;        // armor points
    uint8_t armorSlot;    // 0 helmet 1 chest 2 legs 3 boots
    uint8_t food;         // hunger points restored
    uint8_t saturation10; // saturation restored * 10
};

struct RecipeDef {
    uint16_t result;
    uint8_t count;
    uint8_t w, h;
    uint8_t shapeless;
    uint16_t start;       // index into RECIPE_INGREDIENTS
};

struct EntityTypeDef {
    const char* name;
    uint16_t id;
    float width, height;
};

extern const PropDef PROPS[];
extern const int NUM_PROPS;
extern const uint16_t BLOCK_PROPS[];
extern const BlockDef BLOCKS[];
extern const uint16_t STATE_TO_BLOCK[];
extern const ItemDef ITEMS[];
extern const RecipeDef RECIPES[];
extern const int NUM_RECIPES;
extern const uint16_t RECIPE_INGREDIENTS[];
extern const EntityTypeDef ENTITY_TYPES[];
extern const int NUM_ENTITY_TYPES;
extern const uint8_t DIMENSION_CODEC_NBT[];
extern const size_t DIMENSION_CODEC_NBT_LEN;
// per dimension (DIM_OVERWORLD, DIM_NETHER, DIM_END): its type for Join Game / Respawn, and its name
extern const uint8_t* const DIMENSION_NBT[];
extern const size_t DIMENSION_NBT_LEN[];
extern const char* const DIMENSION_NAME[];

// ---------------------------------------------------------------- state helpers
inline bool validState(int s) { return s >= 0 && s < NUM_STATES; }
inline uint16_t blockIdOf(uint16_t state) { return STATE_TO_BLOCK[state]; }
inline const BlockDef& blockOf(uint16_t state) { return BLOCKS[STATE_TO_BLOCK[state]]; }
inline bool stateIsAir(uint16_t s) { return s == 0 || s == bs::CaveAir || s == bs::VoidAir; }
inline bool stateCollides(uint16_t s) { return blockOf(s).flags & BF_COLLIDES; }
inline bool stateOpaque(uint16_t s) { return blockOf(s).filterLight >= 15; }
inline bool stateIsFluid(uint16_t s) { return blockOf(s).flags & BF_FLUID; }

// Property access by name. Returns -1 if the block has no such property.
int propIndexOf(uint16_t blockId, const char* name);
// Value index of property #i (0-based within the block) for the state.
int propValue(uint16_t state, int i);
uint16_t withPropValue(uint16_t state, int i, int value);
// Convenience: by name.
int getProp(uint16_t state, const char* name);
uint16_t setProp(uint16_t state, const char* name, int valueIndex);
// Enum helpers: set by value string ("north"), get string. bool: "true"/"false".
uint16_t setPropStr(uint16_t state, const char* name, const char* value);
const char* getPropStr(uint16_t state, const char* name);
// Bool props are stored as value 0 = true, 1 = false (matches the wire order).
inline bool getBool(uint16_t state, const char* name) { return getProp(state, name) == 0; }
inline uint16_t setBool(uint16_t state, const char* name, bool v) { return setProp(state, name, v ? 0 : 1); }

// Light emission varies with block state (immutable, safe on worker snapshots).
extern const uint16_t REDSTONE_STATE_PROPERTIES[NUM_STATES];
extern const uint8_t PISTON_STATE_PROPERTIES[NUM_STATES];
extern const uint16_t COLLISION_SHAPE_OFFSETS[NUM_STATES];
extern const int8_t COLLISION_SHAPES[];
// The top of a state's collision boxes in 1/32 block: 0 without any, 32 for a full block,
// 48 for fences, walls and closed fence gates (1.5 blocks: nothing can stand or step on them).
inline int collisionTop32(uint16_t state) {
    const int8_t* p = COLLISION_SHAPES + COLLISION_SHAPE_OFFSETS[state];
    int n = *p++, top = 0;
    for (int i = 0; i < n; i++, p += 6)
        if (p[4] > top) top = p[4];
    return top;
}
enum PushReaction { PUSH_NORMAL, PUSH_DESTROY, PUSH_BLOCK, PUSH_IGNORE, PUSH_ONLY };
inline PushReaction statePushReaction(uint16_t state) { return (PushReaction)(PISTON_STATE_PROPERTIES[state] & 7); }
inline bool stateHasBlockEntity(uint16_t state) { return PISTON_STATE_PROPERTIES[state] & 8; }
inline bool stateUnbreakable(uint16_t state) { return PISTON_STATE_PROPERTIES[state] & 16; }
inline uint8_t stateEmission(uint16_t state) { return REDSTONE_STATE_PROPERTIES[state] & 15; }
inline bool stateConductsRedstone(uint16_t state) { return REDSTONE_STATE_PROPERTIES[state] & 16; }
inline bool stateSignalSource(uint16_t state) { return REDSTONE_STATE_PROPERTIES[state] & 32; }
inline int stateNoteInstrument(uint16_t state) { return REDSTONE_STATE_PROPERTIES[state] >> 12; }
inline bool stateFaceSturdy(uint16_t state, int face) { return REDSTONE_STATE_PROPERTIES[state] & (1u << (6 + face)); }

// Lookup by name ("minecraft:" prefix optional). Returns -1 if unknown.
int findBlock(const char* name);
int findItem(const char* name);
int findEntityType(const char* name);
// true for living mob types (not items, projectiles, vehicles, paintings, ...)
bool isMobType(int type);

// Item <-> block helpers
inline bool itemIsBlock(uint16_t item) { return item < NUM_ITEMS && ITEMS[item].block != 0xFFFF; }
inline uint16_t itemBlockState(uint16_t item) { return BLOCKS[ITEMS[item].block].defState; }
inline int maxStack(uint16_t item) { return item < NUM_ITEMS ? (ITEMS[item].stack ? ITEMS[item].stack : 64) : 64; }

}  // namespace mc

namespace mc {
extern const uint8_t TAGS_PAYLOAD[];
extern const size_t TAGS_PAYLOAD_LEN;
}
