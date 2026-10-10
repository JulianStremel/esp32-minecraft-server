// Generator version 3: vanilla's terrain shape (the 1.21.8 noise router, mc/world/vanilla),
// chosen for new worlds (the default stays version 2).
//
// - The shape: final_density from the router (vanilla's continents, ocean depths,
//   mountains to y 250+, noise caves), stone where it is positive, deepslate below y 0
//   (blended over y 0..7 as vanilla's deepslate rule), water up to sea level (y 62) and
//   lava below y -54 where it is not (vanilla without aquifers), bedrock at y -64 and
//   thinning out to -60.
// - Biomes: a 2D biome per 4 x 4 cell from the router's climate (temperature,
//   vegetation, continentalness) and the surface height: oceans by depth and temperature,
//   beaches, snowy plains and taiga, plains, forests, jungle, savanna, desert, peaks.
//   Not yet vanilla's parameter table (it exists only as code in the game).
// - The surface: grass and dirt, sand and sandstone (deserts, beaches), gravel and sand
//   under water, stone on peaks; vanilla's surface rule comes later.
// - Ores and plants as version 2; trees stay inside their own chunk (version 2 takes
//   neighbouring chunks' heights from its cheap column(), which this shape does not have).
// The routers are not thread-safe: a small pool, one per worker generating at a time.
#include <string.h>
#include "mc/platform.h"
#include "mc/registry.h"
#include "mc/world/generator.h"
#include "mc/world/vanilla/density.h"

namespace mc {

namespace {
constexpr int SEA = 63;   // water up to y 62

uint8_t biomeFor(float t, float v, float c, int top) {
    using namespace biome;
    if (c < -0.455f) {   // deep ocean
        if (t < -0.45f) return DeepFrozenOcean;
        if (t < -0.15f) return DeepColdOcean;
        if (t > 0.2f) return DeepLukewarmOcean;
        return DeepOcean;
    }
    if (c < -0.19f || top < SEA - 2) {   // ocean (also inland water below sea level)
        if (c >= -0.19f) return t < -0.45f ? FrozenRiver : River;
        if (t < -0.45f) return FrozenOcean;
        if (t < -0.15f) return ColdOcean;
        if (t > 0.55f) return WarmOcean;
        if (t > 0.2f) return LukewarmOcean;
        return Ocean;
    }
    if (top >= 170) return t < -0.15f ? FrozenPeaks : StonyPeaks;
    if (top >= 130 && t < -0.15f) return SnowySlopes;
    if (c < -0.11f && top <= SEA + 3) return t < -0.45f ? SnowyBeach : Beach;
    if (t < -0.45f) return v > 0.1f ? SnowyTaiga : SnowyPlains;
    if (t < -0.15f) return v > 0.0f ? Taiga : Plains;
    if (t < 0.2f) {
        if (v < -0.35f) return Plains;
        if (v < 0.1f) return v < -0.1f ? Plains : Forest;
        if (v < 0.3f) return BirchForest;
        return DarkForest;
    }
    if (t < 0.55f) {
        if (v < -0.1f) return Savanna;
        if (v < 0.3f) return Forest;
        return Jungle;
    }
    if (v < 0.0f) return Desert;
    return v < 0.3f ? Savanna : Jungle;
}
}  // namespace

Generator::~Generator() {
    for (int i = 0; i < POOL; i++) {
        delete pool_[i];
        plat::bigFree(poolBuf_[i]);
    }
    if (poolLock_) plat::mutexDestroy(poolLock_);
}

// a router of the pool for this chunk (made on first use: ~60 KB PSRAM, ~9 KB internal RAM
// each, plus its 96 KB block buffer)
int Generator::takeRouter() const {
    for (;;) {
        {
            LockGuard g(poolLock_);
            for (int i = 0; i < POOL; i++)
                if (!poolBusy_[i] && pool_[i]) { poolBusy_[i] = true; return i; }
            for (int i = 0; i < POOL; i++)
                if (!pool_[i]) {
                    poolBusy_[i] = true;
                    pool_[i] = new vanilla::Router();
                    poolBuf_[i] = (uint8_t*)plat::bigAlloc(16 * 16 * vanilla::DF_SHAPE.height);
                    if (!pool_[i] || !poolBuf_[i] || !pool_[i]->init(seed_, 1.0f / 64, 0.1f, true, true)) {
                        delete pool_[i];
                        plat::bigFree(poolBuf_[i]);
                        pool_[i] = nullptr;
                        poolBuf_[i] = nullptr;
                        poolBusy_[i] = false;
                        return -1;
                    }
                    return i;
                }
        }
        plat::delayMs(1);   // all busy: another worker is generating
    }
}

void Generator::giveRouter(int i) const {
    LockGuard g(poolLock_);
    poolBusy_[i] = false;
}

void Generator::generateVanilla(Chunk& c) const {
    int slot = takeRouter();
    if (slot < 0) {   // no memory for a router: an empty chunk rather than a crash
        c.setBiomeAll(biome::Plains);
        return;
    }
    vanilla::Router& r = *pool_[slot];
    uint8_t* solid = poolBuf_[slot];
    r.fillChunk(c.cx, c.cz, solid);
    const int minY = vanilla::DF_SHAPE.minY, H = vanilla::DF_SHAPE.height;
    Rng rng(hash3(seed_, c.cx, c.cz, 404));
    int tops[256];
    for (int lz = 0; lz < 16; lz++)
        for (int lx = 0; lx < 16; lx++) {
            int top = minY - 1;
            for (int i = 0; i < H; i++) {
                int y = minY + i;
                bool s = solid[(i * 16 + lz) * 16 + lx];
                uint16_t st;
                if (y == minY || (y <= minY + 4 && (int)rng.range(5) >= y - minY)) st = bs::Bedrock;
                else if (s) st = (y < 0 || (y < 8 && (int)rng.range(8) >= y)) ? bs::Deepslate : bs::Stone;
                else if (y < SEA) st = y < -54 ? bs::Lava : bs::Water;
                else continue;
                c.set(lx, y, lz, st);
                if (s || st == bs::Bedrock) top = y;
            }
            tops[lz * 16 + lx] = top;
        }
    // biomes from the climate at the surface, per 4 x 4 cell
    uint8_t cell[16];
    for (int q = 0; q < 16; q++) {
        int qx = q & 3, qz = q >> 2;
        int lx = qx * 4 + 2, lz = qz * 4 + 2;
        int top = tops[lz * 16 + lx];
        int wx = c.cx * 16 + lx, wz = c.cz * 16 + lz, wy = top > SEA ? top : SEA;
        float t = r.sample(vanilla::ROOT_TEMPERATURE, wx, wy, wz);
        float v = r.sample(vanilla::ROOT_VEGETATION, wx, wy, wz);
        float cont = r.sample(vanilla::ROOT_CONTINENTS, wx, wy, wz);
        cell[q] = biomeFor(t, v, cont, top);
        c.setBiomeCell(qx, qz, cell[q]);
    }
    giveRouter(slot);
    // the surface
    ColumnInfo cols[256];
    for (int lz = 0; lz < 16; lz++)
        for (int lx = 0; lx < 16; lx++) {
            int top = tops[lz * 16 + lx];
            uint8_t b = cell[(lz >> 2) * 4 + (lx >> 2)];
            cols[lz * 16 + lx] = {(int16_t)top, b, false};
            if (top < minY + 1) continue;
            uint16_t ground = c.get(lx, top, lz);
            if (ground != bs::Stone && ground != bs::Deepslate) continue;
            uint16_t topBlock = setBool(bs::GrassBlock, "snowy", false), under = bs::Dirt, deep = bs::Dirt;
            int depth = 3;
            if (top < SEA - 1) {   // under water
                topBlock = under = (SEA - 1 - top) <= 6 ? bs::Sand : bs::Gravel;
                deep = topBlock == bs::Sand ? bs::Sandstone : bs::Gravel;
                depth = 2;
            } else if (b == biome::Desert || b == biome::Beach || b == biome::SnowyBeach) {
                topBlock = under = bs::Sand;
                deep = bs::Sandstone;
            } else if (b == biome::StonyPeaks || b == biome::FrozenPeaks) {
                continue;   // bare stone (snow by the snow cover)
            } else if (b == biome::SnowySlopes) {
                topBlock = under = deep = bs::SnowBlock;
                depth = 1;
            }
            for (int d = 0; d <= depth + 1 && top - d > minY; d++) {
                uint16_t at = c.get(lx, top - d, lz);
                if (at != bs::Stone && at != bs::Deepslate) break;
                c.set(lx, top - d, lz, d == 0 ? topBlock : d <= depth ? under : deep);
            }
        }
    placeOres(c);
    // trees, wholly inside the chunk
    int count = 0;
    uint8_t mid = cell[5];
    switch (mid) {
        case biome::Forest: case biome::BirchForest: count = 6; break;
        case biome::DarkForest: count = 9; break;
        case biome::Taiga: count = 5; break;
        case biome::SnowyTaiga: count = 3; break;
        case biome::Jungle: count = 8; break;
        case biome::Savanna: count = rng.range(3) == 0 ? 1 : 0; break;
        case biome::Plains: count = rng.range(5) == 0 ? 1 : 0; break;
        case biome::SnowyPlains: count = rng.range(6) == 0 ? 1 : 0; break;
        default: break;
    }
    for (int i = 0; i < count; i++) {
        int lx = 3 + (int)rng.range(10), lz = 3 + (int)rng.range(10);
        const ColumnInfo& ci = cols[lz * 16 + lx];
        if (ci.height < SEA || blockIdOf(c.get(lx, ci.height, lz)) != blk::GrassBlock) continue;
        if (!stateIsAir(c.get(lx, ci.height + 1, lz))) continue;
        uint8_t b = ci.biome, kind = TREE_OAK;
        if (b == biome::BirchForest) kind = TREE_BIRCH;
        else if (b == biome::Forest) kind = rng.range(5) == 0 ? TREE_BIRCH : TREE_OAK;
        else if (b == biome::Taiga || b == biome::SnowyTaiga || b == biome::SnowyPlains) kind = TREE_SPRUCE;
        else if (b == biome::Jungle) kind = TREE_JUNGLE;
        else if (b == biome::Savanna) kind = TREE_ACACIA;
        else if (b == biome::DarkForest) kind = TREE_DARK_OAK;
        placeTree(c, {c.cx * 16 + lx, c.cz * 16 + lz, ci.height + 1, kind});
    }
    decorate(c, cols, false);
}

// the first dry land near the origin, from generated chunks (version 3 has no cheap
// column height)
void Generator::findVanillaSpawn(int& x, int& y, int& z) const {
    int px = 0, pz = 0, dx = 0, dz = -1;
    for (int i = 0; i < 256; i++) {
        Chunk* c = new Chunk(px, pz);
        if (!c) break;
        generateVanilla(*c);
        c->recomputeHeightmap();
        for (int lz = 4; lz < 12; lz++)
            for (int lx = 4; lx < 12; lx++) {
                int h = c->height(lx, lz);   // the first free block
                uint16_t g = c->get(lx, h - 1, lz);
                if (h > SEA && h < 120 && blockIdOf(g) == blk::GrassBlock) {
                    x = px * 16 + lx; y = h; z = pz * 16 + lz;
                    delete c;
                    return;
                }
            }
        delete c;
        if (px == pz || (px < 0 && px == -pz) || (px > 0 && px == 1 - pz)) {
            int t = dx; dx = -dz; dz = t;
        }
        px += dx; pz += dz;
    }
    x = 0; z = 0; y = 120;
}

}  // namespace mc
