#include "mc/world/generator.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "mc/platform.h"
#include "mc/registry.h"

namespace mc {

static const int WATER_TOP = SEA_LEVEL - 1;  // highest water block of the sea (y = 62)

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float smooth(float e0, float e1, float x) {
    float t = clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

// noise frequencies (cycles per block), exact fractions plus the version-1 float literal
static constexpr Freq F_CONT = {1, 625, 0.0016f};
static constexpr Freq F_HILL = {3, 400, 0.0075f};
static constexpr Freq F_DETAIL = {9, 200, 0.045f};
static constexpr Freq F_MOUNT = {7, 2000, 0.0035f};
static constexpr Freq F_RIVER = {21, 10000, 0.0021f};
static constexpr Freq F_TEMP = {11, 10000, 0.0011f};
static constexpr Freq F_HUMID = {13, 10000, 0.0013f};
static constexpr Freq F_TUNNEL_XZ = {11, 500, 0.022f};
static constexpr Freq F_TUNNEL_Y = {7, 200, 0.035f};
static constexpr Freq F_CAVERN_XZ = {3, 250, 0.012f};
static constexpr Freq F_CAVERN_Y = {11, 500, 0.022f};

float Generator::fbm(const Layer& l, int x, int z, const Freq& f, int octaves) const {
    if (version_ < 2) return l.v1.fbm2((float)x * f.approx, (float)z * f.approx, octaves);
    return l.v2.fbm2(x, z, f, octaves);
}

float Generator::ridged(const Layer& l, int x, int z, const Freq& f, int octaves) const {
    if (version_ < 2) return l.v1.ridged2((float)x * f.approx, (float)z * f.approx, octaves);
    return l.v2.ridged2(x, z, f, octaves);
}

float Generator::noise3(const Layer& l, int x, int y, int z, const Freq& fxz, const Freq& fy) const {
    if (version_ < 2) return l.v1.noise3((float)x * fxz.approx, (float)y * fy.approx, (float)z * fxz.approx);
    return l.v2.noise3(x, y, z, fxz, fy);
}

bool Generator::init(uint64_t seed, WorldType type, uint8_t version) {
    if (version < 1 || version > GENERATOR_LATEST) return false;
    seed_ = seed;
    type_ = type;
    version_ = version;
    cont_.init(seed ^ 0x1001);
    hill_.init(seed ^ 0x2002);
    detail_.init(seed ^ 0x3003);
    mount_.init(seed ^ 0x4004);
    river_.init(seed ^ 0x5005);
    temp_.init(seed ^ 0x6006);
    humid_.init(seed ^ 0x7007);
    cave1_.init(seed ^ 0x8008);
    cave2_.init(seed ^ 0x9009);
    cave3_.init(seed ^ 0xA00A);
    return true;
}

// ------------------------------------------------------------------ columns
ColumnInfo Generator::column(int x, int z, float* raw) const {
    ColumnInfo ci;
    ci.river = false;
    if (raw) raw[0] = raw[1] = raw[2] = 0;
    if (type_ == WORLD_FLAT) { ci.height = 3; ci.biome = biome::Plains; return ci; }
    if (type_ == WORLD_VOID) { ci.height = -1; ci.biome = biome::TheVoid; return ci; }
    float cont = fbm(cont_, x, z, F_CONT, 4) * 1.7f + 0.12f;
    float hills = fbm(hill_, x, z, F_HILL, 4);
    float detail = fbm(detail_, x, z, F_DETAIL, 2);
    float h;
    float land = 0;
    if (cont < -0.12f) {
        float t = cont + 0.12f;  // < 0
        h = WATER_TOP - 3 + t * 42.0f + hills * 4.0f;
    } else {
        land = cont + 0.12f;
        h = WATER_TOP + 1 + land * 12.0f + hills * (3.0f + land * 13.0f) + detail * 1.2f;
        float m = ridged(mount_, x, z, F_MOUNT, 4);
        h += smooth(0.58f, 0.86f, m) * smooth(0.08f, 0.45f, land) * 62.0f;
    }
    // rivers: narrow valleys along the zero line of a low-frequency noise
    float r = fabsf(fbm(river_, x, z, F_RIVER, 3));
    const float RW = 0.035f;
    if (land > 0.02f && r < RW * 1.8f) {
        float k = smooth(RW * 1.8f, RW * 0.6f, r);
        float bed = WATER_TOP - 3 - (1 - r / RW) * 2.0f;
        if (r < RW) {
            h = h + (bed - h) * k;
            ci.river = h < WATER_TOP + 0.5f;
        } else {
            float bank = WATER_TOP + 1.5f;
            if (h > bank) h = h + (bank - h) * k;
        }
    }
    int hi = (int)floorf(h);
    if (hi < 6) hi = 6;
    if (hi > 245) hi = 245;
    ci.height = (int16_t)hi;

    float temp = fbm(temp_, x, z, F_TEMP, 3) * 1.4f - (hi > 100 ? (hi - 100) * 0.006f : 0);
    float hum = fbm(humid_, x, z, F_HUMID, 3) * 1.4f;
    uint8_t b;
    if (ci.river) {
        b = temp < -0.45f ? biome::FrozenRiver : biome::River;
    } else if (hi < WATER_TOP) {
        int depth = WATER_TOP - hi;
        if (temp < -0.45f) b = biome::FrozenOcean;
        else if (temp < -0.15f) b = depth > 18 ? biome::DeepColdOcean : biome::ColdOcean;
        else if (temp > 0.45f) b = biome::WarmOcean;
        else if (temp > 0.2f) b = biome::LukewarmOcean;
        else b = depth > 18 ? biome::DeepOcean : biome::Ocean;
    } else if (hi <= WATER_TOP + 2 && cont < 0.05f) {
        b = temp < -0.45f ? biome::SnowyBeach : biome::Beach;
    } else if (hi > WATER_TOP + 42) {
        b = biome::Mountains;
    } else if (temp < -0.45f) {
        b = hum > 0.0f ? biome::SnowyTaiga : biome::SnowyTundra;
    } else if (temp < -0.15f) {
        b = hum > -0.05f ? biome::Taiga : biome::Plains;
    } else if (temp < 0.3f) {
        if (hum > 0.4f) b = hi < WATER_TOP + 5 ? biome::Swamp : biome::DarkForest;
        else if (hum > 0.12f) b = biome::Forest;
        else if (hum > -0.08f) b = biome::BirchForest;
        else b = detail > 0.55f ? biome::SunflowerPlains : biome::Plains;
    } else {
        if (hum < -0.15f) b = temp > 0.65f ? biome::Badlands : biome::Desert;
        else if (hum < 0.25f) b = biome::Savanna;
        else b = biome::Jungle;
    }
    ci.biome = b;
    if (raw) {
        raw[0] = h;
        raw[1] = temp;
        raw[2] = hum;
    }
    return ci;
}

uint32_t Generator::floatHash(int x, int z, uint32_t h) const {
    auto mix = [&h](float f) {
        uint32_t v;
        memcpy(&v, &f, 4);
        for (int i = 0; i < 4; i++) {
            h ^= (v >> (8 * i)) & 0xFF;
            h *= 16777619u;
        }
    };
    float raw[3];
    column(x, z, raw);
    for (float f : raw) mix(f);
    mix(fbm(cont_, x, z, F_CONT, 4));
    mix(fbm(hill_, x, z, F_HILL, 4));
    mix(fbm(detail_, x, z, F_DETAIL, 2));
    mix(ridged(mount_, x, z, F_MOUNT, 4));
    mix(fbm(river_, x, z, F_RIVER, 3));
    mix(fbm(temp_, x, z, F_TEMP, 3));
    mix(fbm(humid_, x, z, F_HUMID, 3));
    for (int y = 9; y < 128; y += 37) {
        mix(noise3(cave1_, x, y, z, F_TUNNEL_XZ, F_TUNNEL_Y));
        mix(noise3(cave2_, x, y, z, F_TUNNEL_XZ, F_TUNNEL_Y));
        mix(noise3(cave3_, x, y, z, F_CAVERN_XZ, F_CAVERN_Y));
    }
    return h;
}

static bool isSnowy(uint8_t b) {
    return b == biome::SnowyTundra || b == biome::SnowyTaiga || b == biome::SnowyBeach || b == biome::FrozenRiver ||
           b == biome::FrozenOcean;
}
static bool isOceanish(uint8_t b) {
    return b == biome::Ocean || b == biome::DeepOcean || b == biome::FrozenOcean || b == biome::ColdOcean ||
           b == biome::DeepColdOcean || b == biome::LukewarmOcean || b == biome::WarmOcean;
}

// ------------------------------------------------------------------ terrain fill
void Generator::fillColumns(Chunk& c, ColumnInfo* cols) const {
    int minH = 255;
    for (int lz = 0; lz < 16; lz++)
        for (int lx = 0; lx < 16; lx++) {
            ColumnInfo ci = column(c.cx * 16 + lx, c.cz * 16 + lz);
            cols[lz * 16 + lx] = ci;
            if (ci.height < minH) minH = ci.height;
        }
    // Sections fully below the lowest surface (minus filler depth) start as uniform stone.
    int solidSections = (minH - 6) / 16;
    for (int s = 0; s < solidSections && s < NUM_SECTIONS; s++) c.ensureSection(s)->fill(bs::Stone);

    Rng rng(hash3(seed_, c.cx, c.cz, 17));
    uint16_t snowyGrass = setBool(bs::GrassBlock, "snowy", true);
    for (int lz = 0; lz < 16; lz++) {
        for (int lx = 0; lx < 16; lx++) {
            const ColumnInfo& ci = cols[lz * 16 + lx];
            int h = ci.height;
            uint8_t b = ci.biome;
            bool under = h < WATER_TOP;
            uint16_t top = bs::GrassBlock, filler = bs::Dirt;
            int depth = 3 + rng.range(2);
            if (b == biome::Desert || b == biome::Beach || b == biome::WarmOcean || b == biome::SnowyBeach) {
                top = bs::Sand; filler = bs::Sand; depth = 4;
            } else if (b == biome::Badlands) {
                top = bs::RedSand; filler = bs::Terracotta; depth = 6;
            } else if (b == biome::River || b == biome::FrozenRiver) {
                top = (rng.range(4) == 0) ? bs::Clay : (rng.range(2) ? bs::Sand : bs::Gravel);
                filler = bs::Dirt;
            } else if (isOceanish(b)) {
                int d = WATER_TOP - h;
                top = d > 16 ? bs::Gravel : (rng.range(6) == 0 ? bs::Clay : bs::Sand);
                filler = top == bs::Clay ? bs::Sand : top;
            } else if (b == biome::Mountains) {
                if (h > 118) { top = bs::Stone; filler = bs::Stone; }
                else if (rng.range(10) == 0) { top = bs::Gravel; filler = bs::Gravel; }
            } else if (isSnowy(b)) {
                top = snowyGrass;
            }
            if (under && top == bs::GrassBlock) top = bs::Dirt;
            if (under && top == snowyGrass) top = bs::Dirt;
            int start = solidSections * 16;  // below this the section was pre-filled with stone
            for (int y = start; y <= h; y++) {
                uint16_t s;
                if (y == h) s = top;
                else if (y > h - depth) s = filler;
                else if (b == biome::Desert && y > h - depth - 3) s = bs::Sandstone;
                else if (b == biome::Badlands && y > h - 14) {
                    static const uint16_t bands[] = {bs::Terracotta, bs::OrangeTerracotta, bs::YellowTerracotta,
                                                     bs::Terracotta, bs::WhiteTerracotta, bs::RedTerracotta,
                                                     bs::BrownTerracotta, bs::Terracotta};
                    s = bands[(y / 2) & 7];
                } else s = bs::Stone;
                c.set(lx, y, lz, s);
            }
            for (int y = h + 1; y <= WATER_TOP; y++) c.set(lx, y, lz, bs::Water);
            if (h < WATER_TOP && isSnowy(b)) c.set(lx, WATER_TOP, lz, bs::Ice);
            // bedrock floor
            c.set(lx, 0, lz, bs::Bedrock);
            for (int y = 1; y <= 4; y++)
                if (rng.range(5) >= y) c.set(lx, y, lz, bs::Bedrock);
        }
    }
    for (int cz4 = 0; cz4 < 4; cz4++)
        for (int cx4 = 0; cx4 < 4; cx4++) c.setBiomeCell(cx4, cz4, cols[(cz4 * 4 + 1) * 16 + cx4 * 4 + 1].biome);
}

// ------------------------------------------------------------------ caves
// Two "spaghetti" noises whose joint zero set forms tunnels, plus rare caverns.
// Noise is sampled on a 4x4x4 lattice and trilinearly interpolated.
void Generator::carveCaves(Chunk& c, const ColumnInfo* cols) const {
    const int GX = 5, GY = 33;  // lattice over 16 x 128 (y 0..128)
    // heap, not static: keeps ~10 KB out of the ESP32's permanent DRAM. Out of memory is
    // fatal: a chunk without its caves would differ from the same chunk elsewhere.
    float* lattice = (float*)plat::bigAlloc(sizeof(float) * GX * GX * GY * 3);
    if (!lattice) {
        MC_LOGE("out of memory generating the caves of chunk %d %d", (int)c.cx, (int)c.cz);
        abort();
    }
    float* a = lattice;
    float* b2 = lattice + GX * GX * GY;
    float* cv = lattice + 2 * GX * GX * GY;
    int maxH = 0;
    for (int i = 0; i < 256; i++)
        if (cols[i].height > maxH) maxH = cols[i].height;
    int topY = maxH + 1 < 128 ? maxH + 1 : 128;
    int gyMax = (topY + 3) / 4;
    if (gyMax >= GY) gyMax = GY - 1;
    for (int gx = 0; gx < GX; gx++)
        for (int gz = 0; gz < GX; gz++)
            for (int gy = 0; gy <= gyMax; gy++) {
                int wx = c.cx * 16 + gx * 4, wy = gy * 4, wz = c.cz * 16 + gz * 4;
                int i = (gx * GX + gz) * GY + gy;
                a[i] = noise3(cave1_, wx, wy, wz, F_TUNNEL_XZ, F_TUNNEL_Y);
                b2[i] = noise3(cave2_, wx, wy, wz, F_TUNNEL_XZ, F_TUNNEL_Y);
                cv[i] = noise3(cave3_, wx, wy, wz, F_CAVERN_XZ, F_CAVERN_Y);
            }
    for (int lx = 0; lx < 16; lx++) {
        for (int lz = 0; lz < 16; lz++) {
            const ColumnInfo& ci = cols[lz * 16 + lx];
            int limit = ci.height < WATER_TOP + 1 ? ci.height - 8 : ci.height;  // never breach sea floors
            if (ci.river) limit = ci.height - 6;
            if (limit > topY) limit = topY;
            int gx = lx >> 2, gz = lz >> 2;
            float fx = (lx & 3) / 4.0f, fz = (lz & 3) / 4.0f;
            for (int y = 5; y < limit; y++) {
                int gy = y >> 2;
                if (gy >= gyMax) break;
                float fy = (y & 3) / 4.0f;
#define IDX(X, Z, Y) (((X) * GX + (Z)) * GY + (Y))
#define TRI(arr)                                                                                       \
    ((((arr[IDX(gx, gz, gy)] * (1 - fx) + arr[IDX(gx + 1, gz, gy)] * fx) * (1 - fz) +                   \
       (arr[IDX(gx, gz + 1, gy)] * (1 - fx) + arr[IDX(gx + 1, gz + 1, gy)] * fx) * fz) * (1 - fy)) +       \
     (((arr[IDX(gx, gz, gy + 1)] * (1 - fx) + arr[IDX(gx + 1, gz, gy + 1)] * fx) * (1 - fz) +             \
       (arr[IDX(gx, gz + 1, gy + 1)] * (1 - fx) + arr[IDX(gx + 1, gz + 1, gy + 1)] * fx) * fz) * fy))
                float va = TRI(a), vb = TRI(b2), vc = TRI(cv);
#undef TRI
#undef IDX
                bool tunnel = va * va + vb * vb < 0.0042f;
                bool cavern = vc > 0.42f && y < 50;
                if (!tunnel && !cavern) continue;
                uint16_t cur = c.get(lx, y, lz);
                if (cur == bs::Bedrock || cur == bs::Water || cur == 0) continue;
                // don't undercut water / sand that would float in the air
                uint16_t above = c.get(lx, y + 1, lz);
                if (above == bs::Water) continue;
                c.set(lx, y, lz, y <= 10 ? bs::Lava : bs::CaveAir);
                // expose grass to dirt below carved surface holes
                if (y == ci.height - 1) {
                    uint16_t t = c.get(lx, y + 1, lz);
                    if (t == bs::Dirt) c.set(lx, y + 1, lz, bs::GrassBlock);
                }
            }
        }
    }
    plat::bigFree(lattice);
}

// ------------------------------------------------------------------ ores
void Generator::placeOres(Chunk& c) const {
    struct OreDef { uint16_t state; int tries, minY, maxY, size; };
    static const OreDef ores[] = {
        {bs::Dirt, 7, 5, 120, 20},      {bs::Gravel, 6, 5, 120, 20},    {bs::Granite, 6, 5, 80, 24},
        {bs::Diorite, 6, 5, 80, 24},    {bs::Andesite, 6, 5, 80, 24},   {bs::CoalOre, 18, 5, 127, 12},
        {bs::IronOre, 18, 5, 63, 8},    {bs::GoldOre, 2, 5, 31, 8},     {bs::RedstoneOre, 7, 5, 15, 7},
        {bs::DiamondOre, 1, 5, 15, 7},  {bs::LapisOre, 1, 5, 30, 6},
    };
    Rng rng(hash3(seed_, c.cx, c.cz, 29));
    for (const OreDef& o : ores) {
        for (int t = 0; t < o.tries; t++) {
            int x = rng.range(16), y = rng.between(o.minY, o.maxY), z = rng.range(16);
            for (int i = 0; i < o.size; i++) {
                if (x >= 0 && x < 16 && z >= 0 && z < 16 && y > 0 && y < WORLD_HEIGHT) {
                    uint16_t cur = c.get(x, y, z);
                    if (cur == bs::Stone || cur == bs::Granite || cur == bs::Diorite || cur == bs::Andesite)
                        c.set(x, y, z, o.state);
                }
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
    }
    // emeralds in mountains
    if (c.biome(8, 8) == biome::Mountains) {
        for (int t = 0; t < 4; t++) {
            int x = rng.range(16), y = rng.between(4, 31), z = rng.range(16);
            if (c.get(x, y, z) == bs::Stone) c.set(x, y, z, bs::EmeraldOre);
        }
    }
}

// ------------------------------------------------------------------ trees
int Generator::treeSites(int cx, int cz, TreeSite* out, int max) const {
    Rng rng(hash3(seed_, cx, cz, 101));
    ColumnInfo center = column(cx * 16 + 8, cz * 16 + 8);
    int count = 0;
    switch (center.biome) {
        case biome::Forest: count = 7; break;
        case biome::DarkForest: count = 11; break;
        case biome::BirchForest: count = 7; break;
        case biome::Taiga: count = 6; break;
        case biome::SnowyTaiga: count = 4; break;
        case biome::Jungle: count = 10; break;
        case biome::Swamp: count = 2; break;
        case biome::Savanna: count = rng.range(3) == 0 ? 1 : 0; break;
        case biome::Plains: count = rng.range(5) == 0 ? 1 : 0; break;
        case biome::SnowyTundra: count = rng.range(6) == 0 ? 1 : 0; break;
        case biome::Mountains: count = rng.range(2); break;
        default: count = 0; break;
    }
    int n = 0;
    for (int i = 0; i < count && n < max; i++) {
        int x = cx * 16 + rng.range(16), z = cz * 16 + rng.range(16);
        ColumnInfo ci = column(x, z);
        if (ci.height < WATER_TOP + 1 || ci.river) continue;
        uint8_t b = ci.biome;
        if (b == biome::Beach || b == biome::SnowyBeach || b == biome::Desert || b == biome::Badlands) continue;
        if (b == biome::Mountains && ci.height > 118) continue;
        uint8_t kind = TREE_OAK;
        if (b == biome::BirchForest) kind = TREE_BIRCH;
        else if (b == biome::Forest) kind = rng.range(5) == 0 ? TREE_BIRCH : TREE_OAK;
        else if (b == biome::Taiga || b == biome::SnowyTaiga || b == biome::SnowyTundra || b == biome::Mountains) kind = TREE_SPRUCE;
        else if (b == biome::Jungle) kind = TREE_JUNGLE;
        else if (b == biome::Savanna) kind = TREE_ACACIA;
        else if (b == biome::DarkForest) kind = TREE_DARK_OAK;
        out[n++] = {x, z, ci.height + 1, kind};
    }
    return n;
}

void Generator::placeTree(Chunk& c, const TreeSite& t) const {
    Rng rng(hash3(seed_, t.x, t.z, 202));
    uint16_t log = bs::OakLog, leaves = bs::OakLeaves;
    int height = 4 + rng.range(3);
    switch (t.kind) {
        case TREE_BIRCH: log = bs::BirchLog; leaves = bs::BirchLeaves; height = 5 + rng.range(3); break;
        case TREE_SPRUCE: log = bs::SpruceLog; leaves = bs::SpruceLeaves; height = 6 + rng.range(4); break;
        case TREE_JUNGLE: log = bs::JungleLog; leaves = bs::JungleLeaves; height = 6 + rng.range(6); break;
        case TREE_ACACIA: log = bs::AcaciaLog; leaves = bs::AcaciaLeaves; height = 5 + rng.range(2); break;
        case TREE_DARK_OAK: log = bs::DarkOakLog; leaves = bs::DarkOakLeaves; height = 5 + rng.range(2); break;
        default: break;
    }
    leaves = setBool(leaves, "persistent", false);
    leaves = setProp(leaves, "distance", 0);  // value index 0 == distance 1
    int ox = c.cx * 16, oz = c.cz * 16;
    auto put = [&](int wx, int y, int wz, uint16_t s, bool onlyAir) {
        int lx = wx - ox, lz = wz - oz;
        if (lx < 0 || lx > 15 || lz < 0 || lz > 15 || y < 1 || y >= WORLD_HEIGHT) return;
        if (onlyAir) {
            uint16_t cur = c.get(lx, y, lz);
            if (!stateIsAir(cur) && !(blockOf(cur).flags & BF_REPLACEABLE)) return;
            if (cur == bs::Water) return;
        }
        c.set(lx, y, lz, s);
    };
    int top = t.y + height - 1;
    if (t.kind == TREE_SPRUCE) {
        static const int8_t radius[] = {0, 1, 1, 2, 1, 2, 1, 2, 2, 3};
        int i = 0;
        for (int y = top + 1; y >= t.y + 2; y--, i++) {
            int r = radius[i < 10 ? i : 9];
            for (int dx = -r; dx <= r; dx++)
                for (int dz = -r; dz <= r; dz++)
                    if (abs(dx) + abs(dz) <= r + (r > 1 ? 1 : 0)) put(t.x + dx, y, t.z + dz, leaves, true);
        }
    } else if (t.kind == TREE_ACACIA) {
        for (int dx = -2; dx <= 2; dx++)
            for (int dz = -2; dz <= 2; dz++) {
                if (abs(dx) == 2 && abs(dz) == 2) continue;
                put(t.x + dx, top, t.z + dz, leaves, true);
                if (abs(dx) <= 1 && abs(dz) <= 1) put(t.x + dx, top + 1, t.z + dz, leaves, true);
            }
    } else {
        int rad = t.kind == TREE_JUNGLE || t.kind == TREE_DARK_OAK ? 3 : 2;
        for (int y = top - 2; y <= top + 1; y++) {
            int r = y >= top ? 1 : rad;
            for (int dx = -r; dx <= r; dx++)
                for (int dz = -r; dz <= r; dz++) {
                    bool corner = abs(dx) == r && abs(dz) == r;
                    if (corner && (y >= top || rng.range(2) == 0)) continue;
                    put(t.x + dx, y, t.z + dz, leaves, true);
                }
        }
    }
    for (int y = t.y; y <= top; y++) put(t.x, y, t.z, log, false);
    put(t.x, t.y - 1, t.z, bs::Dirt, false);
}

// ------------------------------------------------------------------ decoration
void Generator::decorate(Chunk& c, const ColumnInfo* cols) const {
    TreeSite sites[16];
    for (int dz = -1; dz <= 1; dz++)
        for (int dx = -1; dx <= 1; dx++) {
            int n = treeSites(c.cx + dx, c.cz + dz, sites, 16);
            for (int i = 0; i < n; i++) placeTree(c, sites[i]);
        }
    Rng rng(hash3(seed_, c.cx, c.cz, 303));
    static const uint16_t flowers[] = {bs::Dandelion, bs::Poppy, bs::Cornflower, bs::OxeyeDaisy, bs::AzureBluet};
    for (int lz = 0; lz < 16; lz++) {
        for (int lx = 0; lx < 16; lx++) {
            const ColumnInfo& ci = cols[lz * 16 + lx];
            int h = ci.height;
            if (h + 1 >= WORLD_HEIGHT) continue;
            uint16_t ground = c.get(lx, h, lz);
            if (!stateIsAir(c.get(lx, h + 1, lz))) continue;
            uint8_t b = ci.biome;
            float p = rng.unit();
            if (blockIdOf(ground) == blk::GrassBlock) {
                float grass = 0, flower = 0;
                switch (b) {
                    case biome::Plains: grass = 0.12f; flower = 0.012f; break;
                    case biome::SunflowerPlains: grass = 0.1f; flower = 0.03f; break;
                    case biome::Forest: case biome::BirchForest: grass = 0.08f; flower = 0.01f; break;
                    case biome::DarkForest: grass = 0.03f; flower = 0.002f; break;
                    case biome::Taiga: grass = 0.06f; break;
                    case biome::Savanna: grass = 0.2f; break;
                    case biome::Jungle: grass = 0.25f; break;
                    case biome::Swamp: grass = 0.05f; break;
                    default: break;
                }
                if (p < flower) c.set(lx, h + 1, lz, flowers[rng.range(5)]);
                else if (p < flower + grass) c.set(lx, h + 1, lz, b == biome::Taiga ? bs::Fern : bs::Grass);
                else if (p < flower + grass + 0.0006f) c.set(lx, h + 1, lz, bs::Pumpkin);
            } else if (ground == bs::Sand && b == biome::Desert) {
                if (p < 0.004f) {
                    bool clear = true;
                    for (int d = 0; d < 4 && clear; d++) {
                        int nx = lx + (d == 0) - (d == 1), nz = lz + (d == 2) - (d == 3);
                        if (nx >= 0 && nx < 16 && nz >= 0 && nz < 16 && !stateIsAir(c.get(nx, h + 1, nz))) clear = false;
                    }
                    if (clear) {
                        int ch = 1 + rng.range(3);
                        for (int y = 1; y <= ch; y++) c.set(lx, h + y, lz, bs::Cactus);
                    }
                } else if (p < 0.01f) {
                    c.set(lx, h + 1, lz, bs::DeadBush);
                }
            }
            // sugar cane next to water on beaches / river banks
            if ((ground == bs::Sand || blockIdOf(ground) == blk::GrassBlock) && h == WATER_TOP && p > 0.9f) {
                bool water = false;
                for (int d = 0; d < 4; d++) {
                    int nx = lx + (d == 0) - (d == 1), nz = lz + (d == 2) - (d == 3);
                    if (nx >= 0 && nx < 16 && nz >= 0 && nz < 16 && c.get(nx, h, nz) == bs::Water) water = true;
                }
                if (water && stateIsAir(c.get(lx, h + 1, lz))) {
                    int ch = 1 + rng.range(3);
                    for (int y = 1; y <= ch; y++) c.set(lx, h + y, lz, bs::SugarCane);
                }
            }
        }
    }
    // snow cover
    uint16_t snowyGrass = setBool(bs::GrassBlock, "snowy", true);
    for (int lz = 0; lz < 16; lz++)
        for (int lx = 0; lx < 16; lx++) {
            uint8_t b = cols[lz * 16 + lx].biome;
            bool snowy = isSnowy(b) || (b == biome::Mountains && cols[lz * 16 + lx].height > 125);
            if (!snowy) continue;
            int y = c.height(lx, lz);
            if (y <= 0 || y >= WORLD_HEIGHT) continue;
            uint16_t below = c.get(lx, y - 1, lz);
            if (below == bs::Water || below == bs::Ice || !stateCollides(below)) continue;
            if (!stateIsAir(c.get(lx, y, lz))) continue;
            c.set(lx, y, lz, bs::Snow);
            if (blockIdOf(below) == blk::GrassBlock) c.set(lx, y - 1, lz, snowyGrass);
        }
}

// ------------------------------------------------------------------ entry points
void Generator::generateFlat(Chunk& c) const {
    for (int lz = 0; lz < 16; lz++)
        for (int lx = 0; lx < 16; lx++) {
            c.set(lx, 0, lz, bs::Bedrock);
            c.set(lx, 1, lz, bs::Dirt);
            c.set(lx, 2, lz, bs::Dirt);
            c.set(lx, 3, lz, bs::GrassBlock);
        }
    c.setBiomeAll(biome::Plains);
}

void Generator::generate(Chunk& c) const {
    if (type_ == WORLD_FLAT) {
        generateFlat(c);
    } else if (type_ == WORLD_VOID) {
        c.setBiomeAll(biome::TheVoid);
        if (c.cx == 0 && c.cz == 0) c.set(0, 63, 0, bs::Bedrock);  // a block to stand on
    } else {
        ColumnInfo cols[256];
        fillColumns(c, cols);
        carveCaves(c, cols);
        placeOres(c);
        decorate(c, cols);
    }
    c.recomputeHeightmap();
    c.dropEmptySections();
    for (int i = 0; i < NUM_SECTIONS; i++)
        if (c.section(i)) c.section(i)->optimize();
    c.dirty = false;
    c.lightDirty = true;
}

static const int32_t FINGERPRINT_AT[][2] = {
    {0, 0}, {-1, -1}, {3, -2}, {-7, 5}, {100, -50},          // around spawn
    {62500, -62500}, {-62501, 62499},                         // 1 million blocks out
    {1868750, 1868750}, {-1868751, -1868749},                 // 29.9 million blocks out
};

uint32_t generatorFingerprint(uint64_t seed, uint8_t version) {
    Generator g;
    if (!g.init(seed, WORLD_NORMAL, version)) return 0;
    uint32_t h = 2166136261u;
    auto mix = [&h](uint32_t v) {
        for (int i = 0; i < 4; i++) {
            h ^= (v >> (8 * i)) & 0xFF;
            h *= 16777619u;
        }
    };
    for (const auto& a : FINGERPRINT_AT) {
        Chunk* c = new Chunk(a[0], a[1]);
        if (!c) return 0;
        g.generate(*c);
        for (int y = 0; y < WORLD_HEIGHT; y++)
            for (int z = 0; z < 16; z++)
                for (int x = 0; x < 16; x++) mix(c->get(x, y, z));
        for (int z = 0; z < 16; z++)
            for (int x = 0; x < 16; x++) mix((uint32_t)c->height(x, z) | (uint32_t)c->biome(x, z) << 16);
        delete c;
    }
    return h;
}

uint32_t generatorFloatFingerprint(uint64_t seed, uint8_t version) {
    Generator g;
    if (!g.init(seed, WORLD_NORMAL, version)) return 0;
    uint32_t h = 2166136261u;
    for (const auto& a : FINGERPRINT_AT)
        for (int z = 0; z < 16; z += 3)
            for (int x = 0; x < 16; x += 3) h = g.floatHash(a[0] * 16 + x, a[1] * 16 + z, h);
    return h;
}

// a * b + c in this file's arithmetic (not inlined: the arguments must not be constants)
static __attribute__((noinline)) float mulAdd(float a, float b, float c) { return a * b + c; }

bool generatorArithmeticIsPortable() {
    // (1 + 2^-23)^2 = 1 + 2^-22 + 2^-46. Rounded after the multiply, adding
    // -(1 + 2^-22) gives 0; fused, it gives 2^-46.
    volatile float a = 1.0f + 1.0f / 8388608.0f, c = -(1.0f + 1.0f / 4194304.0f);
    return mulAdd(a, a, c) == 0.0f && noiseMulAdd(a, a, c) == 0.0f;
}

void Generator::findSpawn(int& x, int& y, int& z) const {
    if (type_ != WORLD_NORMAL) {
        x = 0; z = 0;
        y = type_ == WORLD_FLAT ? 4 : 64;
        return;
    }
    // spiral search for dry land
    int px = 0, pz = 0, dx = 0, dz = -1;
    for (int i = 0; i < 4096; i++) {
        int wx = px * 16 + 8, wz = pz * 16 + 8;
        ColumnInfo ci = column(wx, wz);
        if (ci.height > WATER_TOP + 1 && !ci.river && ci.height < 110) {
            x = wx; z = wz; y = ci.height + 1;
            return;
        }
        if (px == pz || (px < 0 && px == -pz) || (px > 0 && px == 1 - pz)) {
            int t = dx; dx = -dz; dz = t;
        }
        px += dx; pz += dz;
    }
    x = 0; z = 0; y = 80;
}

}  // namespace mc

namespace mc {
// Both fingerprints of every version for a few seeds, as computed by the PC build. A
// change of `blocks` means unmodified chunks of existing worlds would change: add a new
// generator version instead.
const GeneratorGolden GENERATOR_GOLDEN[] = {
    {42, 1, 0xa4d86badu, 0xbb1556c7u},
    {1, 1, 0x7882cee9u, 0xc12a3c39u},
    {0xDEADBEEFull, 1, 0xf5a6ff85u, 0xf98ffafcu},
    {42, 2, 0xfe0c7832u, 0xde22e496u},
    {1, 2, 0x4ca836a8u, 0x0fe56a56u},
    {0xDEADBEEFull, 2, 0x9d1a3d4du, 0x0563c45bu},
};
const int NUM_GENERATOR_GOLDEN = (int)(sizeof(GENERATOR_GOLDEN) / sizeof(GENERATOR_GOLDEN[0]));
}  // namespace mc
