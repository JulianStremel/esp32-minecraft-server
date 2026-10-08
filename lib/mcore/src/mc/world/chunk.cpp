#include "mc/world/chunk.h"
#include <string.h>
#include "mc/registry.h"

namespace mc {

bool isMotionBlocking(uint16_t state) {
    if (state == 0) return false;
    const BlockDef& b = blockOf(state);
    return (b.flags & (BF_COLLIDES | BF_FLUID)) != 0;
}

Chunk::Chunk(int32_t x, int32_t z, uint8_t d) : cx(x), cz(z), dim(d) {
    for (int i = 0; i < NUM_SECTIONS; i++) sec_[i] = nullptr;
    memset(height_, 0, sizeof(height_));
    memset(biome_, biome::Plains, sizeof(biome_));
}

ChunkSnap::~ChunkSnap() { delete chunk; }

Chunk::~Chunk() {
    if (snap) snap->release();
    clearTicks();
    for (int i = 0; i < NUM_SECTIONS; i++) delete sec_[i];
    while (tiles_) {
        TileEntity* n = tiles_->next;
        delete tiles_;
        tiles_ = n;
    }
}

bool Chunk::setTicks(const ChunkTick* t, int n) {
    clearTicks();
    if (n <= 0) return true;
    if (n > 0xFFFF) n = 0xFFFF;
    ticks = (ChunkTick*)plat::bigAlloc(sizeof(ChunkTick) * (size_t)n);
    if (!ticks) return false;
    for (int i = 0; i < n; i++) ticks[i] = t[i];
    tickCount = (uint16_t)n;
    return true;
}

void Chunk::clearTicks() {
    plat::bigFree(ticks);
    ticks = nullptr;
    tickCount = 0;
}

Chunk* Chunk::clone() const {
    Chunk* c = new Chunk(cx, cz, dim);
    if (!c) return nullptr;
    for (int i = 0; i < NUM_SECTIONS; i++) {
        if (!sec_[i]) continue;
        c->sec_[i] = new Section();
        if (!c->sec_[i] || !c->sec_[i]->copyFrom(*sec_[i])) {
            delete c;
            return nullptr;
        }
    }
    memcpy(c->height_, height_, sizeof(height_));
    memcpy(c->biome_, biome_, sizeof(biome_));
    // keep the list order (newest first) so encodings of the copy are identical
    TileEntity** tail = &c->tiles_;
    for (const TileEntity* t = tiles_; t; t = t->next) {
        TileEntity* n = new TileEntity(*t);
        if (!n) {
            delete c;
            return nullptr;
        }
        n->next = nullptr;
        *tail = n;
        tail = &n->next;
    }
    c->dirty = dirty;
    c->lightDirty = lightDirty;
    c->readOnly = readOnly;
    c->storeSeq = storeSeq;
    c->storeSlot = storeSlot;
    c->version = version;
    c->skyVersion = skyVersion;
    c->residency = residency;
    c->movingPistons_ = movingPistons_;
    c->hoppers_ = hoppers_;
    c->daylights_ = daylights_;
    return c;
}

TileEntity* Chunk::tileAt(int lx, int y, int lz) const {
    for (TileEntity* t = tiles_; t; t = t->next)
        if (t->lx == lx && t->lz == lz && t->y == y) return t;
    return nullptr;
}

TileEntity* Chunk::addTile(uint8_t type, int lx, int y, int lz) {
    TileEntity* t = new TileEntity();
    if (!t) return nullptr;
    removeTile(lx, y, lz);
    t->type = type;
    if (type == TILE_PISTON) ++movingPistons_;
    if (type == TILE_HOPPER) ++hoppers_;
    if (type == TILE_DAYLIGHT) ++daylights_;
    t->lx = (uint8_t)lx;
    t->lz = (uint8_t)lz;
    t->y = (uint8_t)y;
    t->next = tiles_;
    tiles_ = t;
    return t;
}

void Chunk::removeTile(int lx, int y, int lz) {
    TileEntity** pp = &tiles_;
    while (*pp) {
        TileEntity* t = *pp;
        if (t->lx == lx && t->lz == lz && t->y == y) {
            if (t->type == TILE_PISTON) --movingPistons_;
            if (t->type == TILE_HOPPER) --hoppers_;
            if (t->type == TILE_DAYLIGHT) --daylights_;
            *pp = t->next;
            delete t;
            return;
        }
        pp = &t->next;
    }
}

int Chunk::tileCount() const {
    int n = 0;
    for (TileEntity* t = tiles_; t; t = t->next) n++;
    return n;
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
    if (blockOf(old).filterLight != blockOf(state).filterLight) ++skyVersion;
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
    for (TileEntity* t = tiles_; t; t = t->next) n += sizeof(TileEntity);
    return n;
}

}  // namespace mc
