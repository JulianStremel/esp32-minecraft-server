#include "mc/world/vanilla/aquifer.h"
#include <math.h>
#include <string.h>
#include "mc/platform.h"
#include "mc/registry.h"
#include "mc/world/vanilla/density.h"
#include "mc/world/vanilla/surface.h"

namespace mc {
namespace vanilla {

const uint16_t MASK_EXTRA_STATES[NUM_MASK_EXTRA] = {bs::Water, bs::Lava, 0, bs::CopperOre, bs::RawCopperBlock,
                                                    bs::Granite, bs::DeepslateIronOre, bs::RawIronBlock, bs::Tuff};
AquiferSteps aquiferSteps;

int maskExtraCode(int extra) { return 2 + SR_NUM_STATES + extra; }

static uint16_t maskMore[96];
static int numMaskMore = 0;

uint16_t maskState(uint8_t code) {
    int i = code - 2;
    if (i < SR_NUM_STATES) return SR_STATES[i];
    i -= SR_NUM_STATES;
    return i < NUM_MASK_EXTRA ? MASK_EXTRA_STATES[i] : maskMore[i - NUM_MASK_EXTRA];
}

int maskCodeOf(uint16_t state) {
    for (int i = 0; i < numMaskMore; i++)
        if (maskMore[i] == state) return 2 + SR_NUM_STATES + NUM_MASK_EXTRA + i;
    return -1;
}

int maskRegisterState(uint16_t state) {
    int c = maskCodeOf(state);
    if (c >= 0 || numMaskMore >= 96) return c;
    maskMore[numMaskMore++] = state;
    return 2 + SR_NUM_STATES + NUM_MASK_EXTRA + numMaskMore - 1;
}

namespace {
constexpr int SEA = 63;
constexpr int LAVA_LEVEL = -54;
constexpr int WAY_BELOW = -32512;   // DimensionType.WAY_BELOW_MIN_Y: no fluid

inline int floorDiv(int a, int b) { int q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }
inline uint64_t mix(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
inline uint64_t posHash(uint64_t seed, int x, int y, int z) {
    return mix(seed ^ mix((uint64_t)(uint32_t)x * 0x9E3779B97F4A7C15ull ^ (uint64_t)(uint32_t)y * 0xC2B2AE3D27D4EB4Full ^
                          (uint64_t)(uint32_t)z * 0x165667B19E3779F9ull));
}
// floats throughout: the ESP32-S3 has no double-precision hardware
inline float clampedMap(float v, float a, float b, float c, float d) {
    float t = (v - a) / (b - a);
    return t < 0 ? c : t > 1 ? d : c + t * (d - c);
}

// Aquifer.FluidStatus
struct Status {
    int level;
    uint16_t fluid;
    uint16_t at(int y) const { return y < level ? fluid : 0; }
};
inline Status globalStatus(int y) { return y < LAVA_LEVEL ? Status{LAVA_LEVEL, bs::Lava} : Status{SEA, bs::Water}; }

// Aquifer.NoiseBasedAquifer#SURFACE_SAMPLING_OFFSETS_IN_CHUNKS
const int8_t SURFACE_OFFSETS[13][2] = {{0, 0}, {-2, -1}, {-1, -1}, {0, -1}, {1, -1}, {-3, 0}, {-2, 0},
                                       {-1, 0}, {1, 0}, {-2, 1}, {-1, 1}, {0, 1}, {1, 1}};

constexpr int GY0 = -7, GYN = 35;   // aquifer grid y: floorDiv(y + 1, 12) - 1 .. + 1 over y -64..319

struct Aquifer {
    Router& r;
    uint64_t seed;
    int gx0, gz0;
    int nBarrier, nFlood, nSpread, nLava;
    int16_t loc[3 * GYN * 3][3];
    Status status[3 * GYN * 3];
    uint8_t known[3 * GYN * 3];   // 1 location, 2 status
    uint32_t statuses = 0;

    int index(int gx, int gy, int gz) const { return ((gx - gx0) * GYN + (gy - GY0)) * 3 + (gz - gz0); }
    const int16_t* location(int i, int gx, int gy, int gz) {
        if (!(known[i] & 1)) {
            uint64_t h = posHash(seed ^ 0xA9F1E2ull, gx, gy, gz);
            loc[i][0] = (int16_t)(gx * 16 + (int)(h % 10));
            loc[i][1] = (int16_t)(gy * 12 + (int)((h >> 16) % 9));
            loc[i][2] = (int16_t)(gz * 16 + (int)((h >> 32) % 10));
            known[i] |= 1;
        }
        return loc[i];
    }
    uint64_t statusUs = 0;
    const Status& statusOf(int i) {
        if (!(known[i] & 2)) {
            uint64_t t = plat::micros();
            status[i] = computeFluid(loc[i][0], loc[i][1], loc[i][2]);
            known[i] |= 2;
            statuses++;
            statusUs += plat::micros() - t;
        }
        return status[i];
    }
    // NoiseBasedAquifer#computeFluid
    Status computeFluid(int x, int y, int z) {
        Status global = globalStatus(y);
        int minPrelim = INT32_MAX, above = y + 12, below = y - 12;
        bool surfaceFluid = false;
        for (const auto& o : SURFACE_OFFSETS) {
            int sx = x + o[0] * 16, sz = z + o[1] * 16;
            // on a 16-block grid (vanilla: a 4-block one): neighbouring chunks share them
            int n = r.preliminarySurface(sx & ~15, sz & ~15);
            if (n > 10000) n = 10000;
            int top = n + 8;
            bool here = o[0] == 0 && o[1] == 0;
            if (here && below > top) return global;
            bool over = above > top;
            if (over || here) {
                Status g = globalStatus(top);
                if (g.at(top) != 0) {
                    if (here) surfaceFluid = true;
                    if (over) return g;
                }
            }
            if (n < minPrelim) minPrelim = n;
        }
        int level = surfaceLevel(x, y, z, global, minPrelim, surfaceFluid);
        return {level, fluidType(x, y, z, global, level)};
    }
    // NoiseBasedAquifer#computeSurfaceLevel
    int surfaceLevel(int x, int y, int z, Status global, int minPrelim, bool surfaceFluid) {
        float full, partial;
        if (r.sample(ROOT_EROSION, x, y, z) < -0.225f && r.sample(ROOT_DEPTH, x, y, z) > 0.9f) {   // the deep dark: dry
            full = partial = -1;
        } else {
            int dist = minPrelim + 8 - y;
            float f = surfaceFluid ? clampedMap((float)dist, 0, 64, 1, 0) : 0;
            float g = r.noiseValue(nFlood, (float)x, (float)y * 0.67f, (float)z);
            g = g < -1 ? -1 : g > 1 ? 1 : g;
            float h = -0.3f + (1 - f) * 1.1f;   // Mth.map(f, 1, 0, -0.3, 0.8)
            float k = -0.8f + (1 - f) * 1.2f;   // Mth.map(f, 1, 0, -0.8, 0.4)
            partial = g - k;
            full = g - h;
        }
        if (full > 0) return global.level;
        if (partial > 0) {   // computeRandomizedFluidSurfaceLevel
            int k = floorDiv(x, 16), l = floorDiv(y, 40), m = floorDiv(z, 16);
            float d = r.noiseValue(nSpread, (float)k, (float)l * 0.71428573f, (float)m) * 10.0f;
            int q = (int)floorf(d / 3.0f) * 3;   // Mth.quantize(d, 3)
            int lvl = l * 40 + 20 + q;
            return minPrelim < lvl ? minPrelim : lvl;
        }
        return WAY_BELOW;
    }
    // NoiseBasedAquifer#computeFluidType
    uint16_t fluidType(int x, int y, int z, Status global, int level) {
        uint16_t f = global.fluid;
        if (level <= -10 && level != WAY_BELOW && global.fluid != bs::Lava) {
            float d = r.noiseValue(nLava, (float)floorDiv(x, 64), (float)floorDiv(y, 40), (float)floorDiv(z, 64));
            if (fabsf(d) > 0.3f) f = bs::Lava;
        }
        return f;
    }
    // NoiseBasedAquifer#calculatePressure
    float pressure(int x, int y, int z, float& barrier, bool& haveBarrier, const Status& a, const Status& b) {
        uint16_t fa = a.at(y), fb = b.at(y);
        if ((fa == bs::Lava && fb == bs::Water) || (fa == bs::Water && fb == bs::Lava)) return 2.0f;
        int diff = abs(a.level - b.level);
        if (diff == 0) return 0.0f;
        float mid = 0.5f * (float)(a.level + b.level);
        float e = (float)y + 0.5f - mid;
        float half = (float)diff / 2.0f;
        float o = half - fabsf(e);
        float q;
        if (e > 0) {
            float p = 0.0f + o;
            q = p > 0 ? p / 1.5f : p / 2.5f;
        } else {
            float p = 3.0f + o;
            q = p > 0 ? p / 3.0f : p / 10.0f;
        }
        float h = 0;
        if (q >= -2.0f && q <= 2.0f) {
            if (!haveBarrier) {
                barrier = r.noiseValue(nBarrier, (float)x, (float)y * 0.5f, (float)z);
                haveBarrier = true;
            }
            h = barrier;
        }
        return 2.0f * (h + q);
    }
    static float similarity(int a, int b) { return 1.0f - (float)abs(b - a) / 25.0f; }

    // NoiseBasedAquifer#computeSubstance for a block the router left empty: the state, or
    // 1 for a barrier (stone)
    // carving: the density is 0 (a carver's block), not the router's
    int substance(int lx, int x, int y, int lz, int z, uint32_t& barriers, bool carving = false) {
        int l = floorDiv(x - 5, 16), m = floorDiv(y + 1, 12), n = floorDiv(z - 5, 16);
        int o = INT32_MAX, p = INT32_MAX, q = INT32_MAX, rr = INT32_MAX;
        int s = 0, t = 0, u = 0, v = 0;
        for (int w = 0; w <= 1; w++)
            for (int dy = -1; dy <= 1; dy++)
                for (int dz = 0; dz <= 1; dz++) {
                    int gx = l + w, gy = m + dy, gz = n + dz;
                    int ac = index(gx, gy, gz);
                    const int16_t* L = location(ac, gx, gy, gz);
                    int af = L[0] - x, ag = L[1] - y, ah = L[2] - z;
                    int ai = af * af + ag * ag + ah * ah;
                    if (o >= ai) { v = u; u = t; t = s; s = ac; rr = q; q = p; p = o; o = ai; }
                    else if (p >= ai) { v = u; u = t; t = ac; rr = q; q = p; p = ai; }
                    else if (q >= ai) { v = u; u = ac; rr = q; q = ai; }
                    else if (rr >= ai) { v = ac; rr = ai; }
                }
        (void)v;
        (void)rr;
        const Status& s1 = statusOf(s);
        float d = similarity(o, p);
        uint16_t fluid = s1.at(y);
        if (d <= 0.0f) return fluid;
        if (fluid == bs::Water && globalStatus(y - 1).at(y - 1) == bs::Lava) return fluid;
        float barrier = 0;
        bool haveBarrier = false;
        const Status& s2 = statusOf(t);
        float density = carving ? 0.0f : r.densityBound(lx, y, lz);
        float e = d * pressure(x, y, z, barrier, haveBarrier, s1, s2);
        if (density + e > 0.0f) { barriers++; return 1; }
        const Status& s3 = statusOf(u);
        float f = similarity(o, q);
        if (f > 0.0f) {
            float g = d * f * pressure(x, y, z, barrier, haveBarrier, s1, s3);
            if (density + g > 0.0f) { barriers++; return 1; }
        }
        float g = similarity(p, q);
        if (g > 0.0f) {
            float h = d * g * pressure(x, y, z, barrier, haveBarrier, s2, s3);
            if (density + h > 0.0f) { barriers++; return 1; }
        }
        return fluid;
    }
};
}  // namespace

struct AquiferHandle {
    Aquifer* a;
};

AquiferHandle* aquiferOpen(Router& r, uint64_t seed, int cx, int cz) {
    // ~5 KB: on the heap, not the stack
    Aquifer* a = new Aquifer{r, seed, 0, 0, noiseIndex("aquifer_barrier"), noiseIndex("aquifer_fluid_level_floodedness"),
                             noiseIndex("aquifer_fluid_level_spread"), noiseIndex("aquifer_lava")};
    if (!a) return nullptr;
    if (a->nBarrier < 0 || a->nFlood < 0 || a->nSpread < 0 || a->nLava < 0) {
        delete a;
        return nullptr;
    }
    a->gx0 = floorDiv(cx * 16 - 5, 16);
    a->gz0 = floorDiv(cz * 16 - 5, 16);
    memset(a->known, 0, sizeof(a->known));
    AquiferHandle* h = new AquiferHandle{a};
    if (!h) delete a;
    return h;
}

void aquiferClose(AquiferHandle* h) {
    if (!h) return;
    delete h->a;
    delete h;
}

int aquiferCarve(AquiferHandle* h, int cx, int cz, int lx, int y, int lz) {
    if (y < LAVA_LEVEL) return bs::Lava;
    uint32_t barriers = 0;
    return h->a->substance(lx, cx * 16 + lx, y, lz, cz * 16 + lz, barriers, true);
}

void fillAquifersAndVeins(AquiferHandle* h, Router& r, uint64_t seed, int cx, int cz, uint8_t* mask, const int* tops) {
    const int minY = DF_SHAPE.minY, cw = DF_SHAPE.cellW, ch = DF_SHAPE.cellH;
    uint64_t t0 = plat::micros();
    AquiferSteps st = {};
    if (!h) return;
    Aquifer* a = h->a;
    int maxTop = minY;
    for (int col = 0; col < 256; col++)
        if (tops[col] > maxTop) maxTop = tops[col];
    // 1) the aquifer, a row at a time: the empty blocks under each column's top
    uint32_t barriers = 0;
    const int water = maskExtraCode(AQ_WATER), lava = maskExtraCode(AQ_LAVA), air = maskExtraCode(AQ_AIR);
    for (int y = LAVA_LEVEL; y < maxTop; y++) {   // below -54 vanilla's aquifer gives lava, as the mask's 0 does
        uint8_t* row = mask + (size_t)(y - minY) * 256;
        for (int col = 0; col < 256; col++) {
            if (row[col] || y >= tops[col]) continue;
            int lx = col & 15, lz = col >> 4;
            st.cave++;
            int sub = a->substance(lx, cx * 16 + lx, y, lz, cz * 16 + lz, barriers);
            uint16_t global = globalStatus(y).at(y);
            if (sub == 1) row[col] = 1;
            else if ((uint16_t)sub != global) row[col] = (uint8_t)(sub == bs::Water ? water : sub == bs::Lava ? lava : air);
        }
    }
    uint64_t t1 = plat::micros();
    st.statuses = a->statuses;
    st.statusUs = (uint32_t)a->statusUs;
    st.barriers = barriers;
    // 2) ore veins (OreVeinifier) in the stone of y -60..50: the toggle lerped over cells
    // from its corners, cells whose corners all stay under the threshold left out
    const int nToggle = noiseIndex("ore_veininess"), nA = noiseIndex("ore_vein_a"), nB = noiseIndex("ore_vein_b"),
              nGap = noiseIndex("ore_gap");
    if (nToggle >= 0 && nA >= 0 && nB >= 0 && nGap >= 0) {
        const int layers = (50 - minY) / ch + 1;   // cells from minY up to y 50
        auto corner = [&](int n, float scale, int ix, int iy, int iz) {
            int x = cx * 16 + ix * cw, y = minY + iy * ch, z = cz * 16 + iz * cw;
            if (y < -60 || y >= 51) return 0.0f;   // range_choice(y, -60, 51, noise, 0)
            return r.noiseValue(n, x * scale, y * scale, z * scale);
        };
        // the toggle at every corner; the ridges' two noises at a corner when a cell first needs
        // them (NAN: not yet), shared by the cells around it
        float* toggle = (float*)malloc(sizeof(float) * 25 * (layers + 1) * 3);
        if (toggle) {
            float* ridgeA = toggle + 25 * (layers + 1);
            float* ridgeB = ridgeA + 25 * (layers + 1);
            for (int k = 0; k < 25 * (layers + 1); k++) ridgeA[k] = ridgeB[k] = NAN;
            uint64_t tt = plat::micros();
            for (int iy = 0; iy <= layers; iy++)
                for (int k = 0; k < 25; k++) toggle[iy * 25 + k] = corner(nToggle, 1.5f, k % 5, iy, k / 5);
            st.toggleUs = (uint32_t)(plat::micros() - tt);
            const int copper = maskExtraCode(VEIN_COPPER_ORE), iron = maskExtraCode(VEIN_IRON_ORE);
            // per y and vein type the edge term (Mth.clampedMap(distance to the range's edge, 0,
            // 20, -0.2, 0)), -1 outside the range: no vein there
            float edge[2][112];   // [copper, iron][y + 60]
            for (int y = -60; y <= 50; y++)
                for (int t = 0; t < 2; t++) {
                    int top = t == 0 ? 50 : -8, bottom = t == 0 ? 0 : -60, dt = top - y, db = y - bottom;
                    edge[t][y + 60] = db < 0 || dt < 0 ? -1.0f : clampedMap((float)(dt < db ? dt : db), 0, 20, -0.2f, 0);
                }
            for (int cy = 0; cy < layers; cy++) {
                float rowEdge[2] = {-1, -1};   // per vein type the best edge term of this layer of cells
                for (int dy = 0; dy < ch; dy++) {
                    int y = minY + cy * ch + dy;
                    if (y < -60 || y > 50) continue;
                    rowEdge[0] = fmaxf(rowEdge[0], edge[0][y + 60]);
                    rowEdge[1] = fmaxf(rowEdge[1], edge[1][y + 60]);
                }
                if (rowEdge[0] < -0.5f && rowEdge[1] < -0.5f) continue;   // no vein's range reaches these cells
                for (int cell = 0; cell < 16; cell++) {
                    int cxi = cell & 3, czi = cell >> 2;
                    float c[8], va[8], vb[8], hi = -1, lo = 1;
                    for (int k = 0; k < 8; k++) {
                        c[k] = toggle[(cy + (k >> 2)) * 25 + (czi + ((k >> 1) & 1)) * 5 + cxi + (k & 1)];
                        hi = fmaxf(hi, c[k]);
                        lo = fminf(lo, c[k]);
                    }
                    // copper where the toggle is above 0, iron below: the toggle inside stays
                    // within its corners
                    if (hi + rowEdge[0] < 0.4f && -lo + rowEdge[1] < 0.4f) continue;
                    st.veinCells++;
                    bool ridges = false;
                    for (int dy = 0; dy < ch; dy++) {
                        int y = minY + cy * ch + dy;
                        if (y < -60 || y > 50) continue;
                        uint8_t* row = mask + (size_t)(y - minY) * 256;
                        float fy = (float)dy / ch;
                        for (int dz = 0; dz < cw; dz++)
                            for (int dx = 0; dx < cw; dx++) {
                                int lx = cxi * cw + dx, lz = czi * cw + dz;
                                if (row[lz * 16 + lx] != 1) continue;
                                float fx = (float)dx / cw, fz = (float)dz / cw;
                                auto lerp3 = [&](const float* v) {
                                    auto l = [](float t, float a0, float b0) { return a0 + t * (b0 - a0); };
                                    return l(fy, l(fz, l(fx, v[0], v[1]), l(fx, v[2], v[3])), l(fz, l(fx, v[4], v[5]), l(fx, v[6], v[7])));
                                };
                                float d = lerp3(c);
                                st.veinTested++;
                                bool isCopper = d > 0;
                                float ed = edge[isCopper ? 0 : 1][y + 60];
                                if (ed < -0.5f) continue;   // outside this vein's range
                                float e = fabsf(d);
                                if (e + ed < 0.4f) continue;
                                int x = cx * 16 + lx, z = cz * 16 + lz;
                                uint64_t h = posHash(seed ^ 0x0E7E1Dull, x, y, z);
                                float r1 = (float)(h & 0xFFFFFF) / 16777216.0f, r2 = (float)((h >> 24) & 0xFFFFFF) / 16777216.0f,
                                      r3 = (float)((h >> 48) & 0xFFFF) / 65536.0f;
                                if (r1 > 0.7f) continue;
                                if (!ridges) {   // the ridges' corners of this cell
                                    for (int k = 0; k < 8; k++) {
                                        int at = (cy + (k >> 2)) * 25 + (czi + ((k >> 1) & 1)) * 5 + cxi + (k & 1);
                                        if (isnan(ridgeA[at])) {
                                            st.ridgeCorners++;
                                            ridgeA[at] = corner(nA, 4.0f, cxi + (k & 1), cy + (k >> 2), czi + ((k >> 1) & 1));
                                            ridgeB[at] = corner(nB, 4.0f, cxi + (k & 1), cy + (k >> 2), czi + ((k >> 1) & 1));
                                        }
                                        va[k] = ridgeA[at];
                                        vb[k] = ridgeB[at];
                                    }
                                    ridges = true;
                                }
                                float ridged = -0.08f + fmaxf(fabsf(lerp3(va)), fabsf(lerp3(vb)));
                                if (ridged >= 0) continue;
                                float g = clampedMap(e, 0.4f, 0.6f, 0.1f, 0.3f);
                                int base = isCopper ? copper : iron;   // ore, raw block, filler follow it
                                st.veinBlocks++;
                                if (r2 < g && r.noiseValue(nGap, (float)x, (float)y, (float)z) > -0.3f)
                                    row[lz * 16 + lx] = (uint8_t)(r3 < 0.02f ? base + 1 : base);
                                else
                                    row[lz * 16 + lx] = (uint8_t)(base + 2);
                            }
                    }
                }
            }
            free(toggle);
        }
    }
    uint64_t t2 = plat::micros();
    st.blocksUs = (uint32_t)(t1 - t0);
    st.veinsUs = (uint32_t)(t2 - t1);
    aquiferSteps = st;
}

}  // namespace vanilla
}  // namespace mc
