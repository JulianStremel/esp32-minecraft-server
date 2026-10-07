#include "mc/world/chunk.h"
#include <string.h>
#include "mc/registry.h"

namespace mc {

bool isMotionBlocking(uint16_t state) {
    if (state == 0) return false;
    const BlockDef& b = blockOf(state);
    return (b.flags & (BF_COLLIDES | BF_FLUID)) != 0;
}

Chunk::Chunk(int32_t x, int32_t z) : cx(x), cz(z) {
    for (int i = 0; i < NUM_SECTIONS; i++) sec_[i] = nullptr;
    memset(height_, 0, sizeof(height_));
    memset(biome_, biome::Plains, sizeof(biome_));
}

Chunk::~Chunk() {
    for (int i = 0; i < NUM_SECTIONS; i++) delete sec_[i];
}

Section* Chunk::ensureSection(int i) {
    if (!sec_[i]) sec_[i] = new Section();
    return sec_[i];
}

void Chunk::dropEmptySections() {
    for (int i = 0; i < NUM_SECTIONS; i++) {
        if (sec_[i] && sec_[i]->isUniform() && sec_[i]->uniformState() == 0) {
            delete sec_[i];
            sec_[i] = nullptr;
        }
    }
}

uint16_t Chunk::set(int lx, int y, int lz, uint16_t state) {
    if (y < 0 || y >= WORLD_HEIGHT) return 0;
    int si = y >> 4;
    Section* s = sec_[si];
    if (!s) {
        if (state == 0) return 0;
        s = ensureSection(si);
    }
    uint16_t old = s->set(lx, y, lz, state);
    if (old == state) return old;
    version++;
    int hi = lx + lz * 16;
    bool oldB = isMotionBlocking(old), newB = isMotionBlocking(state);
    if (newB && y + 1 > height_[hi]) {
        height_[hi] = (uint16_t)(y + 1);
    } else if (oldB && !newB && y + 1 == height_[hi]) {
        int h = y;
        while (h > 0 && !isMotionBlocking(get(lx, h - 1, lz))) h--;
        height_[hi] = (uint16_t)(h > 0 ? h : 0);
    }
    return old;
}

void Chunk::recomputeHeightmap() {
    for (int lz = 0; lz < 16; lz++) {
        for (int lx = 0; lx < 16; lx++) {
            int h = WORLD_HEIGHT;
            while (h > 0 && !isMotionBlocking(get(lx, h - 1, lz))) {
                // skip whole empty sections quickly
                if ((h & 15) == 0 && !sec_[(h - 1) >> 4]) h -= 16;
                else h--;
            }
            height_[lx + lz * 16] = (uint16_t)h;
        }
    }
}

int Chunk::highestSection() const {
    for (int i = NUM_SECTIONS - 1; i >= 0; i--)
        if (sec_[i] && sec_[i]->nonAirCount() > 0) return i;
    return -1;
}

size_t Chunk::memoryBytes() const {
    size_t n = sizeof(Chunk);
    for (int i = 0; i < NUM_SECTIONS; i++)
        if (sec_[i]) n += sec_[i]->memoryBytes();
    return n;
}

}  // namespace mc
