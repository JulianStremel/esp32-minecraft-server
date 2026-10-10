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

// The largest |ImprovedNoise| for the air bound: every value is a convex combination of
// eight gradient dot products, each at most 2 (vanilla's PerlinNoise#maxValue uses 2 too)
constexpr float NOISE_MAX = 2.0f;
constexpr float IV_BIG = 1e18f;
constexpr int PRELIM_CACHE = 1024;   // "unbounded", finite so that products stay numbers
inline float ivc(float v) { return v < -IV_BIG ? -IV_BIG : v > IV_BIG ? IV_BIG : v; }

}  // namespace

int noiseIndex(const char* name) {
    for (int i = 0; i < NUM_DF_NOISES; i++)
        if (!strcmp(DF_NOISE_NAMES[i], name)) return i;
    return -1;
}

void ImprovedNoise::init(uint64_t& rng, const uint8_t* shared, uint8_t* own) {
    xo_ = unit(rng) * 256;
    yo_ = unit(rng) * 256;
    zo_ = unit(rng) * 256;
    p_ = shared ? shared : own;
    if (shared) return;
    for (int i = 0; i < 256; i++) own[i] = (uint8_t)i;
    for (int i = 0; i < 256; i++) {
        int j = i + (int)(splitmix(rng) % (uint64_t)(256 - i));
        uint8_t t = own[i];
        own[i] = own[j];
        own[j] = t;
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
    plat::bigFree(perlin_);
    plat::bigFree(normalFactor_);
    if (octavesInternal_) free(octaves_);
    else plat::bigFree(octaves_);
    plat::bigFree(ownPerms_);
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
    free(flatSlot_);
    free(colFlat_);
    free(noiseBound_);
    free(iv_);
    free(ivStamp_);
    free(ivCell_);
    free(cellTop_);
    free(cornerTop_);
    free(cellState_);
    free(cornerFull_);
    free(interpStamp_);
    plat::bigFree(prelim_);
    free(zLerps_);
    free(xLerps_);
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
    // every octave in one block (each NormalNoise two PerlinNoises, then the blended
    // noise's 40), in internal RAM if there is room: read for every sample
    int levels = 40;
    for (int n = 0; n < NUM_DF_NOISES; n++) levels += 2 * DF_NOISES[n].count;
    octaves_ = (ImprovedNoise*)malloc(sizeof(ImprovedNoise) * levels);
    octavesInternal_ = octaves_ != nullptr;
    if (!octaves_) octaves_ = (ImprovedNoise*)plat::bigAlloc(sizeof(ImprovedNoise) * levels);
    if (!sharedPermutation) ownPerms_ = (uint8_t*)plat::bigAlloc((size_t)256 * levels);
    if (!octaves_ || (!sharedPermutation && !ownPerms_)) return false;
    int nextLevel = 0;
    auto take = [&](int count) {
        ImprovedNoise* l = octaves_ + nextLevel;
        nextLevel += count;
        return l;
    };
    auto own = [&](ImprovedNoise* l) { return ownPerms_ ? ownPerms_ + (size_t)256 * (l - octaves_) : nullptr; };
    blendedNoise_ = take(40);
    // touched for every node evaluated: small, in internal RAM (malloc) rather than PSRAM
    memo_ = (float*)malloc(sizeof(float) * NUM_DF_NODES);
    memoStamp_ = (uint32_t*)malloc(sizeof(uint32_t) * NUM_DF_NODES);
    cacheKey_ = (int32_t*)malloc(sizeof(int32_t) * NUM_DF_NODES);
    interpSlot_ = (int16_t*)malloc(sizeof(int16_t) * NUM_DF_NODES);
    flatSlot_ = (int16_t*)malloc(sizeof(int16_t) * NUM_DF_NODES);
    noiseBound_ = (float*)malloc(sizeof(float) * NUM_DF_NOISES);
    iv_ = (Iv*)malloc(sizeof(Iv) * NUM_DF_NODES);
    ivStamp_ = (uint32_t*)malloc(sizeof(uint32_t) * NUM_DF_NODES);
    if (!perlin_ || !normalFactor_ || !blendedNoise_ || !memo_ || !memoStamp_ || !cacheKey_ || !interpSlot_ ||
        !flatSlot_ || !noiseBound_ || !iv_ || !ivStamp_)
        return false;
    memset(ivStamp_, 0, sizeof(uint32_t) * NUM_DF_NODES);
    numFlat_ = 0;
    for (int i = 0; i < NUM_DF_NODES; i++) flatSlot_[i] = DF_NODES[i].op == DF_FLAT_CACHE ? (int16_t)numFlat_++ : -1;
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
            p.levels = take(d.count);
            uint64_t rng = seed ^ (0x9E3779B97F4A7C15ull * (uint64_t)(2 * n + k + 1));
            for (int i = 0; i < d.count; i++) p.levels[i].init(rng, perm_, own(p.levels + i));
            p.inFactor = ldexpf(1.0f, d.first);
            p.valFactor = ldexpf(1.0f, d.count - 1) / (ldexpf(1.0f, d.count) - 1);
            // an octave weighs amplitude * valFactor / 2^i
            float top = 0;
            for (int i = 0; i < d.count; i++) top = fmaxf(top, fabsf(DF_AMPLITUDES[d.start + i]) / ldexpf(1.0f, i));
            for (int i = 0; i < d.count; i++)
                if (fabsf(DF_AMPLITUDES[d.start + i]) / ldexpf(1.0f, i) < top * octaveCut) p.skip |= 1u << i;
        }
        float bound = 0;   // as noiseValue sums
        for (int k = 0; k < 2; k++) {
            const Perlin& p = perlin_[2 * n + k];
            float val = p.valFactor;
            for (int i = 0; i < p.count; i++) {
                if (!(p.skip & (1u << i))) bound += fabsf(DF_AMPLITUDES[p.start + i]) * NOISE_MAX * val;
                val *= 0.5f;
            }
        }
        noiseBound_[n] = bound * normalFactor_[n] * 1.001f;
    }
    // the blended noise's octave q weighs 2^q (q 0: the finest)
    blendedSkip_ = 0;
    while (blendedSkip_ < 8 && ldexpf(1.0f, blendedSkip_) < ldexpf(1.0f, 15) * octaveCut) blendedSkip_++;
    mainSkip_ = 0;
    while (mainSkip_ < 4 && ldexpf(1.0f, mainSkip_) < ldexpf(1.0f, 7) * octaveCut) mainSkip_++;
    // the blended noise: the limit noises' octaves q (weight 2^q) / 512 / 128
    blendedBound_ = 0;
    for (int q = blendedSkip_; q < 16; q++) blendedBound_ += ldexpf(1.0f, q) * NOISE_MAX;
    blendedBound_ = blendedBound_ / 512 / 128 * 1.001f;
    uint64_t rng = seed ^ 0xB1E4DEDull;
    for (int i = 0; i < 40; i++) blendedNoise_[i].init(rng, perm_, own(blendedNoise_ + i));
    // one corner slot per interpolated node
    interpCount_ = 0;
    for (int i = 0; i < NUM_DF_NODES; i++) interpSlot_[i] = DF_NODES[i].op == DF_INTERPOLATED ? (int16_t)interpCount_++ : -1;
    cornersX_ = 16 / DF_SHAPE.cellW + 1;
    cornersY_ = DF_SHAPE.height / DF_SHAPE.cellH + 1;
    cornerValues_ = (float*)plat::bigAlloc(sizeof(float) * (size_t)(interpCount_ ? interpCount_ : 1) * cornersX_ * cornersX_ * cornersY_);
    cornerDensity_ = (float*)plat::bigAlloc(sizeof(float) * (size_t)cornersX_ * cornersX_ * cornersY_);
    interpNow_ = (float*)malloc(sizeof(float) * (interpCount_ ? interpCount_ : 1));
    interpStamp_ = (uint32_t*)calloc(interpCount_ ? interpCount_ : 1, sizeof(uint32_t));
    zLerps_ = (float*)malloc(sizeof(float) * 32 * 8);
    xLerps_ = (float*)malloc(sizeof(float) * 8 * 16 * 4);
    // a layer of cells: their corners and whether they are filled without per-block work
    int layerCells = (cornersX_ - 1) * (cornersX_ - 1);
    cellCorners_ = (float*)malloc(sizeof(float) * 8 * (interpCount_ ? interpCount_ : 1) * layerCells);
    cellFill_ = (int8_t*)malloc((size_t)layerCells);
    colFlat_ = (float*)malloc(sizeof(float) * (size_t)(numFlat_ ? numFlat_ : 1) * cornersX_ * cornersX_);
    ivCell_ = (Iv*)malloc(sizeof(Iv) * (interpCount_ ? interpCount_ : 1));
    cellTop_ = (uint8_t*)malloc((size_t)layerCells);
    cornerTop_ = (uint8_t*)malloc((size_t)cornersX_ * cornersX_);
    cellState_ = (int8_t*)malloc((size_t)layerCells * (cornersY_ - 1));
    cornerFull_ = (uint8_t*)malloc((size_t)cornersX_ * cornersX_ * cornersY_);
    {
        const DfNode& f = DF_NODES[DF_ROOTS[ROOT_FINAL_DENSITY]];
        signSlot_ = chainSlot(DF_ROOTS[ROOT_FINAL_DENSITY]);
        signNode_ = DF_ROOTS[ROOT_FINAL_DENSITY];
        if (signSlot_ < 0 && f.op == DF_MIN) {
            signSlot_ = chainSlot(f.a);
            signNode_ = f.a;
            if (signSlot_ < 0) {
                signSlot_ = chainSlot(f.b);
                signNode_ = f.b;
            }
        }
    }
    prelim_ = (PrelimEntry*)plat::bigAlloc(sizeof(PrelimEntry) * PRELIM_CACHE);
    if (prelim_)
        for (int i = 0; i < PRELIM_CACHE; i++) prelim_[i].key = INT32_MIN;
    // the flat_cache table needs corner columns 4 blocks apart (flat_cache's own grid)
    if (DF_SHAPE.cellW != 4) airBound = false;
    return cornerValues_ && cornerDensity_ && interpNow_ && cellCorners_ && cellFill_ && colFlat_ && ivCell_ && cellTop_ &&
           cornerTop_ && cellState_ && cornerFull_ && interpStamp_ && zLerps_ && xLerps_;
}

// the interpolated node n stands on, through functions that keep a value's sign (0: not
// above 0) and grow with it; -1 if not such a chain
int Router::chainSlot(int ni) const {
    const DfNode& n = DF_NODES[ni];
    switch (n.op) {
        case DF_INTERPOLATED: return interpSlot_[ni];
        case DF_SQUEEZE: case DF_HALF_NEGATIVE: case DF_QUARTER_NEGATIVE: case DF_CUBE:
        case DF_CACHE_ONCE: case DF_CACHE_ALL_IN_CELL: case DF_BLEND_DENSITY:
            return chainSlot(n.a);
        case DF_MUL:
            if (DF_NODES[n.a].op == DF_CONSTANT && DF_NODES[n.a].p[0] > 0) return chainSlot(n.b);
            if (DF_NODES[n.b].op == DF_CONSTANT && DF_NODES[n.b].p[0] > 0) return chainSlot(n.a);
            return -1;
        default: return -1;
    }
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
    // o and inv: a power of two and its reciprocal, so x * inv is exactly x / o
    float main = 0, o = 1, inv = 1;
    for (int q = 0; q < 8; q++) {
        if (q >= mainSkip_) {
            main += blendedNoise_[32 + q].noise(g * o, h * o, i * o, k * o, h * o) * inv;
            ++const_cast<Router*>(this)->octaveSamples;
        }
        o *= 0.5f;
        inv *= 2;
    }
    float t = (main / 10 + 1) / 2;
    bool high = t >= 1, low = t <= 0;
    float lo = 0, hi = 0;
    o = 1;
    inv = 1;
    for (int q = 0; q < 16; q++) {
        float s = d * o, u = e * o, w = f * o, v = j * o;
        if (q >= blendedSkip_) {
            if (!high) lo += blendedNoise_[q].noise(s, u, w, v, u) * inv;
            if (!low) hi += blendedNoise_[16 + q].noise(s, u, w, v, u) * inv;
            const_cast<Router*>(this)->octaveSamples += !high + !low;
        }
        o *= 0.5f;
        inv *= 2;
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
            v = corners_ ? eval(n.a, x, y, z) : interpAt(interpSlot_[ni]);
            break;
        }
        case DF_FLAT_CACHE:
        case DF_CACHE_2D: {
            // per column (flat_cache: per 4x4 column, sampled at y 0)
            bool flat = n.op == DF_FLAT_CACHE;
            int cx = flat ? x & ~3 : x, cz = flat ? z & ~3 : z;
            if (flat && flatTable(ni, cx, cz, v)) break;
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

int Router::preliminarySurface(int x, int z) {
    x &= ~3;
    z &= ~3;
    int32_t key = (int32_t)(((uint32_t)(x >> 2) & 0xFFFF) << 16 | ((uint32_t)(z >> 2) & 0xFFFF));
    uint32_t h = ((uint32_t)key * 2654435761u) >> 22;   // PRELIM_CACHE = 1024
    if (prelim_ && prelim_[h].key == key) return prelim_[h].level;
    int level = INT32_MAX;
    for (int y = DF_SHAPE.minY + DF_SHAPE.height; y >= DF_SHAPE.minY; y -= DF_SHAPE.cellH)
        if (sample(ROOT_INITIAL_DENSITY, x, y, z) > 0.390625f) {
            level = y;
            break;
        }
    if (prelim_) prelim_[h] = {key, level};
    return level;
}

float Router::densityBound(int lx, int y, int lz) {
    const int cw = DF_SHAPE.cellW, ch = DF_SHAPE.cellH, cells = cornersX_ - 1;
    int ly = y - DF_SHAPE.minY;
    if (signSlot_ < 0 || ly < 0 || ly >= DF_SHAPE.height) return 0;
    int cxi = lx / cw, czi = lz / cw, cy = ly / ch;
    if (cy >= cellTop_[czi * cells + cxi]) return -1;
    size_t plane = (size_t)cornersX_ * cornersX_ * cornersY_;
    const float* base = cornerValues_ + signSlot_ * plane;
    float c[8];
    for (int k = 0; k < 8; k++)
        c[k] = base[((size_t)(cy + (k >> 2)) * cornersX_ + czi + ((k >> 1) & 1)) * cornersX_ + cxi + (k & 1)];
    float fx = (float)(lx % cw) / cw, fy = (float)(ly % ch) / ch, fz = (float)(lz % cw) / cw;
    float a = lerp(fy, lerp(fz, lerp(fx, c[0], c[1]), lerp(fx, c[2], c[3])), lerp(fz, lerp(fx, c[4], c[5]), lerp(fx, c[6], c[7])));
    corners_ = false;
    blockCorners_ = nullptr;
    blockZ_ = nullptr;
    stamp_ = ++stampCounter_;
    interpNow_[signSlot_] = a;
    interpStamp_[signSlot_] = stamp_;
    return node(signNode_, cellX0_ + lx, y, cellZ0_ + lz);
}

float Router::sample(int root, int x, int y, int z) {
    stamp_ = ++stampCounter_;
    corners_ = true;   // interpolated parts at the position itself
    float v = node(DF_ROOTS[root], x, y, z);   // the generated code if there is (the same values)
    corners_ = false;
    return v;
}

// The range of node n: at corner column ivCol_ for y in [ivY0_, ivY1_] (the 2D parts from
// the column table), or inside a cell (ivCol_ -1: the interpolated nodes from ivInterp_).
// Noises at their largest possible values: never narrower than the true range.
Router::Iv Router::interval(int ni) {
    if (ivStamp_[ni] == ivStampN_) return iv_[ni];
    const DfNode& n = DF_NODES[ni];
    Iv r = {-IV_BIG, IV_BIG};
    auto a = [&]() { return interval(n.a); };
    switch (n.op) {
        case DF_CONSTANT: r = {n.p[0], n.p[0]}; break;
        case DF_ADD: { Iv x = a(), y = interval(n.b); r = {x.lo + y.lo, x.hi + y.hi}; break; }
        case DF_MUL: {
            Iv x = a(), y = interval(n.b);
            float p0 = x.lo * y.lo, p1 = x.lo * y.hi, p2 = x.hi * y.lo, p3 = x.hi * y.hi;
            r = {fminf(fminf(p0, p1), fminf(p2, p3)), fmaxf(fmaxf(p0, p1), fmaxf(p2, p3))};
            break;
        }
        case DF_MIN: { Iv x = a(), y = interval(n.b); r = {fminf(x.lo, y.lo), fminf(x.hi, y.hi)}; break; }
        case DF_MAX: { Iv x = a(), y = interval(n.b); r = {fmaxf(x.lo, y.lo), fmaxf(x.hi, y.hi)}; break; }
        case DF_ABS: {
            Iv x = a();
            if (x.lo >= 0) r = x;
            else if (x.hi <= 0) r = {-x.hi, -x.lo};
            else r = {0, fmaxf(-x.lo, x.hi)};
            break;
        }
        case DF_SQUARE: {
            Iv x = a();
            float l = x.lo * x.lo, h = x.hi * x.hi;
            r = x.lo >= 0 ? Iv{l, h} : x.hi <= 0 ? Iv{h, l} : Iv{0, fmaxf(l, h)};
            break;
        }
        case DF_CUBE: { Iv x = a(); r = {x.lo * x.lo * x.lo, x.hi * x.hi * x.hi}; break; }
        case DF_HALF_NEGATIVE: { Iv x = a(); r = {x.lo > 0 ? x.lo : x.lo * 0.5f, x.hi > 0 ? x.hi : x.hi * 0.5f}; break; }
        case DF_QUARTER_NEGATIVE: { Iv x = a(); r = {x.lo > 0 ? x.lo : x.lo * 0.25f, x.hi > 0 ? x.hi : x.hi * 0.25f}; break; }
        case DF_SQUEEZE: {   // increasing on [-1, 1]
            Iv x = a();
            float l = clampf(x.lo, -1, 1), h = clampf(x.hi, -1, 1);
            r = {l / 2 - l * l * l / 24, h / 2 - h * h * h / 24};
            break;
        }
        case DF_CLAMP: { Iv x = a(); r = {clampf(x.lo, n.p[0], n.p[1]), clampf(x.hi, n.p[0], n.p[1])}; break; }
        case DF_Y_CLAMPED_GRADIENT: {
            float l = clampedMap(ivY0_, n.p[0], n.p[1], n.p[2], n.p[3]), h = clampedMap(ivY1_, n.p[0], n.p[1], n.p[2], n.p[3]);
            r = {fminf(l, h), fmaxf(l, h)};
            break;
        }
        case DF_RANGE_CHOICE: {
            Iv x = a();
            if (x.lo >= n.p[0] && x.hi < n.p[1]) r = interval(n.b);
            else if (x.hi < n.p[0] || x.lo >= n.p[1]) r = interval(n.c);
            else { Iv y = interval(n.b), z = interval(n.c); r = {fminf(y.lo, z.lo), fmaxf(y.hi, z.hi)}; }
            break;
        }
        case DF_NOISE: case DF_SHIFTED_NOISE: r = {-noiseBound_[n.noise], noiseBound_[n.noise]}; break;
        case DF_SHIFT_A: case DF_SHIFT_B: case DF_SHIFT: r = {-4 * noiseBound_[n.noise], 4 * noiseBound_[n.noise]}; break;
        case DF_WEIRD_SCALED_SAMPLER: r = {0, (n.aux == 1 ? 2.0f : 3.0f) * noiseBound_[n.noise]}; break;
        case DF_OLD_BLENDED_NOISE: r = {-blendedBound_, blendedBound_}; break;
        case DF_INTERPOLATED: r = ivInterp_ ? ivInterp_[interpSlot_[ni]] : a(); break;
        case DF_FLAT_CACHE:
            if (ivCol_ >= 0) {   // exact: the value at this corner column
                float v = colFlat_[ivCol_ * numFlat_ + flatSlot_[ni]];
                r = {v, v};
            } else {
                r = a();
            }
            break;
        case DF_CACHE_2D: case DF_CACHE_ONCE: case DF_CACHE_ALL_IN_CELL: case DF_BLEND_DENSITY: r = a(); break;
        case DF_BLEND_ALPHA: r = {1, 1}; break;
        case DF_BLEND_OFFSET: case DF_BEARDIFIER: r = {0, 0}; break;
        default: break;   // a spline outside a flat_cache, end_islands: unbounded
    }
    r.lo = ivc(r.lo);
    r.hi = ivc(r.hi);
    if (!(r.lo <= r.hi)) r = {-IV_BIG, IV_BIG};   // NaN
    iv_[ni] = r;
    ivStamp_[ni] = ivStampN_;
    return r;
}

// Is every block of cell column `cell` from cell layer cy up air for certain? The
// interpolated nodes' corner values over the cell's four corner columns and y from the
// layer to the top, then final_density inside a cell from them.
bool Router::cellAir(int cell, int cy) {
    const int cells = cornersX_ - 1;
    int cxi = cell % cells, czi = cell / cells;
    for (int sl = 0; sl < interpCount_; sl++) ivCell_[sl] = {IV_BIG, -IV_BIG};
    ivY0_ = (float)(DF_SHAPE.minY + cy * DF_SHAPE.cellH);
    ivY1_ = (float)(DF_SHAPE.minY + (cornersY_ - 1) * DF_SHAPE.cellH);
    ivInterp_ = nullptr;
    for (int k = 0; k < 4; k++) {
        ivCol_ = (czi + (k >> 1)) * cornersX_ + cxi + (k & 1);
        ++ivStampN_;
        for (int i = 0; i < NUM_DF_NODES; i++) {
            int sl = interpSlot_[i];
            if (sl < 0) continue;
            Iv v = interval(DF_NODES[i].a);
            ivCell_[sl].lo = fminf(ivCell_[sl].lo, v.lo);
            ivCell_[sl].hi = fmaxf(ivCell_[sl].hi, v.hi);
        }
    }
    ivCol_ = -1;
    ivInterp_ = ivCell_;
    ++ivStampN_;
    Iv d = interval(DF_ROOTS[ROOT_FINAL_DENSITY]);
    ivInterp_ = nullptr;
    return d.hi <= 0;   // air: final_density never above 0
}

bool Router::fillChunk(int cx, int cz, uint8_t* out) {
    cellX0_ = cx * 16;
    cellZ0_ = cz * 16;
    octaveSamples = 0;
    cornersEvaluated = 0;
    int cw = DF_SHAPE.cellW, ch = DF_SHAPE.cellH;
    const int cells = cornersX_ - 1;   // per side
    const int layers = cornersY_ - 1;
    uint64_t t0 = plat::micros();
    // 0) the 2D parts at the corner columns (kept until the next chunk: the biomes read the
    // climate there), and the cell layers that are air for certain
    colFlatReady_ = false;
    corners_ = true;
    for (int c = 0; c < cornersX_ * cornersX_; c++) {
        int x = cellX0_ + (c % cornersX_) * cw, z = cellZ0_ + (c / cornersX_) * cw;
        for (int i = 0; i < NUM_DF_NODES; i++)
            if (flatSlot_[i] >= 0) {
                stamp_ = ++stampCounter_;
                colFlat_[c * numFlat_ + flatSlot_[i]] = node(i, x, 0, z);
            }
    }
    colFlatReady_ = true;
    if (airBound && signSlot_ >= 0) {
        // with a sign slot a cell is air where that node is not above 0 at its corners: a
        // bound per corner column, a cell from the highest of its four
        int signNode = -1;
        for (int i = 0; i < NUM_DF_NODES; i++)
            if (interpSlot_[i] == signSlot_) signNode = DF_NODES[i].a;
        ivInterp_ = nullptr;
        ivY1_ = (float)(DF_SHAPE.minY + layers * ch);
        for (int c = 0; c < cornersX_ * cornersX_; c++) {
            ivCol_ = c;
            int lo = 0, hi = cornersY_;   // the lowest corner from which up the node is not above 0
            while (lo < hi) {
                int mid = (lo + hi) / 2;
                ivY0_ = (float)(DF_SHAPE.minY + mid * ch);
                ++ivStampN_;
                if (interval(signNode).hi <= 0) hi = mid;
                else lo = mid + 1;
            }
            cornerTop_[c] = (uint8_t)lo;
        }
        ivCol_ = -1;
        for (int cell = 0; cell < cells * cells; cell++) {
            int cxi = cell % cells, czi = cell / cells, top = 0;
            for (int k = 0; k < 4; k++) {
                int t = cornerTop_[(czi + (k >> 1)) * cornersX_ + cxi + (k & 1)];
                if (t > top) top = t;
            }
            cellTop_[cell] = (uint8_t)(top < layers ? top : layers);
        }
    } else if (airBound) {
        for (int cell = 0; cell < cells * cells; cell++) {
            // the lowest layer from which up everything is air (monotone: the range from a
            // higher layer up is narrower)
            int lo = 0, hi = layers;
            if (!cellAir(cell, layers - 1)) lo = layers;
            else hi = layers - 1;
            while (lo < hi) {
                int mid = (lo + hi) / 2;
                if (cellAir(cell, mid)) hi = mid;
                else lo = mid + 1;
            }
            cellTop_[cell] = (uint8_t)lo;
        }
    } else {
        for (int cell = 0; cell < cells * cells; cell++) cellTop_[cell] = (uint8_t)layers;
    }
    // a corner column is needed up to the highest bound of its cells
    for (int c = 0; c < cornersX_ * cornersX_; c++) {
        int ix = c % cornersX_, iz = c / cornersX_, top = 0;
        for (int k = 0; k < 4; k++) {
            int ccx = ix - (k & 1), ccz = iz - (k >> 1);
            if (ccx < 0 || ccz < 0 || ccx >= cells || ccz >= cells) continue;
            if (cellTop_[ccz * cells + ccx] > top) top = cellTop_[ccz * cells + ccx];
        }
        cornerTop_[c] = (uint8_t)top;
    }
    uint64_t tb = plat::micros();
    // 1) cell corners: every interpolated node's argument; with a sign slot that one first,
    // the others only at corners of cells it does not make air
    size_t plane = (size_t)cornersX_ * cornersX_ * cornersY_;
    auto cornerAt = [&](int iy, int iz, int ix) { return ((size_t)iy * cornersX_ + iz) * cornersX_ + ix; };
    auto evalCorner = [&](int iy, int iz, int ix, bool all) {
        int x = cellX0_ + ix * cw, z = cellZ0_ + iz * cw, y = DF_SHAPE.minY + iy * ch;
        size_t at = cornerAt(iy, iz, ix);
        corners_ = true;
        stamp_ = ++stampCounter_;
        for (int i = 0; i < NUM_DF_NODES; i++) {
            int sl = interpSlot_[i];
            if (sl < 0 || (sl == signSlot_) == all) continue;
            cornerValues_[sl * plane + at] = node(DF_NODES[i].a, x, y, z);
        }
        if (!all || cellMargin_ <= 0) return;
        // final_density at the corner: the interpolated nodes read back as in the block pass
        corners_ = false;
        blockCorners_ = nullptr;
        blockZ_ = nullptr;
        stamp_ = ++stampCounter_;
        for (int sl = 0; sl < interpCount_; sl++) {
            interpNow_[sl] = cornerValues_[sl * plane + at];
            interpStamp_[sl] = stamp_;
        }
        cornerDensity_[at] = node(DF_ROOTS[ROOT_FINAL_DENSITY], x, y, z);
    };
    // corner flags: what is known and what is needed
    enum : uint8_t { C_SIGN = 1, C_FULL = 2 };
    memset(cornerFull_, 0, plane);
    uint64_t tp = plat::micros();
    // a) the cells below the bound need their corners' sign slot
    for (int cy = 0; cy < layers; cy++)
        for (int cell = 0; cell < cells * cells; cell++) {
            int8_t& st = cellState_[cy * cells * cells + cell];
            st = -1;
            if (cy >= cellTop_[cell]) { st = 0; continue; }
            int czi = cell / cells, cxi = cell % cells;
            for (int k = 0; k < 8; k++) cornerFull_[cornerAt(cy + (k >> 2), czi + ((k >> 1) & 1), cxi + (k & 1))] |= C_SIGN;
        }
    if (signSlot_ >= 0) {
        for (int iy = 0; iy < cornersY_; iy++)
            for (int iz = 0; iz < cornersX_; iz++)
                for (int ix = 0; ix < cornersX_; ix++)
                    if (cornerFull_[cornerAt(iy, iz, ix)] & C_SIGN) {
                        cornersEvaluated++;
                        evalCorner(iy, iz, ix, false);
                    }
    }
    uint64_t ts = plat::micros();
    // b) the cells the sign slot does not make air need every interpolated node at their corners
    for (int cy = 0; cy < layers; cy++)
        for (int cell = 0; cell < cells * cells; cell++) {
            int8_t& st = cellState_[cy * cells * cells + cell];
            if (st == 0) continue;
            int czi = cell / cells, cxi = cell % cells;
            if (signSlot_ >= 0) {
                bool air = true;
                for (int k = 0; k < 8 && air; k++)
                    air = cornerValues_[signSlot_ * plane + cornerAt(cy + (k >> 2), czi + ((k >> 1) & 1), cxi + (k & 1))] <= 0;
                if (air) { st = 0; continue; }
            }
            for (int k = 0; k < 8; k++) cornerFull_[cornerAt(cy + (k >> 2), czi + ((k >> 1) & 1), cxi + (k & 1))] |= C_FULL;
        }
    for (int iy = 0; iy < cornersY_; iy++)
        for (int iz = 0; iz < cornersX_; iz++)
            for (int ix = 0; ix < cornersX_; ix++)
                if (cornerFull_[cornerAt(iy, iz, ix)] & C_FULL) {
                    if (signSlot_ < 0) cornersEvaluated++;
                    evalCorner(iy, iz, ix, true);
                }
    corners_ = false;
    uint64_t t1 = plat::micros();
    // 2) every block, a layer of cells at a time: the layer's corners in internal RAM; the
    // interpolated parts lerped
    int root = DF_ROOTS[ROOT_FINAL_DENSITY];
    cellsSkipped = cellsTotal = cellsAbove = 0;
    blocksEvaluated = blocksSignAir = cellsProven = 0;
    float fxs[16], fys[16];   // a block's position in its cell (cells are at most 16 wide and high)
    for (int k = 0; k < cw; k++) fxs[k] = (float)k / cw;
    for (int k = 0; k < ch; k++) fys[k] = (float)k / ch;
    for (int cy = 0; cy < layers; cy++) {
        for (int cz = 0; cz < cells; cz++)
            for (int cxi = 0; cxi < cells; cxi++) {
                int cell = cz * cells + cxi;
                cellsTotal++;
                int fill = cellState_[cy * cells * cells + cell];   // 1 solid, 0 air: all corners clearly on one side
                if (cy >= cellTop_[cell]) cellsAbove++;
                if (fill < 0) {
                    // final_density over the cell from its interpolated nodes' ranges (a
                    // trilinear lerp stays within its corners): all solid or all air for
                    // certain, as block by block would find
                    for (int sl = 0; sl < interpCount_; sl++) {
                        float lo = IV_BIG, hi = -IV_BIG;
                        for (int k = 0; k < 8; k++) {
                            float v = cornerValues_[sl * plane + cornerAt(cy + (k >> 2), cz + ((k >> 1) & 1), cxi + (k & 1))];
                            lo = v < lo ? v : lo;
                            hi = v > hi ? v : hi;
                        }
                        ivCell_[sl] = {lo, hi};
                    }
                    ivCol_ = -1;
                    ivInterp_ = ivCell_;
                    ivY0_ = (float)(DF_SHAPE.minY + cy * ch);
                    ivY1_ = (float)(DF_SHAPE.minY + cy * ch + ch - 1);
                    ++ivStampN_;
                    Iv d = interval(root);
                    ivInterp_ = nullptr;
                    if (d.lo > 0) fill = 1;
                    else if (d.hi <= 0) fill = 0;
                    if (fill >= 0) cellsProven++;
                }
                if (fill < 0 && cellMargin_ > 0) {
                    bool solid = true, air = true;
                    for (int k = 0; k < 8; k++) {
                        float d = cornerDensity_[cornerAt(cy + (k >> 2), cz + ((k >> 1) & 1), cxi + (k & 1))];
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
                            cornerValues_[sl * plane + cornerAt(cy + (k >> 2), cz + ((k >> 1) & 1), cxi + (k & 1))];
            }
        // a cell at a time: filled cells written whole; in the others the sign slot's lerps
        // shared over the cell (the same arithmetic, in the same order, as interpAt)
        for (int cell = 0; cell < cells * cells; cell++) {
            int cz = cell / cells, cxi = cell % cells, fill = cellFill_[cell];
            uint8_t* base = out + (size_t)cy * ch * 256 + cz * cw * 16 + cxi * cw;
            if (fill >= 0) {
                for (int dy = 0; dy < ch; dy++)
                    for (int dz = 0; dz < cw; dz++) memset(base + dy * 256 + dz * 16, fill, (size_t)cw);
                continue;
            }
            blockCorners_ = cellCorners_ + cell * interpCount_ * 8;
            // per interpolated node and x: the four x lerps; per z then the two z lerps
            // (zLerps_: node * 32 + x, + 16 + x); a block lerps y alone (interpAt)
            float (*xl)[16][4] = (float (*)[16][4])xLerps_;   // [8][16][4], not on the stack
            const int nodes = interpCount_ < 8 ? interpCount_ : 8;
            for (int sl = 0; sl < nodes; sl++) {
                const float* c = blockCorners_ + sl * 8;
                for (int dx = 0; dx < cw; dx++) {
                    float fx = fxs[dx];
                    xl[sl][dx][0] = lerp(fx, c[0], c[1]);
                    xl[sl][dx][1] = lerp(fx, c[2], c[3]);
                    xl[sl][dx][2] = lerp(fx, c[4], c[5]);
                    xl[sl][dx][3] = lerp(fx, c[6], c[7]);
                }
            }
            blockZ_ = interpCount_ <= 8 ? zLerps_ : nullptr;
            for (int dz = 0; dz < cw; dz++) {
                float fz = fxs[dz];
                for (int sl = 0; sl < nodes; sl++)
                    for (int dx = 0; dx < cw; dx++) {
                        zLerps_[sl * 32 + dx] = lerp(fz, xl[sl][dx][0], xl[sl][dx][1]);
                        zLerps_[sl * 32 + 16 + dx] = lerp(fz, xl[sl][dx][2], xl[sl][dx][3]);
                    }
                const float* sz = signSlot_ >= 0 && blockZ_ ? zLerps_ + signSlot_ * 32 : nullptr;
                for (int dy = 0; dy < ch; dy++) {
                    uint8_t* row = base + dy * 256 + dz * 16;
                    for (int dx = 0; dx < cw; dx++) {
                        stamp_ = ++stampCounter_;
                        bfx_ = fxs[dx];
                        bfy_ = fys[dy];
                        bfz_ = fz;
                        bdx_ = dx;
                        if (sz) {   // air where the sign slot is not above 0
                            float v = lerp(fys[dy], sz[dx], sz[16 + dx]);
                            if (v <= 0) { row[dx] = 0; blocksSignAir++; continue; }
                            interpNow_[signSlot_] = v;
                            interpStamp_[signSlot_] = stamp_;
                        }
                        int bx = cellX0_ + cxi * cw + dx, by = DF_SHAPE.minY + cy * ch + dy, bz = cellZ0_ + cz * cw + dz;
                        blocksEvaluated++;
                        row[dx] = (generated_ ? DF_BLOCK_DENSITY(*this, bx, by, bz) : eval(root, bx, by, bz)) > 0;
                    }
                }
            }
        }
    }
    blockCorners_ = nullptr;
    blockZ_ = nullptr;
    uint64_t t2 = plat::micros();
    signUs = (uint32_t)(ts - tp);
    fullUs = (uint32_t)(t1 - ts);
    cornerUs = (uint32_t)(t1 - t0);
    boundUs = (uint32_t)(tb - t0);
    blockUs = (uint32_t)(t2 - t1);
    return true;
}

}  // namespace vanilla
}  // namespace mc
