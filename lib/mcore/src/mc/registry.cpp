#include "mc/registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace mc {

static const char* stripNs(const char* n) {
    if (!strncmp(n, "minecraft:", 10)) return n + 10;
    return n;
}

int propIndexOf(uint16_t blockId, const char* name) {
    const BlockDef& b = BLOCKS[blockId];
    for (int i = 0; i < b.propCount; i++)
        if (!strcmp(PROPS[BLOCK_PROPS[b.propStart + i]].name, name)) return i;
    return -1;
}

// stride of property i = product of the value counts of the properties after it
static int strideOf(const BlockDef& b, int i) {
    int s = 1;
    for (int j = i + 1; j < b.propCount; j++) s *= PROPS[BLOCK_PROPS[b.propStart + j]].n;
    return s;
}

int propValue(uint16_t state, int i) {
    const BlockDef& b = blockOf(state);
    const PropDef& p = PROPS[BLOCK_PROPS[b.propStart + i]];
    return ((state - b.minState) / strideOf(b, i)) % p.n;
}

uint16_t withPropValue(uint16_t state, int i, int value) {
    const BlockDef& b = blockOf(state);
    const PropDef& p = PROPS[BLOCK_PROPS[b.propStart + i]];
    if (value < 0 || value >= p.n) return state;
    int stride = strideOf(b, i);
    int cur = ((state - b.minState) / stride) % p.n;
    return (uint16_t)(state + (value - cur) * stride);
}

int getProp(uint16_t state, const char* name) {
    int i = propIndexOf(blockIdOf(state), name);
    return i < 0 ? -1 : propValue(state, i);
}

uint16_t setProp(uint16_t state, const char* name, int valueIndex) {
    int i = propIndexOf(blockIdOf(state), name);
    return i < 0 ? state : withPropValue(state, i, valueIndex);
}

uint16_t setPropStr(uint16_t state, const char* name, const char* value) {
    const BlockDef& b = blockOf(state);
    int i = propIndexOf(blockIdOf(state), name);
    if (i < 0) return state;
    const PropDef& p = PROPS[BLOCK_PROPS[b.propStart + i]];
    if (p.type == 0) return withPropValue(state, i, !strcmp(value, "true") ? 0 : 1);
    if (p.type == 1) return withPropValue(state, i, atoi(value));
    for (int v = 0; v < p.n; v++)
        if (!strcmp(p.values[v], value)) return withPropValue(state, i, v);
    return state;
}

const char* getPropStr(uint16_t state, const char* name) {
    const BlockDef& b = blockOf(state);
    int i = propIndexOf(blockIdOf(state), name);
    if (i < 0) return nullptr;
    const PropDef& p = PROPS[BLOCK_PROPS[b.propStart + i]];
    int v = propValue(state, i);
    if (p.type == 0) return v == 0 ? "true" : "false";
    if (p.type == 2) return p.values[v];
    static char tmp[8];
    snprintf(tmp, sizeof(tmp), "%d", v);
    return tmp;
}

int findBlock(const char* name) {
    name = stripNs(name);
    for (int i = 0; i < NUM_BLOCKS; i++)
        if (!strcmp(BLOCKS[i].name, name)) return i;
    return -1;
}

int findItem(const char* name) {
    name = stripNs(name);
    for (int i = 1; i < NUM_ITEMS; i++)
        if (!strcmp(ITEMS[i].name, name)) return i;
    return -1;
}

int findEntityType(const char* name) {
    name = stripNs(name);
    for (int i = 0; i < NUM_ENTITY_TYPES; i++)
        if (!strcmp(ENTITY_TYPES[i].name, name)) return ENTITY_TYPES[i].id;
    return -1;
}

bool isMobType(int type) {
    static const char* const NOT_MOBS[] = {
        "area_effect_cloud", "armor_stand", "item_frame", "leash_knot", "painting", "arrow", "dragon_fireball",
        "fireball", "llama_spit", "shulker_bullet", "small_fireball", "snowball", "spectral_arrow", "egg",
        "ender_pearl", "potion", "wither_skull", "boat", "minecart", "chest_minecart", "command_block_minecart",
        "furnace_minecart", "hopper_minecart", "spawner_minecart", "tnt_minecart", "falling_block", "tnt", "item",
        "end_crystal", "experience_orb", "eye_of_ender", "firework_rocket", "lightning_bolt", "experience_bottle",
        "trident", "player", "fishing_bobber", "evoker_fangs"};
    for (int i = 0; i < NUM_ENTITY_TYPES; i++) {
        if (ENTITY_TYPES[i].id != type) continue;
        for (const char* n : NOT_MOBS)
            if (!strcmp(ENTITY_TYPES[i].name, n)) return false;
        return true;
    }
    return false;
}

}  // namespace mc
