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
    if (p.type == 1 && !p.values) return withPropValue(state, i, atoi(value));
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
    if (p.values) return p.values[v];
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
    for (int i = 0; i < NUM_ENTITY_TYPES; i++)
        if (ENTITY_TYPES[i].id == type) return ENTITY_TYPES[i].mob;
    return false;
}

bool ingredientMatches(uint16_t set, uint16_t item) {
    if (set == 0 || set >= NUM_INGREDIENT_SETS) return false;
    // sorted: a binary search (a set is one item, or a tag of up to a few dozen)
    int lo = INGREDIENT_SET_START[set], hi = INGREDIENT_SET_START[set + 1] - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (INGREDIENT_ITEMS[mid] == item) return true;
        if (INGREDIENT_ITEMS[mid] < item) lo = mid + 1;
        else hi = mid - 1;
    }
    return false;
}

int fuelBurnTicks(uint16_t item) {
    int lo = 0, hi = NUM_FUELS - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (FUELS[mid].item == item) return FUELS[mid].ticks;
        if (FUELS[mid].item < item) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0;
}

const CookingDef* cookingRecipe(uint16_t in, uint8_t kind) {
    int lo = 0, hi = NUM_COOKING - 1;   // sorted by the input item
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (COOKING[mid].in < in) lo = mid + 1;
        else hi = mid;
    }
    for (int i = lo; i < NUM_COOKING && COOKING[i].in == in; i++)
        if (COOKING[i].kinds & kind) return &COOKING[i];
    return nullptr;
}

}  // namespace mc
