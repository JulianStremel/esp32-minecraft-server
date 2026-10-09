#include "mc/world/vanilla/density.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "mc/platform.h"

namespace mc {
namespace vanilla {

namespace {

// the dot product with gradient h & 15 (SimplexNoise.GRADIENT), without multiplications
inline float gradDot(int h, float x, float y, float z) {
    switch (h & 15) {
        case 0: case 12: return x + y;
        case 1: case 14: return -x + y;
        case 2: return x - y;
        case 3: return -x - y;
        case 4: return x + z;
        case 5: return -x + z;
        case 6: return x - z;
        case 7: return -x - z;
        case 8: return y + z;
        case 9: case 13: return -y + z;
        case 10: return y - z;
        default: return -y - z;   // 11, 15
    }
}
inline int fastFloor(float v) {
    int i = (int)v;
    return v < (float)i ? i - 1 : i;
}
inline float smooth(float t) { return t * t * t * (t * (t * 6 - 15) + 10); }
inline float lerp(float t, float a, float b) { return a + t * (b - a); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
inline float clampedLerp(float a, float b, float t) { return t < 0 ? a : t > 1 ? b : lerp(t, a, b); }
inline float clampedMap(float v, float a, float b, float c, float d) { return clampedLerp(c, d, (v - a) / (b - a)); }

uint64_t splitmix(uint64_t& s) {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
float unit(uint64_t& s) { return (float)((splitmix(s) >> 40) * (1.0 / (1ull << 24))); }

// expected deviation of a NormalNoise over n octaves (NormalNoise#expectedDeviation)
float expectedDeviation(int n) { return 0.1f * (1.0f + 1.0f / (float)(n + 1)); }

}  // namespace

void ImprovedNoise::init(uint64_t& rng, const uint8_t* shared) {
    xo_ = unit(rng) * 256;
    yo_ = unit(rng) * 256;
    zo_ = unit(rng) * 256;
    p_ = shared ? shared : own_;
    if (shared) return;
    for (int i = 0; i < 256; i++) own_[i] = (uint8_t)i;
    for (int i = 0; i < 256; i++) {
        int j = i + (int)(splitmix(rng) % (uint64_t)(256 - i));
        uint8_t t = own_[i];
        own_[i] = own_[j];
        own_[j] = t;
    }
}

float ImprovedNoise::noise(float x, float y, float z, float yScale, float yMax) const {
    float d = x + xo_, e = y + yo_, f = z + zo_;
    int i = fastFloor(d), j = fastFloor(e), k = fastFloor(f);
    float g = d - (float)i, h = e - (float)j, l = f - (float)k;
    float snap = 0;
    if (yScale != 0) {
        float m = yMax >= 0 && yMax < h ? yMax : h;
        snap = (float)fastFloor(m / yScale + 1.0e-7f) * yScale;
    }
    float hy = h - snap;
    auto P = [&](int v) { return p_[v & 255]; };
    int a = P(i), b = P(i + 1), aa = P(a + j), ab = P(a + j + 1), ba = P(b + j), bb = P(b + j + 1);
    float n000 = gradDot(P(aa + k), g, hy, l), n100 = gradDot(P(ba + k), g - 1, hy, l);
    float n010 = gradDot(P(ab + k), g, hy - 1, l), n110 = gradDot(P(bb + k), g - 1, hy - 1, l);
    float n001 = gradDot(P(aa + k + 1), g, hy, l - 1), n101 = gradDot(P(ba + k + 1), g - 1, hy, l - 1);
    float n011 = gradDot(P(ab + k + 1), g, hy - 1, l - 1), n111 = gradDot(P(bb + k + 1), g - 1, hy - 1, l - 1);
    float u = smooth(g), v = smooth(h), w = smooth(l);
    return lerp(w, lerp(v, lerp(u, n000, n100), lerp(u, n010, n110)), lerp(v, lerp(u, n001, n101), lerp(u, n011, n111)));
}

Router::~Router() {
    if (perlin_)
        for (int i = 0; i < 2 * NUM_DF_NOISES; i++) plat::bigFree(perlin_[i].levels);
    plat::bigFree(perlin_);
    plat::bigFree(normalFactor_);
    plat::bigFree(blendedNoise_);
    free(memo_);
    free(memoStamp_);
    free(cacheKey_);
    free(interpSlot_);
    free(interpNow_);
    free(cellCorners_);
    free(cellFill_);
    free(perm_);
    plat::bigFree(cornerValues_);
    plat::bigFree(cornerDensity_);
}

bool Router::init(uint64_t seed, float octaveCut, float cellMargin, bool sharedPermutation, bool generated) {
    cellMargin_ = cellMargin;
    generated_ = generated;
    if (sharedPermutation) {
        perm_ = (uint8_t*)malloc(256);
        if (!perm_) return false;
        uint64_t prng = seed ^ 0x5EED5EEDull;
        for (int i = 0; i < 256; i++) perm_[i] = (uint8_t)i;
        for (int i = 0; i < 256; i++) {
            int j = i + (int)(splitmix(prng) % (uint64_t)(256 - i));
            uint8_t t = perm_[i];
            perm_[i] = perm_[j];
            perm_[j] = t;
        }
    }
    perlin_ = (Perlin*)plat::bigAlloc(sizeof(Perlin) * 2 * NUM_DF_NOISES);
    normalFactor_ = (float*)plat::bigAlloc(sizeof(float) * NUM_DF_NOISES);
    blendedNoise_ = (ImprovedNoise*)plat::bigAlloc(sizeof(ImprovedNoise) * 40);
    // touched for every node evaluated: small, in internal RAM (malloc) rather than PSRAM
    memo_ = (float*)malloc(sizeof(float) * NUM_DF_NODES);
    memoStamp_ = (uint32_t*)malloc(sizeof(uint32_t) * NUM_DF_NODES);
    cacheKey_ = (int32_t*)malloc(sizeof(int32_t) * NUM_DF_NODES);
    interpSlot_ = (int16_t*)malloc(sizeof(int16_t) * NUM_DF_NODES);
    if (!perlin_ || !normalFactor_ || !blendedNoise_ || !memo_ || !memoStamp_ || !cacheKey_ || !interpSlot_) return false;
    memset(memoStamp_, 0, sizeof(uint32_t) * NUM_DF_NODES);
    for (int i = 0; i < NUM_DF_NODES; i++) cacheKey_[i] = INT32_MIN;
    for (int n = 0; n < NUM_DF_NOISES; n++) {
        const NoiseDef& d = DF_NOISES[n];
        int lo = -1, hi = -1;
        for (int i = 0; i < d.count; i++)
            if (DF_AMPLITUDES[d.start + i] != 0) {
                if (lo < 0) lo = i;
                hi = i;
            }
        normalFactor_[n] = (1.0f / 6.0f) / expectedDeviation(hi - lo);
        for (int k = 0; k < 2; k++) {   // NormalNoise: two PerlinNoises, the second at a slightly larger scale
            Perlin& p = perlin_[2 * n + k];
            p = Perlin();
            p.first = d.first;
            p.count = d.count;
            p.start = d.start;
            p.levels = (ImprovedNoise*)plat::bigAlloc(sizeof(ImprovedNoise) * d.count);
            if (!p.levels) return false;
            uint64_t rng = seed ^ (0x9E3779B97F4A7C15ull * (uint64_t)(2 * n + k + 1));
            for (int i = 0; i < d.count; i++) p.levels[i].init(rng, perm_);
            p.inFactor = ldexpf(1.0f, d.first);
            p.valFactor = ldexpf(1.0f, d.count - 1) / (ldexpf(1.0f, d.count) - 1);
            // an octave weighs amplitude * valFactor / 2^i
            float top = 0;
            for (int i = 0; i < d.count; i++) top = fmaxf(top, fabsf(DF_AMPLITUDES[d.start + i]) / ldexpf(1.0f, i));
            for (int i = 0; i < d.count; i++)
                if (fabsf(DF_AMPLITUDES[d.start + i]) / ldexpf(1.0f, i) < top * octaveCut) p.skip |= 1u << i;
        }
    }
    // the blended noise's octave q weighs 2^q (q 0: the finest)
    blendedSkip_ = 0;
    while (blendedSkip_ < 8 && ldexpf(1.0f, blendedSkip_) < ldexpf(1.0f, 15) * octaveCut) blendedSkip_++;
    mainSkip_ = 0;
    while (mainSkip_ < 4 && ldexpf(1.0f, mainSkip_) < ldexpf(1.0f, 7) * octaveCut) mainSkip_++;
    uint64_t rng = seed ^ 0xB1E4DEDull;
    for (int i = 0; i < 40; i++) blendedNoise_[i].init(rng, perm_);
    // one corner slot per interpolated node
    interpCount_ = 0;
    for (int i = 0; i < NUM_DF_NODES; i++) interpSlot_[i] = DF_NODES[i].op == DF_INTERPOLATED ? (int16_t)interpCount_++ : -1;
    cornersX_ = 16 / DF_SHAPE.cellW + 1;
    cornersY_ = DF_SHAPE.height / DF_SHAPE.cellH + 1;
    cornerValues_ = (float*)plat::bigAlloc(sizeof(float) * (size_t)(interpCount_ ? interpCount_ : 1) * cornersX_ * cornersX_ * cornersY_);
    cornerDensity_ = (float*)plat::bigAlloc(sizeof(float) * (size_t)cornersX_ * cornersX_ * cornersY_);
    interpNow_ = (float*)malloc(sizeof(float) * (interpCount_ ? interpCount_ : 1));
    // a layer of cells: their corners and whether they are filled without per-block work
    int layerCells = (cornersX_ - 1) * (cornersX_ - 1);
    cellCorners_ = (float*)malloc(sizeof(float) * 8 * (interpCount_ ? interpCount_ : 1) * layerCells);
    cellFill_ = (int8_t*)malloc((size_t)layerCells);
    return cornerValues_ && cornerDensity_ && interpNow_ && cellCorners_ && cellFill_;
}

float Router::noiseValue(int n, float x, float y, float z) const {
    float sum = 0;
    for (int k = 0; k < 2; k++) {
        const Perlin& p = perlin_[2 * n + k];
        float s = k ? 1.0181268882175227f : 1.0f;
        float in = p.inFactor, val = p.valFactor, v = 0;
        for (int i = 0; i < p.count; i++) {
            float amp = DF_AMPLITUDES[p.start + i];
            if (amp != 0 && !(p.skip & (1u << i))) {
                v += amp * p.levels[i].noise(x * s * in, y * s * in, z * s * in) * val;
                ++const_cast<Router*>(this)->octaveSamples;
            }
            in *= 2;
            val *= 0.5f;
        }
        sum += v;
    }
    return sum * normalFactor_[n];
}

// BlendedNoise#compute (old_blended_noise)
float Router::blended(const DfNode& n, int bx, int by, int bz) const {
    float xzMul = 684.412f * n.p[0], yMul = 684.412f * n.p[1];
    float xzFactor = n.p[2], yFactor = n.p[3], smear = n.p[4];
    float d = bx * xzMul, e = by * yMul, f = bz * xzMul;
    float g = d / xzFactor, h = e / yFactor, i = f / xzFactor;
    float j = yMul * smear, k = j / yFactor;
    float main = 0, o = 1;
    for (int q = 0; q < 8; q++) {
        if (q >= mainSkip_) {
            main += blendedNoise_[32 + q].noise(g * o, h * o, i * o, k * o, h * o) / o;
            ++const_cast<Router*>(this)->octaveSamples;
        }
        o *= 0.5f;
    }
    float t = (main / 10 + 1) / 2;
    bool high = t >= 1, low = t <= 0;
    float lo = 0, hi = 0;
    o = 1;
    for (int q = 0; q < 16; q++) {
        float s = d * o, u = e * o, w = f * o, v = j * o;
        if (q >= blendedSkip_) {
            if (!high) lo += blendedNoise_[q].noise(s, u, w, v, u) / o;
            if (!low) hi += blendedNoise_[16 + q].noise(s, u, w, v, u) / o;
            const_cast<Router*>(this)->octaveSamples += !high + !low;
        }
        o *= 0.5f;
    }
    return clampedLerp(lo / 512, hi / 512, t) / 128;
}

// CubicSpline.Multipoint#apply
float Router::spline(int si, int x, int y, int z) {
    const SplineDef& s = DF_SPLINES[si];
    float c = eval(s.coord, x, y, z);
    const SplinePoint* pt = DF_SPLINE_POINTS + s.first;
    int n = s.count;
    int i = -1;   // the last point at or below c
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (c < pt[mid].loc) hi = mid;
        else lo = mid + 1;
    }
    i = lo - 1;
    auto value = [&](int k) { return pt[k].sub >= 0 ? spline(pt[k].sub, x, y, z) : pt[k].value; };
    if (i < 0) return value(0) + pt[0].der * (c - pt[0].loc);
    if (i == n - 1) return value(n - 1) + pt[n - 1].der * (c - pt[n - 1].loc);
    float x0 = pt[i].loc, x1 = pt[i + 1].loc, t = (c - x0) / (x1 - x0);
    float y0 = value(i), y1 = value(i + 1);
    float p = pt[i].der * (x1 - x0) - (y1 - y0), q = -pt[i + 1].der * (x1 - x0) + (y1 - y0);
    return lerp(t, y0, y1) + t * (1 - t) * lerp(t, p, q);
}

float Router::eval(int ni, int x, int y, int z) {
    if (memoStamp_[ni] == stamp_) return memo_[ni];
    const DfNode& n = DF_NODES[ni];
    float v = 0;
    switch (n.op) {
        case DF_CONSTANT: v = n.p[0]; break;
        case DF_ADD: v = eval(n.a, x, y, z) + eval(n.b, x, y, z); break;
        case DF_MUL: {   // vanilla skips the second argument when the first is 0
            float a = eval(n.a, x, y, z);
            v = a == 0 ? 0 : a * eval(n.b, x, y, z);
            break;
        }
        case DF_MIN: v = fminf(eval(n.a, x, y, z), eval(n.b, x, y, z)); break;
        case DF_MAX: v = fmaxf(eval(n.a, x, y, z), eval(n.b, x, y, z)); break;
        case DF_ABS: v = fabsf(eval(n.a, x, y, z)); break;
        case DF_SQUARE: { float a = eval(n.a, x, y, z); v = a * a; break; }
        case DF_CUBE: { float a = eval(n.a, x, y, z); v = a * a * a; break; }
        case DF_HALF_NEGATIVE: { float a = eval(n.a, x, y, z); v = a > 0 ? a : a * 0.5f; break; }
        case DF_QUARTER_NEGATIVE: { float a = eval(n.a, x, y, z); v = a > 0 ? a : a * 0.25f; break; }
        case DF_SQUEEZE: { float c = clampf(eval(n.a, x, y, z), -1, 1); v = c / 2 - c * c * c / 24; break; }
        case DF_CLAMP: v = clampf(eval(n.a, x, y, z), n.p[0], n.p[1]); break;
        case DF_Y_CLAMPED_GRADIENT: v = clampedMap((float)y, n.p[0], n.p[1], n.p[2], n.p[3]); break;
        case DF_RANGE_CHOICE: {
            float a = eval(n.a, x, y, z);
            v = a >= n.p[0] && a < n.p[1] ? eval(n.b, x, y, z) : eval(n.c, x, y, z);
            break;
        }
        case DF_NOISE: v = noiseValue(n.noise, x * n.p[0], y * n.p[1], z * n.p[0]); break;
        case DF_SHIFTED_NOISE:
            v = noiseValue(n.noise, x * n.p[0] + eval(n.a, x, y, z), y * n.p[1] + eval(n.b, x, y, z),
                           z * n.p[0] + eval(n.c, x, y, z));
            break;
        case DF_SHIFT_A: v = noiseValue(n.noise, x * 0.25f, 0, z * 0.25f) * 4; break;
        case DF_SHIFT_B: v = noiseValue(n.noise, z * 0.25f, x * 0.25f, 0) * 4; break;
        case DF_SHIFT: v = noiseValue(n.noise, x * 0.25f, y * 0.25f, z * 0.25f) * 4; break;
        case DF_WEIRD_SCALED_SAMPLER: {
            float a = eval(n.a, x, y, z), r;
            if (n.aux == 1) r = a < -0.5f ? 0.75f : a < 0 ? 1.0f : a < 0.5f ? 1.5f : 2.0f;
            else r = a < -0.75f ? 0.5f : a < -0.5f ? 0.75f : a < 0.5f ? 1.0f : a < 0.75f ? 2.0f : 3.0f;
            v = r * fabsf(noiseValue(n.noise, x / r, y / r, z / r));
            break;
        }
        case DF_OLD_BLENDED_NOISE: v = blended(n, x, y, z); break;
        case DF_SPLINE: v = spline(n.noise, x, y, z); break;
        case DF_INTERPOLATED: {
            // at a cell corner: the argument itself; in the block pass: lerped by fillChunk
            v = corners_ ? eval(n.a, x, y, z) : interpNow_[interpSlot_[ni]];
            break;
        }
        case DF_FLAT_CACHE:
        case DF_CACHE_2D: {
            // per column (flat_cache: per 4x4 column, sampled at y 0)
            bool flat = n.op == DF_FLAT_CACHE;
            int cx = flat ? x & ~3 : x, cz = flat ? z & ~3 : z;
            int32_t key = (int32_t)(((uint32_t)cx & 0xFFFF) << 16 | ((uint32_t)cz & 0xFFFF));
            if (cacheKey_[ni] == key && memoStamp_[ni] != 0) { v = memo_[ni]; break; }
            uint32_t saved = stamp_;
            stamp_ = ++stampCounter_;   // the argument at another position: its own memo
            v = eval(n.a, cx, flat ? 0 : y, cz);
            stamp_ = saved;
            cacheKey_[ni] = key;
            break;
        }
        case DF_CACHE_ONCE:
        case DF_CACHE_ALL_IN_CELL:
        case DF_BLEND_DENSITY: v = eval(n.a, x, y, z); break;
        case DF_BLEND_ALPHA: v = 1; break;
        default: v = 0; break;   // blend_offset, beardifier (no structures yet), end_islands
    }
    memo_[ni] = v;
    memoStamp_[ni] = stamp_;
    return v;
}

float Router::sample(int root, int x, int y, int z) {
    stamp_ = ++stampCounter_;
    corners_ = true;   // interpolated parts at the position itself
    float v = eval(DF_ROOTS[root], x, y, z);
    corners_ = false;
    return v;
}

bool Router::fillChunk(int cx, int cz, uint8_t* out) {
    cellX0_ = cx * 16;
    cellZ0_ = cz * 16;
    octaveSamples = 0;
    int cw = DF_SHAPE.cellW, ch = DF_SHAPE.cellH;
    // 1) cell corners: every interpolated node's argument
    uint64_t t0 = plat::micros();
    corners_ = true;
    size_t plane = (size_t)cornersX_ * cornersX_ * cornersY_;
    for (int iz = 0; iz < cornersX_; iz++)
        for (int ix = 0; ix < cornersX_; ix++)
            for (int iy = 0; iy < cornersY_; iy++) {
                int x = cellX0_ + ix * cw, z = cellZ0_ + iz * cw, y = DF_SHAPE.minY + iy * ch;
                stamp_ = ++stampCounter_;
                for (int i = 0; i < NUM_DF_NODES; i++) {
                    if (interpSlot_[i] < 0) continue;
                    cornerValues_[interpSlot_[i] * plane + ((size_t)iy * cornersX_ + iz) * cornersX_ + ix] =
                        node(DF_NODES[i].a, x, y, z);
                }
                if (cellMargin_ > 0)
                    cornerDensity_[((size_t)iy * cornersX_ + iz) * cornersX_ + ix] = node(DF_ROOTS[ROOT_FINAL_DENSITY], x, y, z);
            }
    corners_ = false;
    uint64_t t1 = plat::micros();
    // 2) every block: the rest of final_density, the interpolated parts lerped
    // 2) every block, a layer of cells at a time: the layer's corners in internal RAM, the
    // blocks in memory order (y, z, x)
    int root = DF_ROOTS[ROOT_FINAL_DENSITY];
    cellsSkipped = cellsTotal = 0;
    const int cells = cornersX_ - 1;   // per side
    for (int cy = 0; cy < cornersY_ - 1; cy++) {
        for (int cz = 0; cz < cells; cz++)
            for (int cxi = 0; cxi < cells; cxi++) {
                int cell = cz * cells + cxi;
                cellsTotal++;
                int fill = -1;   // 1 solid, 0 air: all corners clearly on one side
                if (cellMargin_ > 0) {
                    bool solid = true, air = true;
                    for (int k = 0; k < 8; k++) {
                        float d = cornerDensity_[((size_t)(cy + (k >> 2)) * cornersX_ + cz + ((k >> 1) & 1)) * cornersX_ + cxi + (k & 1)];
                        solid &= d > cellMargin_;
                        air &= d < -cellMargin_;
                    }
                    fill = solid ? 1 : air ? 0 : -1;
                }
                cellFill_[cell] = (int8_t)fill;
                if (fill >= 0) { cellsSkipped++; continue; }
                for (int sl = 0; sl < interpCount_; sl++)
                    for (int k = 0; k < 8; k++)
                        cellCorners_[(cell * interpCount_ + sl) * 8 + k] =
                            cornerValues_[sl * plane + ((size_t)(cy + (k >> 2)) * cornersX_ + cz + ((k >> 1) & 1)) * cornersX_ + cxi + (k & 1)];
            }
        for (int dy = 0; dy < ch; dy++) {
            int ly = cy * ch + dy;
            float fy = (float)dy / ch;
            uint8_t* row = out + (size_t)ly * 256;
            for (int lz = 0; lz < 16; lz++) {
                float fz = (float)(lz % cw) / cw;
                for (int lx = 0; lx < 16; lx++) {
                    int cell = (lz / cw) * cells + lx / cw;
                    int fill = cellFill_[cell];
                    if (fill >= 0) { row[lz * 16 + lx] = (uint8_t)fill; continue; }
                    float fx = (float)(lx % cw) / cw;
                    for (int sl = 0; sl < interpCount_; sl++) {
                        const float* c = cellCorners_ + (cell * interpCount_ + sl) * 8;
                        interpNow_[sl] = lerp(fy, lerp(fz, lerp(fx, c[0], c[1]), lerp(fx, c[2], c[3])),
                                              lerp(fz, lerp(fx, c[4], c[5]), lerp(fx, c[6], c[7])));
                    }
                    stamp_ = ++stampCounter_;
                    row[lz * 16 + lx] = node(root, cellX0_ + lx, DF_SHAPE.minY + ly, cellZ0_ + lz) > 0;
                }
            }
        }
    }
    uint64_t t2 = plat::micros();
    cornerUs = (uint32_t)(t1 - t0);
    blockUs = (uint32_t)(t2 - t1);
    return true;
}

}  // namespace vanilla
}  // namespace mc
