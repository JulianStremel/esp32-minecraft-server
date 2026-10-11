// Generator version 9: the Nether and the End from vanilla's routers (noise_settings
// nether.json and end.json through the same engine as the overworld, mc/world/vanilla).
// - The Nether: the router's caverns, a lava sea below y 32, bedrock floor and roof
//   thinning out over five blocks as vanilla's gradients; its five biomes from the
//   temperature and vegetation noises (vanilla's parameter list: points with offsets);
//   the Nether's surface rule (soul sand valleys, nylium, basalt deltas, gravel and soul
//   sand by the lava sea); its features (quartz, gold, fire, glowstone, fungi, vines, ...).
// - The End: the router with end_islands: the main island and the outer islands beyond
//   1,000 blocks, the void between; vanilla's End biomes by chunk (highlands, midlands,
//   barrens, small islands); the generator's spikes on the main island; chorus plants.
#include <stdlib.h>
#include <string.h>
#include "mc/platform.h"
#include "mc/registry.h"
#include "mc/world/generator.h"
#include "mc/world/vanilla/density.h"
#include "mc/world/vanilla/features.h"
#include "mc/world/vanilla/structures.h"
#include "mc/world/vanilla/surface.h"
#include "mc/world/vanilla_probes.h"

namespace mc {

namespace {
uint64_t mix64(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
// a random float in [0, 1) for a block (vanilla's positional random of a gradient)
float blockRandom(uint64_t seed, int x, int y, int z, uint64_t salt) {
    uint64_t h = mix64(seed ^ salt ^ mix64((uint64_t)(uint32_t)x * 0x9E3779B97F4A7C15ull ^ (uint64_t)(uint32_t)y * 0xC2B2AE3D27D4EB4Full ^
                                           (uint64_t)(uint32_t)z * 0x165667B19E3779F9ull));
    return (float)(h >> 40) * (1.0f / 16777216.0f);
}
}  // namespace

const vanilla::RouterDef* Generator::routerDef() const {
    return dim_ == DIM_NETHER ? &vanilla::nether::ROUTER : dim_ == DIM_END ? &vanilla::end::ROUTER : nullptr;
}

void Generator::generateVanillaNether(Chunk& c) const {
    int slot = takeRouter();
    if (slot < 0) {
        c.setBiomeAll(biome::NetherWastes);
        return;
    }
    vanilla::Router& r = *pool_[slot];
    uint8_t* mask = poolBuf_[slot];
    r.fillChunk(c.cx, c.cz, mask);
    const vanilla::RouterShape& sh = r.def().shape;
    // biomes: the climate at each 4 x 4 column's corner
    uint8_t cell[16];
    for (int q = 0; q < 16; q++) {
        int wx = c.cx * 16 + (q & 3) * 4, wz = c.cz * 16 + (q >> 2) * 4;
        cell[q] = netherBiome(r.sample(vanilla::ROOT_TEMPERATURE, wx, 0, wz), r.sample(vanilla::ROOT_VEGETATION, wx, 0, wz));
        c.setBiomeCell(q & 3, q >> 2, cell[q]);
    }
    vanilla::buildSurfaceNether(r, seed_, c.cx, c.cz, mask, cell);
    // version 12: the fortresses reaching this chunk
    vanilla::StructureList* structs = nullptr;
    if (structures_) {
        NetherProbe probe(r);
        structs = structures_->find(c.cx, c.cz, probe);
    }
    giveRouter(slot);
    // the blocks, a section at a time
    uint16_t* states = poolStates_[slot];
    const int top = sh.minY + sh.height - 1;
    // the router covers the dimension's noise height only (the Nether's 128 of 256)
    for (int si = 0; si < c.numSections() && c.sectionY(si) < sh.minY + sh.height; si++) {
        int y0 = c.sectionY(si);
        for (int dy = 0; dy < 16; dy++) {
            int y = y0 + dy, i = y - sh.minY;
            const uint8_t* row = mask + (size_t)i * 256;
            uint16_t* out = states + dy * 256;
            uint16_t fluid = y < sh.seaLevel ? bs::Lava : (uint16_t)0;
            // vanilla's bedrock gradients: certain at y 0 and 127, thinning over five blocks
            float floorP = y <= sh.minY ? 1.0f : y >= sh.minY + 5 ? 0.0f : 1.0f - (float)(y - sh.minY) / 5.0f;
            float roofP = y >= top ? 1.0f : y <= top - 5 ? 0.0f : (float)(y - (top - 5)) / 5.0f;
            for (int col = 0; col < 256; col++) {
                int x = c.cx * 16 + (col & 15), z = c.cz * 16 + (col >> 4);
                uint16_t st;
                if ((floorP > 0 && blockRandom(seed_, x, y, z, 0xB0F1) < floorP) || (roofP > 0 && blockRandom(seed_, x, y, z, 0xB0F2) < roofP))
                    st = bs::Bedrock;
                else if (row[col] >= 2) st = vanilla::nether::SURFACE.states[row[col] - 2];
                else if (row[col]) st = bs::Netherrack;
                else st = fluid;
                out[col] = st;
            }
        }
        bool any = false;
        for (int k = 0; k < SECTION_BLOCKS && !any; k++) any = states[k] != 0;
        if (any) c.ensureSection(si)->assign(states);
    }
    if (structs) structures_->place(c, structs);   // before the features, as vanilla's steps
    c.recomputeHeightmap();
    vanilla::decorateChunk(c, seed_ ^ 0x4E7E4ull, nullptr, nullptr, true);
}

void Generator::generateVanillaEnd(Chunk& c) const {
    int slot = takeRouter();
    if (slot < 0) {
        c.setBiomeAll(biome::TheEnd);
        return;
    }
    vanilla::Router& r = *pool_[slot];
    uint8_t* mask = poolBuf_[slot];
    r.fillChunk(c.cx, c.cz, mask);
    const vanilla::RouterShape& sh = r.def().shape;
    // TheEndBiomeSource: the main island's chunks, else by the islands' height at the chunk's middle
    uint8_t b = biome::TheEnd;
    if ((int64_t)c.cx * c.cx + (int64_t)c.cz * c.cz > 4096) {
        float d = r.sample(vanilla::ROOT_EROSION, c.cx * 16 + 8, 0, c.cz * 16 + 8);
        b = d > 0.25f ? biome::EndHighlands : d >= -0.0625f ? biome::EndMidlands : d < -0.21875f ? biome::SmallEndIslands
                                                                                               : biome::EndBarrens;
    }
    c.setBiomeAll(b);
    giveRouter(slot);
    uint16_t* states = poolStates_[slot];
    for (int si = 0; si < c.numSections() && c.sectionY(si) < sh.minY + sh.height; si++) {
        int y0 = c.sectionY(si);
        bool any = false;
        for (int dy = 0; dy < 16; dy++) {
            const uint8_t* row = mask + (size_t)(y0 + dy - sh.minY) * 256;
            for (int col = 0; col < 256; col++) {
                states[dy * 256 + col] = row[col] ? bs::EndStone : (uint16_t)0;
                any |= row[col] != 0;
            }
        }
        if (any) c.ensureSection(si)->assign(states);
    }
    // the spikes stand on (and through) the main island
    if (abs(c.cx) <= 3 && abs(c.cz) <= 3) {
        EndSpike spikes[10];
        endSpikes(seed_, spikes);
        placeEndSpikes(c, spikes);
    }
    c.recomputeHeightmap();
    vanilla::decorateChunk(c, seed_ ^ 0xE4Dull, nullptr, nullptr, true);
}

// the End's highest block at a column (the exit portal's place), from the router
int Generator::vanillaEndSurfaceY(int x, int z) const {
    int slot = takeRouter();
    if (slot < 0) return 64;
    vanilla::Router& r = *pool_[slot];
    uint8_t* mask = poolBuf_[slot];
    int cx = x >> 4, cz = z >> 4, col = (z & 15) * 16 + (x & 15);
    r.fillChunk(cx, cz, mask);
    const vanilla::RouterShape& sh = r.def().shape;
    int y = 0;
    for (int i = sh.height - 1; i >= 0; i--)
        if (mask[(size_t)i * 256 + col]) {
            y = sh.minY + i + 1;
            break;
        }
    giveRouter(slot);
    return y;
}

}  // namespace mc
