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
#include <stdlib.h>
#include <string.h>
#include "mc/platform.h"
#include "mc/registry.h"
#include "mc/world/generator.h"
#include "mc/world/vanilla/aquifer.h"
#include "mc/world/vanilla/biomes.h"
#include "mc/world/vanilla/carvers.h"
#include "mc/world/vanilla/density.h"
#include "mc/world/vanilla/surface.h"

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
// Version 4: vanilla's biomes. The climate at each quart column's corner (where vanilla
// samples it: QuartPos.toBlock), the depth at each quart y; the surface biome of a column
// (weather, snow, spawning, the surface) is the one at its top block.
void vanillaBiomes(vanilla::Router& r, Chunk& c, const int* tops, uint8_t* cell, uint8_t* cells3d) {
    using namespace vanilla;
    const int quarts = c.numSections() * 4;
    for (int q = 0; q < 16; q++) {
        int qx = q & 3, qz = q >> 2;
        int wx = c.cx * 16 + qx * 4, wz = c.cz * 16 + qz * 4;
        const float climate[5] = {r.sample(ROOT_TEMPERATURE, wx, 0, wz), r.sample(ROOT_VEGETATION, wx, 0, wz),
                                  r.sample(ROOT_CONTINENTS, wx, 0, wz), r.sample(ROOT_EROSION, wx, 0, wz),
                                  r.sample(ROOT_RIDGES, wx, 0, wz)};
        ColumnClimate cc;
        columnClimate(climate, cc);
        int top = tops[(qz * 4 + 2) * 16 + qx * 4 + 2];
        int topQuart = (top < SEA - 1 ? SEA - 1 : top) - c.minY();
        topQuart = topQuart < 0 ? 0 : topQuart >> 2;
        uint8_t b3[96];
        for (int qy = 0; qy < quarts && qy < 96; qy++)
            b3[qy] = biomeAtDepth(cc, quantizeClimate(r.sample(ROOT_DEPTH, wx, c.minY() + qy * 4, wz)));
        cell[q] = b3[topQuart < quarts ? topQuart : quarts - 1];
        c.setBiomeCell(qx, qz, cell[q]);
        for (int qy = 0; qy < quarts && qy < 96; qy++) {
            c.setBiome3d(qy * 16 + q, b3[qy]);
            cells3d[qy * 16 + q] = b3[qy];
        }
    }
}
}  // namespace

Generator::~Generator() {
    for (int i = 0; i < POOL; i++) {
        delete pool_[i];
        plat::bigFree(poolBuf_[i]);
        free(poolStates_[i]);
        free(poolBiomes_[i]);
    }
    if (poolLock_) plat::mutexDestroy(poolLock_);
}

// a router of the pool for this chunk (made on first use: ~60 KB PSRAM, ~9 KB internal RAM
// each, plus its 96 KB block buffer and 8 KB of states for one section)
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
                    poolStates_[i] = (uint16_t*)malloc(SECTION_BLOCKS * sizeof(uint16_t));
                    poolBiomes_[i] = (uint8_t*)malloc(16 * 96);
                    if (!pool_[i] || !poolBuf_[i] || !poolStates_[i] || !poolBiomes_[i] || !pool_[i]->init(seed_, 1.0f / 64, 0.1f, true, true)) {
                        delete pool_[i];
                        plat::bigFree(poolBuf_[i]);
                        free(poolStates_[i]);
                        free(poolBiomes_[i]);
                        pool_[i] = nullptr;
                        poolBuf_[i] = nullptr;
                        poolStates_[i] = nullptr;
                        poolBiomes_[i] = nullptr;
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
    uint64_t t0 = plat::micros();
    r.fillChunk(c.cx, c.cz, solid);
    uint64_t t1 = plat::micros();
    const int minY = vanilla::DF_SHAPE.minY, H = vanilla::DF_SHAPE.height;
    Rng rng(hash3(seed_, c.cx, c.cz, 404));
    // the random parts first, drawn in the order they always were (column by column,
    // upwards): bedrock thinning out over y -63..-60, deepslate blended over y 0..7. The
    // mask is read a row (a y) at a time: it is in PSRAM, a column's blocks 256 bytes apart.
    int tops[256];
    uint8_t bedrock[256], deep[256], low[256];   // a bit per y (bedrock: from minY; deep, low: from 0)
    memset(low, 0, sizeof(low));
    for (int y = 0; y < 8; y++) {
        const uint8_t* row = solid + (size_t)(y - minY) * 256;
        for (int col = 0; col < 256; col++)
            if (row[col]) low[col] |= (uint8_t)(1 << y);
    }
    for (int col = 0; col < 256; col++) {   // col = lz * 16 + lx: the order the columns always had
        uint8_t b = 1, d = 0;
        for (int k = 1; k <= 4; k++)
            if ((int)rng.range(5) >= k) b |= (uint8_t)(1 << k);
        for (int y = 0; y < 8; y++)
            if ((low[col] >> y & 1) && (int)rng.range(8) >= y) d |= (uint8_t)(1 << y);
        bedrock[col] = b;
        deep[col] = d;
        int top = minY;
        for (int k = 4; k > 0; k--)
            if (b & (1 << k)) { top = minY + k; break; }
        tops[col] = top;
    }
    // the highest solid block of each column, from the top down a row at a time
    int found = 0, highest = minY;
    uint8_t done[256];
    memset(done, 0, sizeof(done));
    for (int i = H - 1; i >= 0 && found < 256; i--) {
        const uint8_t* row = solid + (size_t)i * 256;
        for (int col = 0; col < 256; col++)
            if (!done[col] && row[col]) {
                done[col] = 1;
                found++;
                if (minY + i > tops[col]) tops[col] = minY + i;
            }
    }
    for (int col = 0; col < 256; col++)
        if (tops[col] > highest) highest = tops[col];
    uint64_t t2 = plat::micros();
    // biomes from the climate at the surface, per 4 x 4 cell
    uint8_t cell[16];
    uint8_t* cells3d = poolBiomes_[slot];
    if (version_ >= GENERATOR_VANILLA_BIOMES) vanillaBiomes(r, c, tops, cell, cells3d);
    else for (int q = 0; q < 16; q++) {
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
    uint64_t t3 = plat::micros();
    // version 6: vanilla's aquifers and ore veins; version 5: vanilla's surface rule (on the
    // mask: a block they place is 2 + its code, vanilla::maskState)
    vanilla::AquiferHandle* aquifer = version_ >= GENERATOR_VANILLA_AQUIFERS ? vanilla::aquiferOpen(r, seed_, c.cx, c.cz) : nullptr;
    if (aquifer) vanilla::fillAquifersAndVeins(aquifer, r, seed_, c.cx, c.cz, solid, tops);
    if (version_ >= GENERATOR_VANILLA_SURFACE) vanilla::buildSurface(r, seed_, c.cx, c.cz, solid, tops, cells3d);
    // version 7: vanilla's carvers (after the surface, as vanilla)
    if (version_ >= GENERATOR_VANILLA_CARVERS && aquifer) vanilla::carveChunk(seed_, c.cx, c.cz, solid, aquifer);
    vanilla::aquiferClose(aquifer);
    giveRouter(slot);
    uint64_t t4 = plat::micros();
    // then a section at a time: bedrock, stone and deepslate, water up to the sea, lava below
    // y -54, written whole (Section::assign)
    uint16_t* states = poolStates_[slot];
    for (int si = 0; si < c.numSections(); si++) {
        int y0 = c.sectionY(si);
        if (y0 > highest && y0 >= SEA) break;   // air from here up
        for (int dy = 0; dy < 16; dy++) {
            int y = y0 + dy, i = y - minY;
            const uint8_t* row = solid + (size_t)i * 256;
            uint16_t* out = states + dy * 256;
            uint16_t fluid = y < SEA ? (y < -54 ? bs::Lava : bs::Water) : (uint16_t)0;
            for (int col = 0; col < 256; col++) {
                uint16_t st;
                if (i <= 4 && (bedrock[col] >> i & 1)) st = bs::Bedrock;
                else if (row[col] >= 2) st = vanilla::maskState(row[col]);   // the surface rule's, the aquifer's, a vein's
                else if (row[col]) st = (y < 0 || (y < 8 && (deep[col] >> y & 1))) ? bs::Deepslate : bs::Stone;
                else st = fluid;
                out[col] = st;
            }
        }
        bool any = false;
        for (int k = 0; k < SECTION_BLOCKS && !any; k++) any = states[k] != 0;
        if (any) c.ensureSection(si)->assign(states);
    }
    // the heightmap: the top block, or the sea over it
    for (int col = 0; col < 256; col++)
        c.setHeight(col & 15, col >> 4, (tops[col] < SEA - 1 ? SEA - 1 : tops[col]) + 1);
    uint64_t t5 = plat::micros();
    // the surface (versions 3 and 4: provisional, by biome)
    ColumnInfo cols[256];
    for (int lz = 0; lz < 16; lz++)
        for (int lx = 0; lx < 16; lx++) {
            int top = tops[lz * 16 + lx];
            uint8_t b = cell[(lz >> 2) * 4 + (lx >> 2)];
            cols[lz * 16 + lx] = {(int16_t)top, b, false};
            if (top < minY + 1 || version_ >= GENERATOR_VANILLA_SURFACE) continue;
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
            } else if (b == biome::StonyPeaks || b == biome::FrozenPeaks || b == biome::JaggedPeaks ||
                       b == biome::StonyShore) {
                continue;   // bare stone (snow by the snow cover)
            } else if (b == biome::SnowySlopes || b == biome::IceSpikes) {
                topBlock = under = deep = bs::SnowBlock;
                depth = 1;
            } else if (b == biome::Badlands || b == biome::ErodedBadlands || b == biome::WoodedBadlands) {
                topBlock = bs::RedSand;   // the bands come with the surface rule (stage 3)
                under = deep = bs::Terracotta;
            } else if (b == biome::MushroomFields) {
                topBlock = bs::Mycelium;
            } else if (b == biome::OldGrowthPineTaiga || b == biome::OldGrowthSpruceTaiga) {
                topBlock = bs::Podzol;
            } else if (b == biome::WindsweptGravellyHills) {
                topBlock = under = bs::Gravel;
            } else if (b == biome::MangroveSwamp) {
                topBlock = under = bs::Mud;
            }
            for (int d = 0; d <= depth + 1 && top - d > minY; d++) {
                uint16_t at = c.get(lx, top - d, lz);
                if (at != bs::Stone && at != bs::Deepslate) break;
                c.set(lx, top - d, lz, d == 0 ? topBlock : d <= depth ? under : deep);
            }
        }
    uint64_t t6 = plat::micros();
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
        // version 4's other biomes (version 3 has none of these)
        case biome::FlowerForest: case biome::OldGrowthBirchForest: count = 6; break;
        case biome::OldGrowthPineTaiga: case biome::OldGrowthSpruceTaiga: count = 7; break;
        case biome::PaleGarden: count = 8; break;
        case biome::WindsweptForest: case biome::Grove: case biome::BambooJungle: count = 4; break;
        case biome::MangroveSwamp: count = 3; break;
        case biome::SparseJungle: case biome::WoodedBadlands: case biome::Swamp: case biome::CherryGrove: count = 2; break;
        case biome::WindsweptSavanna: case biome::SavannaPlateau: count = 1; break;
        case biome::SunflowerPlains: case biome::Meadow: case biome::WindsweptHills: count = rng.range(4) == 0 ? 1 : 0; break;
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
        else if (b == biome::OldGrowthBirchForest || b == biome::CherryGrove) kind = TREE_BIRCH;
        else if (b == biome::FlowerForest || b == biome::Meadow) kind = rng.range(3) == 0 ? TREE_BIRCH : TREE_OAK;
        else if (b == biome::OldGrowthPineTaiga || b == biome::OldGrowthSpruceTaiga || b == biome::Grove ||
                 b == biome::WindsweptForest || b == biome::WindsweptHills)
            kind = TREE_SPRUCE;
        else if (b == biome::SparseJungle || b == biome::BambooJungle) kind = TREE_JUNGLE;
        else if (b == biome::WindsweptSavanna || b == biome::SavannaPlateau) kind = TREE_ACACIA;
        else if (b == biome::PaleGarden) kind = TREE_DARK_OAK;
        placeTree(c, {c.cx * 16 + lx, c.cz * 16 + lz, ci.height + 1, kind});
    }
    decorate(c, cols, false);
    vanillaSteps = {(uint32_t)(t1 - t0), (uint32_t)(t5 - t4) + (uint32_t)(t2 - t1), (uint32_t)(t3 - t2),
                    (uint32_t)(t4 - t3) + (uint32_t)(t6 - t5), (uint32_t)(plat::micros() - t6)};
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
