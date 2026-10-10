#include "mc/world/vanilla/features.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc/platform.h"
#include "mc/registry.h"
#include "mc/world/chunk.h"
#include "mc/world/vanilla/aquifer.h"
#include "mc/world/vanilla/fastmath.h"
#include "mc/world/vanilla/surface.h"

namespace mc {
namespace vanilla {

FeatureSteps featureSteps;

namespace {
constexpr float PI_F = 3.1415927f;

// the tables' names as states and block ids, resolved once
enum : uint8_t { SV_ANY = 0, SV_PLANT = 1, SV_DOUBLE = 2, SV_SAND_PLANT = 4, SV_CANE = 8, SV_CACTUS = 16, SV_WATER_PLANT = 32,
                 SV_LILY = 64, SV_NETHER = 128 };   // SV_NETHER alone: a Nether plant; with SV_PLANT: fire
struct Resolved {
    bool done = false;
    uint16_t states[512];
    uint8_t survive[512];            // what a state needs under it (SV_*)
    uint16_t* setBlocks = nullptr;   // block ids, FD_SETS' ranges
    // per block set: which mask codes are in it (from 2), and for code 1 stone, deepslate, bedrock
    uint8_t* setCodes = nullptr;     // FD_NUM_SETS * 256
};
Resolved R;

bool nameHas(const char* n, const char* part) { return strstr(n, part) != nullptr; }
bool inSet(int set, uint16_t state);

void resolve() {
    if (!R.done) {
        for (int i = 0; i < FD_NUM_STATES && i < 512; i++) {
            char name[64];
            const char* spec = FD_STATES[i];
            const char* br = strchr(spec, '[');
            size_t n = br ? (size_t)(br - spec) : strlen(spec);
            if (n >= sizeof(name)) n = sizeof(name) - 1;
            memcpy(name, spec, n);
            name[n] = 0;
            int b = findBlock(name);
            uint16_t st = b >= 0 ? BLOCKS[b].defState : 0;
            if (br) {   // k=v,k=v]
                char props[96];
                snprintf(props, sizeof(props), "%s", br + 1);
                char* save = nullptr;
                for (char* kv = strtok_r(props, ",]", &save); kv; kv = strtok_r(nullptr, ",]", &save)) {
                    char* eq = strchr(kv, '=');
                    if (!eq) continue;
                    *eq = 0;
                    if (strcmp(kv, "half") != 0) st = setPropStr(st, kv, eq + 1);   // the lower half is placed first
                }
            }
            R.states[i] = st;
            uint8_t sv = SV_ANY;
            if (nameHas(name, "seagrass") || nameHas(name, "kelp")) sv = SV_WATER_PLANT;
            else if (!strcmp(name, "lily_pad")) sv = SV_LILY;
            else if (!strcmp(name, "sugar_cane")) sv = SV_CANE;
            else if (!strcmp(name, "cactus")) sv = SV_CACTUS;
            else if (!strcmp(name, "dead_bush")) sv = SV_SAND_PLANT;
            else if (!strcmp(name, "fire") || !strcmp(name, "soul_fire")) sv = SV_NETHER | SV_PLANT;
            else if (nameHas(name, "fungus") || nameHas(name, "roots") || !strcmp(name, "nether_sprouts")) sv = SV_NETHER;
            else if (b >= 0 && !(BLOCKS[b].flags & BF_COLLIDES) && !(BLOCKS[b].flags & BF_FLUID) && strcmp(name, "air") &&
                     !nameHas(name, "snow") && !nameHas(name, "vine") && !nameHas(name, "lichen") && !nameHas(name, "mushroom"))
                sv = SV_PLANT;
            if (b >= 0 && propIndexOf((uint16_t)b, "half") >= 0 && strcmp(getPropStr(st, "half"), "lower") == 0) sv |= SV_DOUBLE;
            R.survive[i] = sv;
        }
        int total = 0;
        for (int i = 0; i < FD_NUM_SETS; i++) total += FD_SETS[i].count;
        R.setBlocks = (uint16_t*)malloc(sizeof(uint16_t) * (total ? total : 1));
        for (int i = 0; R.setBlocks && i < FD_NUM_SETS; i++)
            for (int k = 0; k < FD_SETS[i].count; k++) {
                int b = findBlock(FD_SET_NAMES[FD_SETS[i].first + k]);
                R.setBlocks[FD_SETS[i].first + k] = (uint16_t)(b >= 0 ? b : 0xFFFF);
            }
        // the ores' states get codes on the block mask (decorateMask)
        int maxCode = 1;
        for (int i = 0; i < FD_NUM_CFGS; i++)
            if (FD_CFGS[i].type == FC_ORE)
                for (int k = 0; k < FD_CFGS[i].c; k++) {
                    int code = maskRegisterState(R.states[FD_LISTS[FD_CFGS[i].b + 2 * k + 1]]);
                    if (code > maxCode) maxCode = code;
                }
        R.setCodes = (uint8_t*)calloc((size_t)FD_NUM_SETS * 256, 1);
        for (int i = 0; R.setCodes && R.setBlocks && i < FD_NUM_SETS; i++) {
            uint8_t* t = R.setCodes + i * 256;
            t[0] = (inSet(i, bs::Stone) ? 1 : 0) | (inSet(i, bs::Deepslate) ? 2 : 0) | (inSet(i, bs::Bedrock) ? 4 : 0);
            for (int code = 2; code <= maxCode && code < 256; code++) t[code] = inSet(i, maskState((uint8_t)code));
        }
        R.done = true;
    }
}

bool inSet(int set, uint16_t state) {
    uint16_t b = blockIdOf(state);
    const FdSet& s = FD_SETS[set];
    for (int k = 0; k < s.count; k++)
        if (R.setBlocks[s.first + k] == b) return true;
    return false;
}

struct Rand {
    uint32_t s[4];
    explicit Rand(uint64_t seed) { reseed(seed); }
    void reseed(uint64_t seed) {
        for (int i = 0; i < 4; i++) {
            uint64_t z = (seed += 0x9E3779B97F4A7C15ull);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            s[i] = (uint32_t)(z ^ (z >> 31)) | (i == 0 ? 1u : 0u);
        }
    }
    static uint32_t rotl(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }
    uint32_t next32() {
        uint32_t r = rotl(s[1] * 5, 7) * 9, t = s[1] << 9;
        s[2] ^= s[0];
        s[3] ^= s[1];
        s[1] ^= s[2];
        s[0] ^= s[3];
        s[2] ^= t;
        s[3] = rotl(s[3], 11);
        return r;
    }
    int nextInt(int n) { return n <= 0 ? 0 : (int)(((uint64_t)next32() * (uint32_t)n) >> 32); }
    float nextFloat() { return (float)(next32() >> 8) * (1.0f / 16777216.0f); }
    bool nextBool() { return next32() & 0x80000000u; }
    int between(int lo, int hi) { return hi < lo ? lo : lo + nextInt(hi - lo + 1); }
};

// a small 2D gradient noise for vanilla's BIOME_INFO_NOISE (counts by noise)
float infoNoise(uint64_t seed, float x, float z) {
    auto grad = [&](int ix, int iz, float dx, float dz) {
        uint32_t h = ((uint32_t)ix * 374761393u + (uint32_t)iz * 668265263u) ^ (uint32_t)(seed * 0x9E3779B1u);
        h = (h ^ (h >> 13)) * 1274126177u;
        float a = (float)(h & 0xFFFF) * (2 * PI_F / 65536.0f);
        return fastCos(a) * dx + fastSin(a) * dz;
    };
    int ix = (int)floorf(x), iz = (int)floorf(z);
    float fx = x - ix, fz = z - iz;
    float u = fx * fx * (3 - 2 * fx), v = fz * fz * (3 - 2 * fz);
    float a = grad(ix, iz, fx, fz), b = grad(ix + 1, iz, fx - 1, fz), c = grad(ix, iz + 1, fx, fz - 1), d = grad(ix + 1, iz + 1, fx - 1, fz - 1);
    return (a + u * (b - a) + v * ((c + u * (d - c)) - (a + u * (b - a)))) * 1.4f;
}

struct Pos { int x, y, z; };

struct Fx {
    Chunk& c;
    uint64_t seed;
    int x0, z0, minY, maxY;
    int16_t wsurf[256], ofloor[256];   // WORLD_SURFACE_WG, OCEAN_FLOOR_WG (as generated)
    Rand rnd{0};
    TreeFn tree;
    void* treeCtx;
    int top = -1;   // the placed feature being decorated (the biome filter's)
    FeatureSteps st = {};
    uint8_t seen[1024];       // placed features run this step
    Pos bufs[8][2][96];       // the positions of each nesting level (not on the stack)
    float sp[64][4];          // an ore's spheres
    uint8_t visited[4096];    // an ore's box, a bit per block

    bool inside(int x, int y, int z) const { return x >= x0 && x < x0 + 16 && z >= z0 && z < z0 + 16 && y >= minY && y <= maxY; }
    // the ores run on the block mask before the sections are written (vanilla::maskState's
    // codes; 1 is stone, deepslate or bedrock by y); everything else on the chunk
    uint8_t* mask = nullptr;
    const uint8_t* bedrock = nullptr;   // per column, a bit per y from minY
    const uint8_t* deep = nullptr;      // per column, a bit per y from 0
    uint16_t maskGet(int lx, int y, int lz) const {
        int col = lz * 16 + lx, i = y - minY;
        uint8_t code = mask[(size_t)i * 256 + col];
        if (code == 0) return y < -54 ? bs::Lava : y < 63 ? bs::Water : (uint16_t)0;
        if (code == 1) {
            if (i <= 4 && (bedrock[col] >> i & 1)) return bs::Bedrock;
            return (y < 0 || (y < 8 && (deep[col] >> y & 1))) ? bs::Deepslate : bs::Stone;
        }
        return maskState(code);
    }
    uint16_t get(int x, int y, int z) const {
        if (!inside(x, y, z)) return 0;
        return mask ? maskGet(x - x0, y, z - z0) : c.get(x - x0, y, z - z0);
    }
    bool set(int x, int y, int z, uint16_t s) {
        if (!inside(x, y, z)) return false;
        if (mask) {
            int code = maskCodeOf(s);
            if (code < 0) return false;
            mask[(size_t)(y - minY) * 256 + (z - z0) * 16 + (x - x0)] = (uint8_t)code;
            st.blocks++;
            return true;
        }
        c.set(x - x0, y, z - z0, s);
        st.blocks++;
        return true;
    }
    int heightmap(int type, int x, int z) const {
        if (x < x0 || x >= x0 + 16 || z < z0 || z >= z0 + 16) return minY;
        int col = (z - z0) * 16 + (x - x0);
        switch (type) {
            case 0: return wsurf[col];
            case 1: return ofloor[col];
            default: return mask ? wsurf[col] : c.height(x - x0, z - z0);   // MOTION_BLOCKING (with or without leaves)
        }
    }
    uint8_t biomeAt(int x, int y, int z) const { return c.biomeAt(x & 15, y, z & 15); }

    int sample(int pi) {
        const FdProv& p = FD_PROVS[pi];
        switch (p.type) {
            case FP_CONSTANT: return p.a;
            case FP_UNIFORM: return rnd.between(p.a, p.b);
            case FP_BIASED: return p.a + rnd.nextInt(rnd.nextInt(p.b - p.a + 1) + 1);   // BiasedToBottomInt
            case FP_VERY_BIASED: {
                int i = rnd.between(p.a + 1, p.b), j = rnd.between(p.a, i - 1);
                return rnd.between(p.a, j);
            }
            case FP_TRAPEZOID: {
                int span = p.b - p.a;
                if (p.c >= span) return rnd.between(p.a, p.b);
                int side = (span - p.c) / 2, rest = span - side;
                return p.a + rnd.between(0, rest) + rnd.between(0, side);
            }
            case FP_WEIGHTED: {
                int total = 0;
                for (int k = 0; k < p.b; k++) total += FD_LISTS[p.a + 2 * k + 1];
                int r = rnd.nextInt(total);
                for (int k = 0; k < p.b; k++) {
                    r -= FD_LISTS[p.a + 2 * k + 1];
                    if (r < 0) return sample(FD_LISTS[p.a + 2 * k]);
                }
                return 0;
            }
            case FP_CLAMPED: {
                int v = sample(p.a);
                return v < p.b ? p.b : v > p.c ? p.c : v;
            }
            default: return p.a;
        }
    }
    float sampleF(int pi) {
        const FdProv& p = FD_PROVS[pi];
        if (p.type == FP_UNIFORM) return p.fa + rnd.nextFloat() * (p.fb - p.fa);
        return p.fa;
    }
    uint16_t state(int si, int x, int y, int z) {
        const FdStateProv& p = FD_SPROVS[si];
        switch (p.type) {
            case FS_SIMPLE: return (uint16_t)p.a;
            case FS_WEIGHTED: {
                int total = 0;
                for (int k = 0; k < p.b; k++) total += FD_LISTS[p.a + 2 * k + 1];
                int r = rnd.nextInt(total);
                for (int k = 0; k < p.b; k++) {
                    r -= FD_LISTS[p.a + 2 * k + 1];
                    if (r < 0) return (uint16_t)FD_LISTS[p.a + 2 * k];
                }
                return (uint16_t)FD_LISTS[p.a];
            }
            case FS_NOISE_THRESHOLD: {   // NoiseThresholdProvider: the default, or low and high states by noise
                float n = infoNoise(seed ^ 2345, x * p.h, z * p.h);
                int low = p.c >> 8, high = p.c & 255;
                if (n < p.f) return low ? (uint16_t)FD_LISTS[p.b + rnd.nextInt(low)] : (uint16_t)p.a;
                if (rnd.nextFloat() < p.g && high) return (uint16_t)FD_LISTS[p.b + low + rnd.nextInt(high)];
                return low ? (uint16_t)FD_LISTS[p.b + rnd.nextInt(low)] : (uint16_t)p.a;
            }
            case FS_NOISE: {   // NoiseProvider: a state by noise
                float n = infoNoise(seed ^ 0x51, x * p.h, z * p.h);
                int k = (int)((n * 0.5f + 0.5f) * p.b);
                k = k < 0 ? 0 : k >= p.b ? p.b - 1 : k;
                return (uint16_t)FD_LISTS[p.a + k];
            }
            case FS_RULES:
                for (int k = 0; k < p.c; k++)
                    if (pred(FD_LISTS[p.b + 2 * k], x, y, z)) return state(FD_LISTS[p.b + 2 * k + 1], x, y, z);
                return p.a >= 0 ? state(p.a, x, y, z) : (uint16_t)0xFFFF;
            default: return (uint16_t)p.a;
        }
    }
    // BlockState#canSurvive, for the plants features place
    bool survives(int si, int x, int y, int z) const {
        uint8_t sv = R.survive[si];
        uint16_t below = get(x, y - 1, z);
        uint16_t bid = blockIdOf(below);
        auto soil = [&]() {
            return bid == blk::GrassBlock || bid == blk::Dirt || bid == blk::CoarseDirt || bid == blk::Podzol || bid == blk::RootedDirt ||
                   bid == blk::Mud || bid == blk::MossBlock || bid == blk::Mycelium || bid == blk::Farmland || bid == blk::MuddyMangroveRoots;
        };
        if (sv == (SV_NETHER | SV_PLANT)) return stateCollides(below);   // fire: on anything solid
        if (sv & SV_NETHER)
            return bid == blk::CrimsonNylium || bid == blk::WarpedNylium || bid == blk::SoulSoil || bid == blk::Mycelium ||
                   bid == blk::Netherrack || soil();
        if (sv & SV_WATER_PLANT) return blockIdOf(get(x, y, z)) == blk::Water && stateCollides(below);
        if (sv & SV_LILY) return blockIdOf(below) == blk::Water && stateIsAir(get(x, y, z));
        if (sv & SV_CANE) {
            if (!(soil() || bid == blk::Sand || bid == blk::RedSand || bid == blk::SugarCane)) return false;
            if (bid == blk::SugarCane) return true;
            const int d[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (auto& o : d) {
                uint16_t w = blockIdOf(get(x + o[0], y - 1, z + o[1]));
                if (w == blk::Water || w == blk::FrostedIce) return true;
            }
            return false;
        }
        if (sv & SV_CACTUS) {
            const int d[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (auto& o : d)
                if (stateCollides(get(x + o[0], y, z + o[1]))) return false;
            return bid == blk::Sand || bid == blk::RedSand || bid == blk::Cactus;
        }
        if (sv & SV_SAND_PLANT) return bid == blk::Sand || bid == blk::RedSand || soil() || strstr(BLOCKS[bid].name, "terracotta");
        if (sv & SV_PLANT) return soil();
        return true;
    }
    // a predicate on a mask code (a block set without an offset: a table)
    bool codeMatches(int pi, uint8_t code, int x, int y, int z) {
        const FdPred& p = FD_PREDS[pi];
        if (p.type != FB_BLOCKS || p.dx || p.dy || p.dz || !R.setCodes) return pred(pi, x, y, z);
        const uint8_t* t = R.setCodes + p.a * 256;
        if (code == 0) return false;   // water, lava or air: no set of an ore's has them
        if (code >= 2) return t[code];
        uint16_t st = maskGet(x - x0, y, z - z0);   // stone, deepslate or bedrock by y
        return st == bs::Stone ? (t[0] & 1) : st == bs::Deepslate ? (t[0] & 2) : (t[0] & 4);
    }
    // a predicate on the block at (x, y, z) already read (a block set without an offset needs nothing more)
    bool predState(int pi, uint16_t here, int x, int y, int z) {
        const FdPred& p = FD_PREDS[pi];
        if (p.type == FB_BLOCKS && !p.dx && !p.dy && !p.dz) return inSet(p.a, here);
        return pred(pi, x, y, z);
    }
    bool pred(int pi, int x, int y, int z) {
        const FdPred& p = FD_PREDS[pi];
        x += p.dx;
        y += p.dy;
        z += p.dz;
        switch (p.type) {
            case FB_TRUE: return true;
            case FB_BLOCKS: return inSet(p.a, get(x, y, z));
            case FB_FLUIDS: {
                uint16_t b = blockIdOf(get(x, y, z));
                return p.a == 2 ? b == blk::Lava : b == blk::Water;
            }
            case FB_REPLACEABLE: {
                uint16_t s = get(x, y, z);
                return stateIsAir(s) || (!stateCollides(s) && !stateIsFluid(s)) || blockIdOf(s) == blk::Water;
            }
            case FB_SOLID: return stateCollides(get(x, y, z));
            case FB_SURVIVE: return inside(x, y, z) && survives(p.a, x, y, z);
            case FB_NOT: return !pred(p.a, x, y, z);
            case FB_ALL:
                for (int k = 0; k < p.b; k++)
                    if (!pred(FD_LISTS[p.a + k], x, y, z)) return false;
                return true;
            case FB_ANY:
                for (int k = 0; k < p.b; k++)
                    if (pred(FD_LISTS[p.a + k], x, y, z)) return true;
                return false;
            default: return true;
        }
    }
    bool biomeHas(uint8_t b, int placed) const {
        for (int k = 0; k < FD_BIOME_COUNT[b]; k++)
            if (FD_BIOME_PAIRS[2 * (FD_BIOME_FIRST[b] + k) + 1] == placed) return true;
        return false;
    }

    // PlacedFeature#place: the modifiers from position p, then the feature at each result
    bool placed(int pi, Pos p, int depth = 0) {
        const FdPlaced& pf = FD_PLACED[pi];
        if (depth >= 8) return false;
        Pos (*buf)[96] = bufs[depth];
        int n = 1, cur = 0;
        buf[0][0] = p;
        for (int m = 0; m < pf.count; m++) {
            const FdMod& md = FD_MODS[pf.first + m];
            int out = 0, nxt = cur ^ 1;
            for (int i = 0; i < n; i++) {
                Pos q = buf[cur][i];
                auto emit = [&](Pos r) { if (out < 96) buf[nxt][out++] = r; };
                switch (md.type) {
                    case FM_COUNT: {
                        int k = sample(md.a);
                        for (int j = 0; j < k; j++) emit(q);
                        break;
                    }
                    case FM_RARITY_FILTER:
                        if (rnd.nextFloat() < 1.0f / (float)md.a) emit(q);
                        break;
                    case FM_IN_SQUARE: {
                        int dx = rnd.nextInt(16), dz = rnd.nextInt(16);
                        emit({q.x + dx, q.y, q.z + dz});
                        break;
                    }
                    case FM_BIOME:
                        if (depth > 0 || biomeHas(biomeAt(q.x, q.y, q.z), top)) emit(q);
                        break;
                    case FM_HEIGHT_RANGE: emit({q.x, sample(md.a), q.z}); break;
                    case FM_HEIGHTMAP: {
                        int h = heightmap(md.a, q.x, q.z);
                        if (h > minY) emit({q.x, h, q.z});
                        break;
                    }
                    case FM_BLOCK_PREDICATE_FILTER:
                        if (pred(md.a, q.x, q.y, q.z)) emit(q);
                        break;
                    case FM_SURFACE_WATER_DEPTH_FILTER:
                        if (heightmap(0, q.x, q.z) - heightmap(1, q.x, q.z) <= md.a) emit(q);
                        break;
                    case FM_ENVIRONMENT_SCAN: {
                        int dir = md.c > 0 ? 1 : -1, steps = md.c > 0 ? md.c : -md.c;
                        Pos r = q;
                        bool found = false;
                        for (int s = 0; s < steps; s++) {
                            if (pred(md.a, r.x, r.y, r.z)) { found = true; break; }
                            r.y += dir;
                            if (r.y < minY || r.y > maxY) break;
                            if (md.b >= 0 && !pred(md.b, r.x, r.y, r.z)) break;
                        }
                        if (found || pred(md.a, r.x, r.y, r.z)) emit(r);
                        break;
                    }
                    case FM_SURFACE_RELATIVE_THRESHOLD_FILTER: {
                        int h = heightmap(md.a, q.x, q.z);
                        if (q.y >= h + md.b && q.y <= h + md.c) emit(q);
                        break;
                    }
                    case FM_NOISE_BASED_COUNT: {
                        float d = infoNoise(seed ^ 2345, q.x / md.f, q.z / md.f);
                        int k = (int)ceilf((d + md.b / 1000.0f) * (float)md.a);
                        for (int j = 0; j < k; j++) emit(q);
                        break;
                    }
                    case FM_NOISE_THRESHOLD_COUNT: {
                        float d = infoNoise(seed ^ 2345, q.x / 200.0f, q.z / 200.0f);
                        int k = d < md.f ? md.a : md.b;
                        for (int j = 0; j < k; j++) emit(q);
                        break;
                    }
                    case FM_COUNT_ON_EVERY_LAYER: {   // CountOnEveryLayerPlacement: each floor down from the top
                        for (int layer = 0; layer < 32; layer++) {
                            bool any = false;
                            int k = sample(md.a);
                            for (int j = 0; j < k; j++) {
                                int x = q.x + rnd.nextInt(16), z = q.z + rnd.nextInt(16);
                                int found = 0, y = heightmap(2, x, z);
                                for (; y > minY + 1; y--) {
                                    uint16_t above = get(x, y, z), under = get(x, y - 1, z);
                                    auto empty = [](uint16_t s) {
                                        uint16_t b = blockIdOf(s);
                                        return stateIsAir(s) || b == blk::Water || b == blk::Lava;
                                    };
                                    if (empty(above) && !empty(under) && blockIdOf(under) != blk::Bedrock) {
                                        if (found == layer) break;
                                        found++;
                                    }
                                }
                                if (y > minY + 1) {
                                    emit({x, y, z});
                                    any = true;
                                }
                            }
                            if (!any) break;
                        }
                        break;
                    }
                    case FM_DROP: break;   // a modifier not done: no position
                    case FM_RANDOM_OFFSET: {
                        int dx = sample(md.a), dy = sample(md.b), dz = sample(md.a);
                        emit({q.x + dx, q.y + dy, q.z + dz});
                        break;
                    }
                    default: emit(q);
                }
            }
            n = out;
            cur = nxt;
        }
        bool any = false;
        for (int i = 0; i < n; i++) any |= feature(pf.cfg, buf[cur][i], depth);
        return any;
    }

    // ConfiguredFeature#place
    bool feature(int ci, Pos p, int depth) {
        const FdCfg& cf = FD_CFGS[ci];
        if (depth > 6) return false;
        switch (cf.type) {
            case FC_SKIP: st.skipped++; return false;
            case FC_RANDOM_PATCH: {
                int placedN = 0;
                for (int i = 0; i < cf.a; i++) {
                    int dx = rnd.nextInt(cf.b + 1) - rnd.nextInt(cf.b + 1);
                    int dy = rnd.nextInt(cf.c + 1) - rnd.nextInt(cf.c + 1);
                    int dz = rnd.nextInt(cf.b + 1) - rnd.nextInt(cf.b + 1);
                    if (placed(cf.d, {p.x + dx, p.y + dy, p.z + dz}, depth + 1)) placedN++;
                }
                return placedN > 0;
            }
            case FC_SIMPLE_BLOCK: {
                if (!inside(p.x, p.y, p.z)) return false;
                uint16_t si = state(cf.a, p.x, p.y, p.z);
                if (si == 0xFFFF || !survives(si, p.x, p.y, p.z)) return false;
                if (R.survive[si] & SV_DOUBLE) {
                    if (!inside(p.x, p.y + 1, p.z) || !stateIsAir(get(p.x, p.y + 1, p.z))) return false;
                    set(p.x, p.y, p.z, R.states[si]);
                    set(p.x, p.y + 1, p.z, setPropStr(R.states[si], "half", "upper"));
                    return true;
                }
                return set(p.x, p.y, p.z, R.states[si]);
            }
            case FC_ORE: return ore(cf, p);
            case FC_TREE:
                if (!tree) return false;
                return tree(treeCtx, c, p.x - x0, p.y, p.z - z0, cf.a, cf.b, cf.c);
            case FC_RANDOM_SELECTOR:
                for (int k = 0; k < cf.b; k++)
                    if (rnd.nextFloat() < (float)FD_LISTS[cf.a + 2 * k + 1] / 1e6f) return placed(FD_LISTS[cf.a + 2 * k], p, depth + 1);
                return placed(cf.c, p, depth + 1);
            case FC_SIMPLE_RANDOM_SELECTOR: return placed(FD_LISTS[cf.a + rnd.nextInt(cf.b)], p, depth + 1);
            case FC_RANDOM_BOOLEAN_SELECTOR: return placed(rnd.nextBool() ? cf.a : cf.b, p, depth + 1);
            case FC_SEAGRASS: {
                int dx = rnd.nextInt(8) - rnd.nextInt(8), dz = rnd.nextInt(8) - rnd.nextInt(8);
                int x = p.x + dx, z = p.z + dz, y = heightmap(1, x, z);
                if (!inside(x, y, z) || blockIdOf(get(x, y, z)) != blk::Water || !stateCollides(get(x, y - 1, z))) return false;
                bool tall = rnd.nextFloat() < cf.f && blockIdOf(get(x, y + 1, z)) == blk::Water && inside(x, y + 1, z);
                if (tall) {
                    set(x, y, z, bs::TallSeagrass);
                    set(x, y + 1, z, setPropStr(bs::TallSeagrass, "half", "upper"));
                } else {
                    set(x, y, z, bs::Seagrass);
                }
                return true;
            }
            case FC_KELP: {
                int y = heightmap(1, p.x, p.z);
                if (!inside(p.x, y, p.z) || blockIdOf(get(p.x, y, p.z)) != blk::Water) return false;
                int h = 1 + rnd.nextInt(10), placedN = 0;
                for (int i = 0; i <= h; i++) {
                    int yy = y + i;
                    if (blockIdOf(get(p.x, yy, p.z)) != blk::Water || blockIdOf(get(p.x, yy + 1, p.z)) != blk::Water) {
                        if (i > 0 && blockIdOf(get(p.x, yy, p.z)) == blk::Water)
                            set(p.x, yy, p.z, setProp(bs::Kelp, "age", 20 + rnd.nextInt(4)));
                        break;
                    }
                    set(p.x, yy, p.z, i == h ? setProp(bs::Kelp, "age", 20 + rnd.nextInt(4)) : bs::KelpPlant);
                    placedN++;
                }
                return placedN > 0;
            }
            case FC_FREEZE_TOP_LAYER: return freeze();
            case FC_GLOWSTONE_BLOB: {   // GlowstoneFeature: hangs from netherrack, grows where it touches one glowstone
                if (!inside(p.x, p.y, p.z) || !stateIsAir(get(p.x, p.y, p.z))) return false;
                uint16_t above = blockIdOf(get(p.x, p.y + 1, p.z));
                if (above != blk::Netherrack && above != blk::Basalt && above != blk::Blackstone) return false;
                set(p.x, p.y, p.z, bs::Glowstone);
                for (int i = 0; i < 1500; i++) {
                    int x = p.x + rnd.nextInt(8) - rnd.nextInt(8), y = p.y - rnd.nextInt(12), z = p.z + rnd.nextInt(8) - rnd.nextInt(8);
                    if (!inside(x, y, z) || !stateIsAir(get(x, y, z))) continue;
                    int n = 0;
                    const int d[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
                    for (auto& o : d) n += blockIdOf(get(x + o[0], y + o[1], z + o[2])) == blk::Glowstone;
                    if (n == 1) set(x, y, z, bs::Glowstone);
                }
                return true;
            }
            case FC_HUGE_FUNGUS: {   // HugeFungusFeature (simpler): a stem on its nylium, a hat, shroomlights in it
                if (!inside(p.x, p.y, p.z) || blockIdOf(get(p.x, p.y - 1, p.z)) != blockIdOf(R.states[cf.d])) return false;
                int h = rnd.between(4, 13);
                if (rnd.nextInt(12) == 0) h *= 2;
                if (p.y + h + 1 > maxY) return false;
                int r = h > 8 ? 3 : 2;
                if (p.x - r < x0 || p.x + r > x0 + 15 || p.z - r < z0 || p.z + r > z0 + 15) return false;   // the chunk's
                for (int i = 0; i < h; i++)
                    if (!stateIsAir(get(p.x, p.y + i, p.z)) && i > 0) { h = i; break; }
                if (h < 4) return false;
                for (int i = 0; i < h; i++) set(p.x, p.y + i, p.z, R.states[cf.c]);
                int hatTop = p.y + h, hatBottom = hatTop - (h > 8 ? 4 : 3);
                for (int y = hatBottom; y <= hatTop; y++) {
                    int rr = y == hatTop ? r - 1 : r;
                    for (int dx = -rr; dx <= rr; dx++)
                        for (int dz = -rr; dz <= rr; dz++) {
                            bool corner = (dx == -rr || dx == rr) && (dz == -rr || dz == rr);
                            if ((dx || dz) == false && y < hatTop) continue;   // the stem
                            if (corner && rnd.nextInt(3)) continue;
                            if (!stateIsAir(get(p.x + dx, y, p.z + dz))) continue;
                            bool inner = abs(dx) < rr && abs(dz) < rr && y < hatTop;
                            set(p.x + dx, y, p.z + dz, inner && rnd.nextInt(8) == 0 ? R.states[cf.b] : R.states[cf.a]);
                        }
                }
                return true;
            }
            case FC_NETHER_VEGETATION: {   // NetherForestVegetationFeature
                int n = 0;
                for (int i = 0; i < cf.b * cf.b; i++) {
                    int x = p.x + rnd.nextInt(cf.b) - rnd.nextInt(cf.b), y = p.y + rnd.nextInt(cf.c) - rnd.nextInt(cf.c),
                        z = p.z + rnd.nextInt(cf.b) - rnd.nextInt(cf.b);
                    if (!inside(x, y, z) || !stateIsAir(get(x, y, z))) continue;
                    uint16_t si = state(cf.a, x, y, z);
                    if (si == 0xFFFF || !survives(si, x, y, z)) continue;
                    set(x, y, z, R.states[si]);
                    n++;
                }
                return n > 0;
            }
            case FC_WEEPING_VINES: {   // hanging from netherrack or wart blocks
                if (!inside(p.x, p.y, p.z) || !stateIsAir(get(p.x, p.y, p.z))) return false;
                uint16_t above = blockIdOf(get(p.x, p.y + 1, p.z));
                if (above != blk::Netherrack && above != blk::NetherWartBlock) return false;
                int len = 1 + rnd.nextInt(8), i = 0;
                for (; i < len && stateIsAir(get(p.x, p.y - i - 1, p.z)) && inside(p.x, p.y - i - 1, p.z); i++)
                    set(p.x, p.y - i, p.z, bs::WeepingVinesPlant);
                set(p.x, p.y - i, p.z, bs::WeepingVines);
                return true;
            }
            case FC_TWISTING_VINES: {   // standing on warped nylium or wart blocks, spread around
                int n = 0;
                for (int t = 0; t < cf.b * cf.b; t++) {
                    int x = p.x + rnd.nextInt(cf.b) - rnd.nextInt(cf.b), y = p.y + rnd.nextInt(cf.c) - rnd.nextInt(cf.c),
                        z = p.z + rnd.nextInt(cf.b) - rnd.nextInt(cf.b);
                    if (!inside(x, y, z) || !stateIsAir(get(x, y, z)) || !stateCollides(get(x, y - 1, z))) continue;
                    int len = 1 + rnd.nextInt(cf.a), i = 0;
                    for (; i < len - 1 && inside(x, y + i + 1, z) && stateIsAir(get(x, y + i + 1, z)); i++)
                        set(x, y + i, z, bs::TwistingVinesPlant);
                    set(x, y + i, z, bs::TwistingVines);
                    n++;
                }
                return n > 0;
            }
            case FC_CHORUS_PLANT: {   // ChorusPlantFeature (simpler): a stem with a branch or two and flowers
                if (!inside(p.x, p.y, p.z) || !stateIsAir(get(p.x, p.y, p.z)) || blockIdOf(get(p.x, p.y - 1, p.z)) != blk::EndStone)
                    return false;
                if (p.x < x0 + 2 || p.x > x0 + 13 || p.z < z0 + 2 || p.z > z0 + 13) return false;
                auto plant = [&](bool up, bool down, bool n, bool s, bool e, bool w) {
                    uint16_t st = bs::ChorusPlant;
                    st = setPropStr(st, "up", up ? "true" : "false");
                    st = setPropStr(st, "down", down ? "true" : "false");
                    st = setPropStr(st, "north", n ? "true" : "false");
                    st = setPropStr(st, "south", s ? "true" : "false");
                    st = setPropStr(st, "east", e ? "true" : "false");
                    return setPropStr(st, "west", w ? "true" : "false");
                };
                int h = 2 + rnd.nextInt(4), side = rnd.nextInt(4), at = 1 + rnd.nextInt(h - 1);
                const int dx[4] = {0, 0, 1, -1}, dz[4] = {-1, 1, 0, 0};
                for (int i = 0; i < h; i++)
                    set(p.x, p.y + i, p.z, plant(true, true, i == at && side == 0, i == at && side == 1, i == at && side == 2, i == at && side == 3));
                set(p.x, p.y + h, p.z, bs::ChorusFlower);
                // the branch: out one block, up one or two, a flower
                int bx = p.x + dx[side], bz = p.z + dz[side], bh = 1 + rnd.nextInt(2);
                const bool back[4][4] = {{0, 1, 0, 0}, {1, 0, 0, 0}, {0, 0, 0, 1}, {0, 0, 1, 0}};
                set(bx, p.y + at, bz, plant(true, false, back[side][0], back[side][1], back[side][2], back[side][3]));
                for (int i = 1; i < bh; i++) set(bx, p.y + at + i, bz, plant(true, true, false, false, false, false));
                set(bx, p.y + at + bh, bz, bs::ChorusFlower);
                return true;
            }
            case FC_DISK: {
                int r = sample(cf.c), placedN = 0;
                for (int dx = -r; dx <= r; dx++)
                    for (int dz = -r; dz <= r; dz++) {
                        if (dx * dx + dz * dz > r * r) continue;
                        int x = p.x + dx, z = p.z + dz;
                        for (int y = p.y + cf.d; y >= p.y - cf.d; y--) {
                            if (!inside(x, y, z) || !pred(cf.b, x, y, z)) continue;
                            uint16_t si = state(cf.a, x, y, z);
                            if (si != 0xFFFF) { set(x, y, z, R.states[si]); placedN++; }
                        }
                    }
                return placedN > 0;
            }
            case FC_SPRING_FEATURE: {
                auto valid = [&](int x, int y, int z) { return inSet(cf.e, get(x, y, z)); };
                if (!inside(p.x, p.y, p.z) || !valid(p.x, p.y + 1, p.z)) return false;
                if (cf.b && !valid(p.x, p.y - 1, p.z)) return false;
                uint16_t here = get(p.x, p.y, p.z);
                if (!stateIsAir(here) && !inSet(cf.e, here)) return false;
                int rock = 0, hole = 0;
                const int d[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (auto& o : d) {
                    rock += valid(p.x + o[0], p.y, p.z + o[1]);
                    hole += stateIsAir(get(p.x + o[0], p.y, p.z + o[1])) && inside(p.x + o[0], p.y, p.z + o[1]);
                }
                if (cf.b) rock += valid(p.x, p.y - 1, p.z);
                if (rock != cf.c || hole != cf.d) return false;
                uint16_t fluid = blockIdOf(R.states[cf.a]) == blk::Lava ? bs::Lava : bs::Water;   // a source
                return set(p.x, p.y, p.z, fluid);
            }
            case FC_BLOCK_COLUMN: {
                int total = 0, heights[4] = {};
                for (int k = 0; k < cf.b && k < 4; k++) total += heights[k] = sample(FD_LISTS[cf.a + 2 * k]);
                int room = 0;
                for (; room < total; room++)
                    if (!pred(cf.c, p.x, p.y + room * cf.d, p.z)) break;
                if (room == 0) return false;
                int y = p.y, left = room;
                for (int k = 0; k < cf.b && k < 4 && left > 0; k++)
                    for (int i = 0; i < heights[k] && left > 0; i++, left--, y += cf.d) {
                        uint16_t si = state(FD_LISTS[cf.a + 2 * k + 1], p.x, y, p.z);
                        if (si == 0xFFFF) continue;
                        if (y == p.y && !survives(si, p.x, y, p.z)) return false;
                        set(p.x, y, p.z, R.states[si]);
                    }
                return true;
            }
            default: return false;
        }
    }

    // OreFeature#place, #doPlace (floats)
    bool ore(const FdCfg& cf, Pos p) {
        st.ores++;
        uint64_t q0 = plat::micros();
        int size = cf.a;
        float f = rnd.nextFloat() * PI_F, g = (float)size / 8.0f;
        int i = (int)ceilf(((float)size / 16.0f * 2.0f + 1.0f) / 2.0f);
        float sf = fastSin(f), cf_ = fastCos(f);
        float ax = p.x + sf * g, bx = p.x - sf * g, az = p.z + cf_ * g, bz = p.z - cf_ * g;
        float ay = (float)(p.y + rnd.nextInt(3) - 2), by = (float)(p.y + rnd.nextInt(3) - 2);
        // vanilla's box around the blob; nothing if all of it is above the ocean floor
        int ceilG = (int)ceilf(g);
        int bx0 = p.x - ceilG - i, by0 = p.y - 2 - i, bz0 = p.z - ceilG - i, bw = 2 * (ceilG + i) + 1, bh = 2 * (2 + i) + 1;
        bool under = false;   // (the box's columns in this chunk)
        int xa = bx0 > x0 ? bx0 : x0, xb = bx0 + bw - 1 < x0 + 15 ? bx0 + bw - 1 : x0 + 15;
        int za = bz0 > z0 ? bz0 : z0, zb = bz0 + bw - 1 < z0 + 15 ? bz0 + bw - 1 : z0 + 15;
        for (int z = za; z <= zb && !under; z++)
            for (int x = xa; x <= xb && !under; x++)
                if (by0 <= ofloor[(z - z0) * 16 + (x - x0)]) under = true;
        uint64_t q1 = plat::micros();
        st.oreUs[0] += (uint32_t)(q1 - q0);
        if (!under) return false;
        st.oresUnder++;
        if (size > 64) size = 64;
        for (int j = 0; j < size; j++) {
            float t = (float)j / (float)size;
            float x = ax + (bx - ax) * t, y = ay + (by - ay) * t, z = az + (bz - az) * t;
            float h = rnd.nextFloat() * (float)size / 16.0f;
            float r = ((fastSin(PI_F * t) + 1.0f) * h + 1.0f) / 2.0f;
            sp[j][0] = x; sp[j][1] = y; sp[j][2] = z; sp[j][3] = r;
        }
        // (vanilla drops the spheres inside others first; with each block looked at once below
        // the blob is the same)
        uint64_t q2 = plat::micros();
        st.oreUs[1] += (uint32_t)(q2 - q1);
        // each block of the box is looked at once (vanilla's BitSet)
        size_t cells = (size_t)bw * bh * bw;
        if (cells > sizeof(visited) * 8) return false;
        memset(visited, 0, (cells + 7) / 8);
        bool any = false;
        for (int j = 0; j < size; j++) {
            float r = sp[j][3];
            if (r < 0) continue;
            int x0b = (int)floorf(sp[j][0] - r), x1b = (int)floorf(sp[j][0] + r);
            int y0b = (int)floorf(sp[j][1] - r), y1b = (int)floorf(sp[j][1] + r);
            int z0b = (int)floorf(sp[j][2] - r), z1b = (int)floorf(sp[j][2] + r);
            if (x1b < x0 || x0b >= x0 + 16 || z1b < z0 || z0b >= z0 + 16) continue;   // outside the chunk
            if (x0b < x0) x0b = x0;
            if (x1b > x0 + 15) x1b = x0 + 15;
            if (y0b < minY) y0b = minY;
            if (y1b > maxY) y1b = maxY;
            float ir = 1.0f / r;
            st.spheres++;
            for (int x = x0b; x <= x1b; x++) {
                float ex = ((float)x + 0.5f - sp[j][0]) * ir;
                if (ex * ex >= 1) continue;
                for (int y = y0b; y <= y1b; y++) {
                    float ey = ((float)y + 0.5f - sp[j][1]) * ir;
                    float exy = ex * ex + ey * ey;
                    if (exy >= 1) continue;
                    // the row's blocks in the chunk (a square root per row costs more than the test)
                    int za = z0b < z0 ? z0 : z0b, zb = z1b > z0 + 15 ? z0 + 15 : z1b;
                    for (int z = za; z <= zb; z++) {
                        float ez = ((float)z + 0.5f - sp[j][2]) * ir;
                        if (exy + ez * ez >= 1) continue;
                        st.voxels++;
                        int vx = x - bx0, vy = y - by0, vz = z - bz0;
                        if (vx < 0 || vy < 0 || vz < 0 || vx >= bw || vy >= bh || vz >= bw) continue;
                        size_t bit = ((size_t)vy * bw + vz) * bw + vx;
                        if (visited[bit >> 3] & (1 << (bit & 7))) continue;
                        visited[bit >> 3] |= (uint8_t)(1 << (bit & 7));
                        uint16_t here = 0;
                        uint8_t code = 0;
                        if (mask) code = mask[(size_t)(y - minY) * 256 + (z - z0) * 16 + (x - x0)];
                        else here = get(x, y, z);
                        for (int k = 0; k < cf.c; k++)
                            if (mask ? codeMatches(FD_LISTS[cf.b + 2 * k], code, x, y, z) : predState(FD_LISTS[cf.b + 2 * k], here, x, y, z)) {
                                if (mask) here = maskGet(x - x0, y, z - z0);
                                if (cf.f > 0 && rnd.nextFloat() < cf.f && exposed(x, y, z)) break;
                                if (blockIdOf(here) != blockIdOf(R.states[FD_LISTS[cf.b + 2 * k + 1]])) {
                                    set(x, y, z, R.states[FD_LISTS[cf.b + 2 * k + 1]]);
                                    any = true;
                                }
                                break;
                            }
                    }
                }
            }
        }
        st.oreUs[2] += (uint32_t)(plat::micros() - q2);
        return any;
    }
    bool exposed(int x, int y, int z) const {
        const int d[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        for (auto& o : d)
            if (stateIsAir(get(x + o[0], y + o[1], z + o[2]))) return true;
        return false;
    }
    // FreezeTopLayerFeature: ice on still water and snow on the ground where it is cold
    bool freeze() {
        for (int lz = 0; lz < 16; lz++)
            for (int lx = 0; lx < 16; lx++) {
                int y = c.height(lx, lz);   // the first free block
                int x = x0 + lx, z = z0 + lz;
                uint8_t b = c.biomeAt(lx, y, lz);
                float t = BIOME_TEMPERATURE[b];
                if (y > 80) t -= (float)(y - 80) * 0.05f / 40.0f;
                if (t >= 0.15f) continue;
                uint16_t below = get(x, y - 1, z);
                if (blockIdOf(below) == blk::Water && getProp(below, "level") == 0) set(x, y - 1, z, bs::Ice);
                else if (stateIsAir(get(x, y, z)) && stateCollides(below) && blockIdOf(below) != blk::Ice &&
                         blockIdOf(below) != blk::PackedIce)
                    set(x, y, z, bs::Snow);
            }
        return true;
    }
};
}  // namespace

void featuresInit() { resolve(); }

namespace {
// the chunk's biomes' features step by step: on the mask only the ores, on the chunk the rest
enum { REST = 0, ORES = 1, ALL = 2 };
void runFeatures(Fx* fx, Chunk& c, uint64_t seed, int ores) {

    // the biomes in the chunk (columns and cave cells) and their features, step by step
    bool present[256] = {};
    for (int q = 0; q < 16; q++) present[c.biome((q & 3) * 4, (q >> 2) * 4)] = true;
    if (const uint8_t* b3 = c.biomes3d())
        for (int i = 0; i < c.numSections() * 64; i++) present[b3[i]] = true;
    uint8_t* seen = fx->seen;
    for (int step = 0; step < 11; step++) {
        memset(seen, 0, sizeof(fx->seen));
        for (int b = 0; b < 256; b++) {
            if (!present[b] || b >= NUM_BIOMES) continue;
            for (int k = 0; k < FD_BIOME_COUNT[b]; k++) {
                int s = FD_BIOME_PAIRS[2 * (FD_BIOME_FIRST[b] + k)], pf = FD_BIOME_PAIRS[2 * (FD_BIOME_FIRST[b] + k) + 1];
                if (s != step || pf >= 1024 || seen[pf]) continue;
                if (ores != ALL && (FD_CFGS[FD_PLACED[pf].cfg].type == FC_ORE) != (ores == ORES)) continue;
                seen[pf] = 1;
                fx->rnd.reseed(seed ^ ((uint64_t)(uint32_t)c.cx * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)(uint32_t)c.cz * 0xC2B2AE3D27D4EB4Full) ^
                               ((uint64_t)(step * 1024 + pf) * 0x165667B19E3779F9ull));
                fx->top = pf;
                fx->st.placed++;
                uint64_t tf = plat::micros();
                fx->placed(pf, {c.cx * 16, c.minY(), c.cz * 16});
                fx->st.typeUs[FD_CFGS[FD_PLACED[pf].cfg].type & 31] += (uint32_t)(plat::micros() - tf);
            }
        }
    }
}
}  // namespace

void decorateMask(Chunk& c, uint8_t* mask, uint64_t seed, const int* tops, const uint8_t* bedrock, const uint8_t* deep) {
    uint64_t t0 = plat::micros();
    if (!R.done) return;
    Fx* fx = new Fx{c, seed, c.cx * 16, c.cz * 16, c.minY(), c.maxY()};
    if (!fx || !R.setBlocks) {
        delete fx;
        return;
    }
    fx->mask = mask;
    fx->bedrock = bedrock;
    fx->deep = deep;
    for (int col = 0; col < 256; col++) {   // the mask's heightmaps: the top block, the sea over it
        fx->ofloor[col] = (int16_t)(tops[col] + 1);
        fx->wsurf[col] = (int16_t)((tops[col] < 62 ? 62 : tops[col]) + 1);
    }
    runFeatures(fx, c, seed, ORES);
    fx->st.us = (uint32_t)(plat::micros() - t0);
    featureSteps = fx->st;
    delete fx;
}

void decorateChunk(Chunk& c, uint64_t seed, TreeFn tree, void* treeCtx, bool withOres) {
    uint64_t t0 = plat::micros();
    if (!R.done) return;   // featuresInit not called
    Fx* fx = new Fx{c, seed, c.cx * 16, c.cz * 16, c.minY(), c.maxY()};
    if (!fx || !R.setBlocks) {
        delete fx;
        return;
    }
    fx->tree = tree;
    fx->treeCtx = treeCtx;
    for (int lz = 0; lz < 16; lz++)
        for (int lx = 0; lx < 16; lx++) {   // the heightmaps as generated: from the chunk's (solid or fluid)
            int ws = c.height(lx, lz), of = ws;
            while (of > c.minY() && !stateCollides(c.get(lx, of - 1, lz))) of--;   // down through the water
            fx->wsurf[lz * 16 + lx] = (int16_t)ws;
            fx->ofloor[lz * 16 + lx] = (int16_t)of;
        }
    fx->st.heightmapUs = (uint32_t)(plat::micros() - t0);
    FeatureSteps ores = withOres ? FeatureSteps{} : featureSteps;   // the mask's part, this chunk's
    runFeatures(fx, c, seed, withOres ? ALL : REST);
    fx->st.us = (uint32_t)(plat::micros() - t0) + ores.us;
    fx->st.placed += ores.placed;
    fx->st.blocks += ores.blocks;
    fx->st.ores = ores.ores;
    fx->st.oresUnder = ores.oresUnder;
    fx->st.spheres = ores.spheres;
    fx->st.voxels = ores.voxels;
    for (int k = 0; k < 4; k++) fx->st.oreUs[k] = ores.oreUs[k];
    fx->st.typeUs[FC_ORE] = ores.typeUs[FC_ORE];
    featureSteps = fx->st;
    delete fx;
}

}  // namespace vanilla
}  // namespace mc
