// Terrain of the Nether and the End. Like the overworld, a pure function of (seed, x, z)
// in portable float arithmetic, so unmodified chunks are regenerated instead of stored.
#include <math.h>
#include <stdlib.h>
#include "mc/platform.h"
#include "mc/registry.h"
#include "mc/world/generator.h"

namespace mc {

// ------------------------------------------------------------------ Nether
// A 3D density field sampled on a 4 x 8 x 4 block lattice and interpolated, as in vanilla:
// solid where the density is above 0. It is biased to solid near the floor and the
// ceiling, the air below y = 32 is a lava sea, and bedrock closes both ends.
static constexpr int NETHER_TOP = 127;          // bedrock ceiling; nothing above it
static constexpr int NETHER_LAVA_TOP = 31;      // highest lava block of the sea
static constexpr Freq F_NETHER_XZ = {1, 96, 0.0f};
static constexpr Freq F_NETHER_Y = {1, 48, 0.0f};
static constexpr Freq F_NETHER2_XZ = {1, 32, 0.0f};
static constexpr Freq F_NETHER2_Y = {1, 20, 0.0f};
static constexpr Freq F_PATCH = {1, 24, 0.0f};

void Generator::generateNether(Chunk& c) const {
    const int GX = 5, GY = 17;   // lattice over 16 x 128 (cells of 4 x 8 x 4)
    float* d = (float*)plat::bigAlloc(sizeof(float) * GX * GX * GY);
    if (!d) {
        MC_LOGE("out of memory generating Nether chunk %d %d", (int)c.cx, (int)c.cz);
        abort();
    }
    for (int gx = 0; gx < GX; gx++)
        for (int gz = 0; gz < GX; gz++)
            for (int gy = 0; gy < GY; gy++) {
                int wx = c.cx * 16 + gx * 4, wy = gy * 8, wz = c.cz * 16 + gz * 4;
                float v = cave1_.v2.noise3(wx, wy, wz, F_NETHER_XZ, F_NETHER_Y) +
                          0.5f * cave2_.v2.noise3(wx, wy, wz, F_NETHER2_XZ, F_NETHER2_Y) - 0.12f;
                if (wy < 24) v += (float)(24 - wy) * (1.6f / 24.0f);      // floor
                if (wy > 92) v += (float)(wy - 92) * (1.6f / 35.0f);      // ceiling
                d[(gx * GX + gz) * GY + gy] = v;
            }
    for (int lx = 0; lx < 16; lx++)
        for (int lz = 0; lz < 16; lz++) {
            int gx = lx >> 2, gz = lz >> 2;
            float fx = (lx & 3) * 0.25f, fz = (lz & 3) * 0.25f;
            for (int y = 0; y <= NETHER_TOP; y++) {
                int gy = y >> 3;
                float fy = (y & 7) * 0.125f;
#define IDX(X, Z, Y) (((X) * GX + (Z)) * GY + (Y))
                float v0 = (d[IDX(gx, gz, gy)] * (1 - fx) + d[IDX(gx + 1, gz, gy)] * fx) * (1 - fz) +
                           (d[IDX(gx, gz + 1, gy)] * (1 - fx) + d[IDX(gx + 1, gz + 1, gy)] * fx) * fz;
                float v1 = (d[IDX(gx, gz, gy + 1)] * (1 - fx) + d[IDX(gx + 1, gz, gy + 1)] * fx) * (1 - fz) +
                           (d[IDX(gx, gz + 1, gy + 1)] * (1 - fx) + d[IDX(gx + 1, gz + 1, gy + 1)] * fx) * fz;
#undef IDX
                float v = v0 * (1 - fy) + v1 * fy;
                if (v > 0)
                    c.set(lx, y, lz, bs::Netherrack);
                else if (y <= NETHER_LAVA_TOP)
                    c.set(lx, y, lz, bs::Lava);
            }
        }
    plat::bigFree(d);

    Rng rng(hash3(seed_, c.cx, c.cz, 0x4E01));
    // bedrock: a full layer at both ends, ragged for 4 more
    for (int lx = 0; lx < 16; lx++)
        for (int lz = 0; lz < 16; lz++) {
            c.set(lx, 0, lz, bs::Bedrock);
            c.set(lx, NETHER_TOP, lz, bs::Bedrock);
            for (int k = 1; k <= 4; k++) {
                if (rng.range(5) < 5 - k) c.set(lx, k, lz, bs::Bedrock);
                if (rng.range(5) < 5 - k) c.set(lx, NETHER_TOP - k, lz, bs::Bedrock);
            }
        }
    // soul sand and gravel on the floors near the lava sea
    for (int lx = 0; lx < 16; lx++)
        for (int lz = 0; lz < 16; lz++) {
            int wx = c.cx * 16 + lx, wz = c.cz * 16 + lz;
            float soul = detail_.v2.fbm2(wx, wz, F_PATCH, 2), grav = hill_.v2.fbm2(wx, wz, F_PATCH, 2);
            if (soul < 0.3f && grav < 0.4f) continue;
            for (int y = 38; y >= 26; y--) {
                uint16_t above = c.get(lx, y + 1, lz);
                if (c.get(lx, y, lz) != bs::Netherrack || (above != bs::Air && above != bs::Lava)) continue;
                uint16_t s = soul >= 0.3f ? bs::SoulSand : bs::Gravel;
                if (s == bs::Gravel && (y < 30 || y > 34)) continue;
                for (int k = 0; k < 3 && c.get(lx, y - k, lz) == bs::Netherrack; k++) c.set(lx, y - k, lz, s);
                break;
            }
        }
    // ore veins in the netherrack
    struct Vein { uint16_t state; int tries, minY, maxY, size; };
    static const Vein veins[] = {
        {bs::NetherQuartzOre, 16, 10, 117, 14},
        {bs::NetherGoldOre, 10, 10, 117, 10},
        {bs::MagmaBlock, 4, 27, 36, 8},
    };
    for (const Vein& o : veins)
        for (int t = 0; t < o.tries; t++) {
            int x = rng.range(16), y = rng.between(o.minY, o.maxY), z = rng.range(16);
            for (int i = 0; i < o.size; i++) {
                if (x >= 0 && x < 16 && z >= 0 && z < 16 && c.get(x, y, z) == bs::Netherrack) c.set(x, y, z, o.state);
                switch (rng.range(6)) {
                    case 0: x++; break;
                    case 1: x--; break;
                    case 2: y++; break;
                    case 3: y--; break;
                    case 4: z++; break;
                    default: z--; break;
                }
            }
        }
    // glowstone clusters hanging from ceilings
    for (int t = 0; t < 2; t++) {
        int ox = rng.between(3, 12), oz = rng.between(3, 12), oy = -1;
        for (int y = NETHER_TOP - 6; y > 40; y--)
            if (c.get(ox, y, oz) == bs::Air && c.get(ox, y + 1, oz) == bs::Netherrack) {
                oy = y;
                break;
            }
        if (oy < 0) continue;
        c.set(ox, oy, oz, bs::Glowstone);
        for (int i = 0; i < 80; i++) {
            int x = ox + rng.between(-3, 3), y = oy - rng.range(6), z = oz + rng.between(-3, 3);
            if (x < 0 || x > 15 || z < 0 || z > 15 || c.get(x, y, z) != bs::Air) continue;
            int n = 0;
            static const int8_t D[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
            for (const auto& dd : D) {
                int nx = x + dd[0], nz = z + dd[2];
                if (nx >= 0 && nx < 16 && nz >= 0 && nz < 16 && c.get(nx, y + dd[1], nz) == bs::Glowstone) n++;
            }
            if (n == 1) c.set(x, y, z, bs::Glowstone);
        }
    }
    c.setBiomeAll(biome::NetherWastes);
}

// ------------------------------------------------------------------ End
// Vanilla's 10 spikes (SpikeFeature): their order comes from Java's Random seeded with
// the world seed, reproduced here so a seed's spikes stand where vanilla puts them.
namespace {
struct JavaRandom {
    uint64_t seed;
    explicit JavaRandom(int64_t s) : seed(((uint64_t)s ^ 0x5DEECE66DULL) & ((1ULL << 48) - 1)) {}
    int32_t next(int bits) {
        seed = (seed * 0x5DEECE66DULL + 0xBULL) & ((1ULL << 48) - 1);
        return (int32_t)(uint32_t)(seed >> (48 - bits));
    }
    int32_t nextInt(int32_t bound) {
        if ((bound & -bound) == bound) return (int32_t)(((int64_t)bound * (int64_t)next(31)) >> 31);
        int32_t bits, val;
        do {
            bits = next(31);
            val = bits % bound;
        } while (bits - val + (bound - 1) < 0);
        return val;
    }
    int64_t nextLong() {
        int64_t hi = next(32), lo = next(32);
        return (int64_t)((uint64_t)hi << 32) + lo;
    }
};
}  // namespace

void endSpikes(uint64_t worldSeed, EndSpike out[10]) {
    JavaRandom r((int64_t)worldSeed);
    int64_t key = r.nextLong() & 0xFFFF;
    int order[10];
    for (int i = 0; i < 10; i++) order[i] = i;
    JavaRandom sh(key);   // Collections.shuffle
    for (int i = 9; i > 0; i--) {
        int j = sh.nextInt(i + 1);
        int t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    for (int i = 0; i < 10; i++) {
        double a = 2.0 * (-3.141592653589793 + 0.3141592653589793 * i);
        out[i].x = (int)floor(42.0 * cos(a));
        out[i].z = (int)floor(42.0 * sin(a));
        int k = order[i];
        out[i].radius = 2 + k / 3;
        out[i].height = 76 + k * 3;
        out[i].guarded = k == 1 || k == 2;
    }
}

// The spikes' blocks in chunk c: an obsidian column (radius r: dx^2 + dz^2 <= r^2 + 1)
// from y 0 to its height, bedrock on top (where the crystal stands), a cage of iron bars
// around the top of the guarded ones.
static void placeSpikes(Chunk& c, const EndSpike* spikes) {
    for (int i = 0; i < 10; i++) {
        const EndSpike& s = spikes[i];
        int r = s.radius + 3;
        if (s.x + r < c.cx * 16 || s.x - r > c.cx * 16 + 15 || s.z + r < c.cz * 16 || s.z - r > c.cz * 16 + 15) continue;
        for (int lx = 0; lx < 16; lx++)
            for (int lz = 0; lz < 16; lz++) {
                int dx = c.cx * 16 + lx - s.x, dz = c.cz * 16 + lz - s.z;
                if (dx * dx + dz * dz <= s.radius * s.radius + 1)
                    for (int y = 0; y < s.height; y++) c.set(lx, y, lz, bs::Obsidian);
                if (dx == 0 && dz == 0) c.set(lx, s.height, lz, bs::Bedrock);
                if (s.guarded && abs(dx) <= 2 && abs(dz) <= 2) {
                    for (int y = 0; y <= 3; y++) {
                        bool side = abs(dx) == 2 || abs(dz) == 2, top = y == 3;
                        if (!side && !top) continue;
                        uint16_t st = BLOCKS[blk::IronBars].defState;
                        bool ns = dx == -2 || dx == 2 || top, ew = dz == -2 || dz == 2 || top;
                        st = setPropStr(st, "north", ns && dz != -2 ? "true" : "false");
                        st = setPropStr(st, "south", ns && dz != 2 ? "true" : "false");
                        st = setPropStr(st, "west", ew && dx != -2 ? "true" : "false");
                        st = setPropStr(st, "east", ew && dx != 2 ? "true" : "false");
                        c.set(lx, s.height + y, lz, st);
                    }
                }
            }
    }
}

// The main island: end stone around (0, 0), its rim wobbled by noise, flat-ish on top and
// tapering to a point below; the spikes on it. Void everywhere else (outer islands come
// later).
static constexpr Freq F_END_RIM = {1, 48, 0.0f};
static constexpr Freq F_END_TOP = {1, 24, 0.0f};

int Generator::endSurfaceY(int wx, int wz) const {
    float dist = sqrtf((float)(wx * wx + wz * wz));
    float r = 80.0f + 20.0f * cont_.v2.fbm2(wx, wz, F_END_RIM, 2);
    if (dist >= r) return 0;
    float e = dist / r;
    return 57 + (int)floorf(5.0f * (1 - e) + 2.0f * detail_.v2.fbm2(wx, wz, F_END_TOP, 2)) + 1;
}

void Generator::generateEnd(Chunk& c) const {
    c.setBiomeAll(biome::TheEnd);
    int32_t nx = c.cx * 16, nz = c.cz * 16;
    // the nearest point of this chunk to the origin
    int32_t px = nx > 0 ? nx : (nx + 15 < 0 ? nx + 15 : 0), pz = nz > 0 ? nz : (nz + 15 < 0 ? nz + 15 : 0);
    if ((int64_t)px * px + (int64_t)pz * pz > 120 * 120) return;
    for (int lx = 0; lx < 16; lx++)
        for (int lz = 0; lz < 16; lz++) {
            int wx = nx + lx, wz = nz + lz;
            float dist = sqrtf((float)(wx * wx + wz * wz));
            float r = 80.0f + 20.0f * cont_.v2.fbm2(wx, wz, F_END_RIM, 2);
            if (dist >= r) continue;
            float e = dist / r;                       // 0 at the centre, 1 at the rim
            int top = 57 + (int)floorf(5.0f * (1 - e) + 2.0f * detail_.v2.fbm2(wx, wz, F_END_TOP, 2));
            int depth = 3 + (int)(40.0f * sqrtf(1 - e * e));
            for (int y = top - depth; y <= top; y++)
                if (y > 0) c.set(lx, y, lz, bs::EndStone);
        }
    // the spikes stand on (and through) the island (a feature after the terrain)
    if (abs(c.cx) <= 3 && abs(c.cz) <= 3) {
        EndSpike spikes[10];
        endSpikes(seed_, spikes);
        placeSpikes(c, spikes);
    }
}

// ------------------------------------------------------------------ fingerprints
static const int32_t DIM_FINGERPRINT_AT[][2] = {
    {0, 0}, {-1, -1}, {3, -2}, {-4, 4}, {5, 0}, {100, -50}, {62500, -62500}, {-233593, 233593},
    {2, 0}, {-3, -1},   // End spikes: (42, 0) and (-42, -1) (z = floor(42 sin(-pi)): the sign of a rounding error)
};

uint32_t generatorDimFingerprint(uint64_t seed, uint8_t dim) {
    Generator g;
    if (!g.init(seed, WORLD_NORMAL, GENERATOR_LATEST, dim)) return 0;
    uint32_t h = 2166136261u;
    auto mix = [&h](uint32_t v) {
        for (int i = 0; i < 4; i++) {
            h ^= (v >> (8 * i)) & 0xFF;
            h *= 16777619u;
        }
    };
    for (const auto& a : DIM_FINGERPRINT_AT) {
        Chunk* c = new Chunk(a[0], a[1], dim);
        if (!c) return 0;
        g.generate(*c);
        for (int y = c->minY(); y <= c->maxY(); y++)
            for (int z = 0; z < 16; z++)
                for (int x = 0; x < 16; x++) mix(c->get(x, y, z));
        for (int z = 0; z < 16; z++)
            for (int x = 0; x < 16; x++) mix((uint32_t)c->height(x, z) | (uint32_t)c->biome(x, z) << 16);
        delete c;
    }
    return h;
}

// computed by the PC build; a change means existing worlds' unmodified chunks change
const GeneratorDimGolden GENERATOR_DIM_GOLDEN[] = {
    {42, 0xe3966769u, 0xf664f826u},
    {1, 0x0d0157d8u, 0x06a5ce10u},
    {0xDEADBEEFull, 0xf553cc71u, 0x3263212fu},
};
const int NUM_GENERATOR_DIM_GOLDEN = (int)(sizeof(GENERATOR_DIM_GOLDEN) / sizeof(GENERATOR_DIM_GOLDEN[0]));

}  // namespace mc
