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
inline float clampedMap(float v, float a, float b, float c, float d) { return clampedLerp(c, d, fdiv(v - a, b - a)); }

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

int Router::noiseIndex(const char* name) const {
    for (int i = 0; i < d_->numNoises; i++)
        if (!strcmp(d_->noiseNames[i], name)) return i;
    return -1;
}

namespace {
// 2D simplex noise (SimplexNoise#getValue) over a permutation
float simplex2(const uint8_t* perm, float x, float y) {
    static const int8_t G[12][2] = {{1, 1}, {-1, 1}, {1, -1}, {-1, -1}, {1, 0}, {-1, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}, {0, 1}, {0, -1}};
    const float F2 = 0.36602540378f, G2 = 0.21132486540f;
    float s = (x + y) * F2;
    int i = fastFloor(x + s), j = fastFloor(y + s);
    float t = (float)(i + j) * G2;
    float x0 = x - ((float)i - t), y0 = y - ((float)j - t);
    int i1 = x0 > y0 ? 1 : 0, j1 = 1 - i1;
    float x1 = x0 - i1 + G2, y1 = y0 - j1 + G2, x2 = x0 - 1 + 2 * G2, y2 = y0 - 1 + 2 * G2;
    int ii = i & 255, jj = j & 255;
    auto corner = [&](int g, float cx, float cy) {
        float tt = 0.5f - cx * cx - cy * cy;
        if (tt < 0) return 0.0f;
        tt *= tt;
        return tt * tt * (G[g % 12][0] * cx + G[g % 12][1] * cy);
    };
    float n = corner(perm[ii + perm[jj]], x0, y0) + corner(perm[ii + i1 + perm[jj + j1]], x1, y1) +
              corner(perm[ii + 1 + perm[jj + 1]], x2, y2);
    return 70.0f * n;
}
}  // namespace

// EndIslandDensityFunction#getHeightValue (in 8-block units) and #compute, per column
float Router::endIslands(int bx, int bz) {
    int x = bx >> 3, z = bz >> 3;   // vanilla: blockX / 8 (towards 0; the same away from the origin's row)
    int32_t key = (int32_t)(((uint32_t)x & 0xFFFF) << 16 | ((uint32_t)z & 0xFFFF));
    if (key == islandKey_) return islandValue_;
    int i = x / 2, j = z / 2, k = x % 2, l = z % 2;
    // square roots in double: correctly rounded on every platform (the S3's sqrtf is an approximation)
    float f = 100.0f - (float)sqrt((double)x * x + (double)z * z) * 8.0f;
    f = clampf(f, -100.0f, 80.0f);
    for (int m = -12; m <= 12; m++)
        for (int n = -12; n <= 12; n++) {
            int64_t o = (int64_t)i + m, p = (int64_t)j + n;   // 64-bit: far out o * o exceeds 32 bits (the ESP32's long)
            if (o * o + p * p > 4096 && simplex2(islandPerm_, (float)o, (float)p) < -0.9f) {
                float g = (float)(((o < 0 ? -o : o) * 3439 + (p < 0 ? -p : p) * 147) % 13 + 9);
                int h = k - m * 2, q = l - n * 2;   // whole numbers: the square root of an int, in double
                float r = clampf(100.0f - (float)sqrt((double)(h * h + q * q)) * g, -100.0f, 80.0f);
                if (r > f) f = r;
            }
        }
    islandKey_ = key;
    islandValue_ = (f - 8.0f) / 128.0f;
    return islandValue_;
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
        snap = (float)fastFloor(fdiv(m, yScale) + 1.0e-7f) * yScale;
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
    plat::bigFree(beardCell_);
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

bool Router::init(uint64_t seed, float octaveCut, float cellMargin, bool sharedPermutation, bool generated,
                  const RouterDef* def) {
    d_ = def ? def : &OVERWORLD_ROUTER;
    {   // end_islands: a simplex permutation of its own
        uint64_t prng = seed ^ 0xE4D151A4Dull;
        for (int i = 0; i < 256; i++) islandPerm_[i] = (uint8_t)i;
        for (int i = 0; i < 256; i++) {
            int j = i + (int)(splitmix(prng) % (uint64_t)(256 - i));
            uint8_t t = islandPerm_[i];
            islandPerm_[i] = islandPerm_[j];
            islandPerm_[j] = t;
        }
        for (int i = 0; i < 256; i++) islandPerm_[256 + i] = islandPerm_[i];
    }
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
    perlin_ = (Perlin*)plat::bigAlloc(sizeof(Perlin) * 2 * d_->numNoises);
    normalFactor_ = (float*)plat::bigAlloc(sizeof(float) * d_->numNoises);
    // every octave in one block (each NormalNoise two PerlinNoises, then the blended
    // noise's 40), in internal RAM if there is room: read for every sample
    int levels = 40;
    for (int n = 0; n < d_->numNoises; n++) levels += 2 * d_->noises[n].count;
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
    memo_ = (float*)malloc(sizeof(float) * d_->numNodes);
    memoStamp_ = (uint32_t*)malloc(sizeof(uint32_t) * d_->numNodes);
    cacheKey_ = (int32_t*)malloc(sizeof(int32_t) * d_->numNodes);
    interpSlot_ = (int16_t*)malloc(sizeof(int16_t) * d_->numNodes);
    flatSlot_ = (int16_t*)malloc(sizeof(int16_t) * d_->numNodes);
    noiseBound_ = (float*)malloc(sizeof(float) * d_->numNoises);
    iv_ = (Iv*)malloc(sizeof(Iv) * d_->numNodes);
    ivStamp_ = (uint32_t*)malloc(sizeof(uint32_t) * d_->numNodes);
    if (!perlin_ || !normalFactor_ || !blendedNoise_ || !memo_ || !memoStamp_ || !cacheKey_ || !interpSlot_ ||
        !flatSlot_ || !noiseBound_ || !iv_ || !ivStamp_)
        return false;
    memset(ivStamp_, 0, sizeof(uint32_t) * d_->numNodes);
    numFlat_ = 0;
    for (int i = 0; i < d_->numNodes; i++) flatSlot_[i] = d_->nodes[i].op == DF_FLAT_CACHE ? (int16_t)numFlat_++ : -1;
    memset(memoStamp_, 0, sizeof(uint32_t) * d_->numNodes);
    for (int i = 0; i < d_->numNodes; i++) cacheKey_[i] = INT32_MIN;
    for (int n = 0; n < d_->numNoises; n++) {
        const NoiseDef& d = d_->noises[n];
        int lo = -1, hi = -1;
        for (int i = 0; i < d.count; i++)
            if (d_->amplitudes[d.start + i] != 0) {
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
            for (int i = 0; i < d.count; i++) top = fmax_(top, fabsf(d_->amplitudes[d.start + i]) / ldexpf(1.0f, i));
            for (int i = 0; i < d.count; i++)
                if (fabsf(d_->amplitudes[d.start + i]) / ldexpf(1.0f, i) < top * octaveCut) p.skip |= 1u << i;
        }
        float bound = 0;   // as noiseValue sums
        for (int k = 0; k < 2; k++) {
            const Perlin& p = perlin_[2 * n + k];
            float val = p.valFactor;
            for (int i = 0; i < p.count; i++) {
                if (!(p.skip & (1u << i))) bound += fabsf(d_->amplitudes[p.start + i]) * NOISE_MAX * val;
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
    for (int i = 0; i < d_->numNodes; i++) interpSlot_[i] = d_->nodes[i].op == DF_INTERPOLATED ? (int16_t)interpCount_++ : -1;
    cornersX_ = 16 / d_->shape.cellW + 1;
    cornersY_ = d_->shape.height / d_->shape.cellH + 1;
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
        const DfNode& f = d_->nodes[d_->roots[ROOT_FINAL_DENSITY]];
        signSlot_ = chainSlot(d_->roots[ROOT_FINAL_DENSITY]);
        signNode_ = d_->roots[ROOT_FINAL_DENSITY];
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
    if (d_->shape.cellW != 4) airBound = false;
    return cornerValues_ && cornerDensity_ && interpNow_ && cellCorners_ && cellFill_ && colFlat_ && ivCell_ && cellTop_ &&
           cornerTop_ && cellState_ && cornerFull_ && interpStamp_ && zLerps_ && xLerps_;
}

// the interpolated node n stands on, through functions that keep a value's sign (0: not
// above 0) and grow with it; -1 if not such a chain
int Router::chainSlot(int ni) const {
    const DfNode& n = d_->nodes[ni];
    switch (n.op) {
        case DF_INTERPOLATED: return interpSlot_[ni];
        case DF_SQUEEZE: case DF_HALF_NEGATIVE: case DF_QUARTER_NEGATIVE: case DF_CUBE:
        case DF_CACHE_ONCE: case DF_CACHE_ALL_IN_CELL: case DF_BLEND_DENSITY:
            return chainSlot(n.a);
        case DF_MUL:
            if (d_->nodes[n.a].op == DF_CONSTANT && d_->nodes[n.a].p[0] > 0) return chainSlot(n.b);
            if (d_->nodes[n.b].op == DF_CONSTANT && d_->nodes[n.b].p[0] > 0) return chainSlot(n.a);
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
            float amp = d_->amplitudes[p.start + i];
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
    float g = fdiv(d, xzFactor), h = fdiv(e, yFactor), i = fdiv(f, xzFactor);
    float j = yMul * smear, k = fdiv(j, yFactor);
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
    float t = (fdiv(main, 10.0f) + 1) / 2;
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
    const SplineDef& s = d_->splines[si];
    float c = eval(s.coord, x, y, z);
    const SplinePoint* pt = d_->points + s.first;
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
    float x0 = pt[i].loc, x1 = pt[i + 1].loc, t = fdiv(c - x0, x1 - x0);
    float y0 = value(i), y1 = value(i + 1);
    float p = pt[i].der * (x1 - x0) - (y1 - y0), q = -pt[i + 1].der * (x1 - x0) + (y1 - y0);
    return lerp(t, y0, y1) + t * (1 - t) * lerp(t, p, q);
}

float Router::eval(int ni, int x, int y, int z) {
    if (memoStamp_[ni] == stamp_) return memo_[ni];
    const DfNode& n = d_->nodes[ni];
    float v = 0;
    switch (n.op) {
        case DF_CONSTANT: v = n.p[0]; break;
        case DF_ADD: v = eval(n.a, x, y, z) + eval(n.b, x, y, z); break;
        case DF_MUL: {   // vanilla skips the second argument when the first is 0
            float a = eval(n.a, x, y, z);
            v = a == 0 ? 0 : a * eval(n.b, x, y, z);
            break;
        }
        case DF_MIN: v = fmin_(eval(n.a, x, y, z), eval(n.b, x, y, z)); break;
        case DF_MAX: v = fmax_(eval(n.a, x, y, z), eval(n.b, x, y, z)); break;
        case DF_ABS: v = fabsf(eval(n.a, x, y, z)); break;
        case DF_SQUARE: { float a = eval(n.a, x, y, z); v = a * a; break; }
        case DF_CUBE: { float a = eval(n.a, x, y, z); v = a * a * a; break; }
        case DF_HALF_NEGATIVE: { float a = eval(n.a, x, y, z); v = a > 0 ? a : a * 0.5f; break; }
        case DF_QUARTER_NEGATIVE: { float a = eval(n.a, x, y, z); v = a > 0 ? a : a * 0.25f; break; }
        case DF_SQUEEZE: { float c = clampf(eval(n.a, x, y, z), -1, 1); v = c / 2 - fdiv(c * c * c, 24.0f); break; }
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
            v = r * fabsf(noiseValue(n.noise, fdiv((float)x, r), fdiv((float)y, r), fdiv((float)z, r)));
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
        case DF_END_ISLANDS: v = endIslands(x, z); break;
        default: v = 0; break;   // blend_offset, beardifier (no structures yet)
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
    for (int y = d_->shape.minY + d_->shape.height; y >= d_->shape.minY; y -= d_->shape.cellH)
        if (sample(ROOT_INITIAL_DENSITY, x, y, z) > 0.390625f) {
            level = y;
            break;
        }
    if (prelim_) prelim_[h] = {key, level};
    return level;
}

float Router::densityBound(int lx, int y, int lz) {
    const int cw = d_->shape.cellW, ch = d_->shape.cellH, cells = cornersX_ - 1;
    int ly = y - d_->shape.minY;
    if (signSlot_ < 0 || ly < 0 || ly >= d_->shape.height) return 0;
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
    float v = node(d_->roots[root], x, y, z);   // the generated code if there is (the same values)
    corners_ = false;
    return v;
}

// The range of node n: at corner column ivCol_ for y in [ivY0_, ivY1_] (the 2D parts from
// the column table), or inside a cell (ivCol_ -1: the interpolated nodes from ivInterp_).
// Noises at their largest possible values: never narrower than the true range.
Router::Iv Router::interval(int ni) {
    if (ivStamp_[ni] == ivStampN_) return iv_[ni];
    const DfNode& n = d_->nodes[ni];
    Iv r = {-IV_BIG, IV_BIG};
    auto a = [&]() { return interval(n.a); };
    switch (n.op) {
        case DF_CONSTANT: r = {n.p[0], n.p[0]}; break;
        case DF_ADD: { Iv x = a(), y = interval(n.b); r = {x.lo + y.lo, x.hi + y.hi}; break; }
        case DF_MUL: {
            Iv x = a(), y = interval(n.b);
            float p0 = x.lo * y.lo, p1 = x.lo * y.hi, p2 = x.hi * y.lo, p3 = x.hi * y.hi;
            r = {fmin_(fmin_(p0, p1), fmin_(p2, p3)), fmax_(fmax_(p0, p1), fmax_(p2, p3))};
            break;
        }
        case DF_MIN: { Iv x = a(), y = interval(n.b); r = {fmin_(x.lo, y.lo), fmin_(x.hi, y.hi)}; break; }
        case DF_MAX: { Iv x = a(), y = interval(n.b); r = {fmax_(x.lo, y.lo), fmax_(x.hi, y.hi)}; break; }
        case DF_ABS: {
            Iv x = a();
            if (x.lo >= 0) r = x;
            else if (x.hi <= 0) r = {-x.hi, -x.lo};
            else r = {0, fmax_(-x.lo, x.hi)};
            break;
        }
        case DF_SQUARE: {
            Iv x = a();
            float l = x.lo * x.lo, h = x.hi * x.hi;
            r = x.lo >= 0 ? Iv{l, h} : x.hi <= 0 ? Iv{h, l} : Iv{0, fmax_(l, h)};
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
            r = {fmin_(l, h), fmax_(l, h)};
            break;
        }
        case DF_RANGE_CHOICE: {
            Iv x = a();
            if (x.lo >= n.p[0] && x.hi < n.p[1]) r = interval(n.b);
            else if (x.hi < n.p[0] || x.lo >= n.p[1]) r = interval(n.c);
            else { Iv y = interval(n.b), z = interval(n.c); r = {fmin_(y.lo, z.lo), fmax_(y.hi, z.hi)}; }
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
    ivY0_ = (float)(d_->shape.minY + cy * d_->shape.cellH);
    ivY1_ = (float)(d_->shape.minY + (cornersY_ - 1) * d_->shape.cellH);
    ivInterp_ = nullptr;
    for (int k = 0; k < 4; k++) {
        ivCol_ = (czi + (k >> 1)) * cornersX_ + cxi + (k & 1);
        ++ivStampN_;
        for (int i = 0; i < d_->numNodes; i++) {
            int sl = interpSlot_[i];
            if (sl < 0) continue;
            Iv v = interval(d_->nodes[i].a);
            ivCell_[sl].lo = fmin_(ivCell_[sl].lo, v.lo);
            ivCell_[sl].hi = fmax_(ivCell_[sl].hi, v.hi);
        }
    }
    ivCol_ = -1;
    ivInterp_ = ivCell_;
    ++ivStampN_;
    Iv d = interval(d_->roots[ROOT_FINAL_DENSITY]);
    ivInterp_ = nullptr;
    return d.hi <= 0;   // air: final_density never above 0
}

// ------------------------------------------------------------------ the Beardifier
// the smallest |v| for v in [lo, hi] (0 if the range holds 0)
static inline int nearest0(int lo, int hi) { return lo > 0 ? lo : hi < 0 ? -hi : 0; }
namespace {
// BEARD_KERNEL's exp(-(x^2 + (y + 0.5)^2 + z^2) / 16): the squared length is n + 0.25 for an
// integer n (up to 3 * 12^2 + 12), the power by multiplying: the same floats on every chip
float kernelByN[460];
bool kernelReady = false;
void initKernel() {
    if (kernelReady) return;
    const float q = 0.93941306f;     // e^(-1/16)
    float v = 0.98449644f;           // e^(-0.25/16)
    for (int n = 0; n < 460; n++) {
        kernelByN[n] = v;
        v *= q;
    }
    kernelReady = true;
}
// Mth.fastInvSqrt in floats (one Newton step)
inline float fastInvSqrt(float x) {
    float half = 0.5f * x;
    uint32_t i;
    memcpy(&i, &x, 4);
    i = 0x5f3759dfu - (i >> 1);
    memcpy(&x, &i, 4);
    return x * (1.5f - half * x * x);
}
// Beardifier#getBeardContribution
inline float beardContribution(int x, int y, int z, int yToGround) {
    if (x < -12 || x > 11 || y < -12 || y > 11 || z < -12 || z > 11) return 0;
    float d = (float)yToGround + 0.5f;
    float d1 = (float)(x * x + z * z) + d * d;
    float d2 = -d * fastInvSqrt(d1 / 2.0f) / 2.0f;
    return d2 * kernelByN[x * x + y * y + y + z * z];
}
// Beardifier#getBuryContribution: 1 at the box, nothing from 6 blocks away
inline float buryContribution(float x, float y, float z) {
    float len = (float)sqrt((double)(x * x + y * y + z * z));
    return len >= 6.0f ? 0.0f : 1.0f - len / 6.0f;
}
}  // namespace

void Router::beardReach(const BeardPiece& p, int& x0, int& y0, int& z0, int& x1, int& y1, int& z1) {
    x0 = p.x0 - 12;
    x1 = p.x1 + 12;
    z0 = p.z0 - 12;
    z1 = p.z1 + 12;
    if (p.kind == BEARD_JUNCTION) {
        x1 = p.x0 + 12;
        z1 = p.z0 + 12;
        y0 = p.y0 - 12;
        y1 = p.y0 + 12;
    } else if (p.kind == BEARD_THIN || p.kind == BEARD_BURY) {
        y0 = p.groundY - 12;
        y1 = p.groundY + 12;
    } else {
        y0 = (p.groundY < p.y0 ? p.groundY : p.y0) - 12;
        y1 = p.y1 + 12;
    }
}

// the smallest |v| for v in [lo, hi] (0 if the range holds 0)
// one piece's bound over a box whose y range lies on one side of the ground (or all of a bury)
static float beardBound(const BeardPiece& p, int x0, int y0, int z0, int x1, int y1, int z1) {
    int l, i1, ly0, ly1;
    if (p.kind == BEARD_JUNCTION) {
        l = nearest0(x0 - p.x0, x1 - p.x0);
        i1 = nearest0(z0 - p.z0, z1 - p.z0);
        ly0 = y0 - p.y0;
        ly1 = y1 - p.y0;
    } else {
        l = x1 < p.x0 ? p.x0 - x1 : x0 > p.x1 ? x0 - p.x1 : 0;
        i1 = z1 < p.z0 ? p.z0 - z1 : z0 > p.z1 ? z0 - p.z1 : 0;
        ly0 = y0 - p.groundY;
        ly1 = y1 - p.groundY;
    }
    if (p.kind == BEARD_BURY || p.kind == BEARD_ENCAPSULATE) {
        int ly = p.kind == BEARD_BURY ? nearest0(ly0, ly1) : (y1 < p.y0 ? p.y0 - y1 : y0 > p.y1 ? y0 - p.y1 : 0);
        float s = p.kind == BEARD_ENCAPSULATE ? 0.5f : 1.0f;
        float lx = l * s, ly2 = ly * 0.5f, lz = i1 * s;
        float len = sqrtf(lx * lx + ly2 * ly2 + lz * lz) - 0.01f;
        float most = len >= 6.0f ? 0.0f : len <= 0 ? 1.0f : 1.0f - len / 6.0f;
        return (p.kind == BEARD_ENCAPSULATE ? 0.8f : 1.0f) * most + 0.0001f;
    }
    if (l > 12 || i1 > 12) return 0;   // the kernel reaches -12 to 11
    if (p.kind == BEARD_BOX) {   // l1: the distance outside the piece's height from the ground up
        int a = p.groundY - y1, b = y0 - p.y1;
        int dd = a > b ? a : b;
        ly0 = ly1 = dd > 0 ? dd : 0;
    }
    // the kernel's smallest index n = x^2 + y^2 + y + z^2 over the box (y^2 + y: least at y 0 or -1)
    int yy;
    if (ly1 < -1) yy = ly1;
    else if (ly0 > 0) yy = ly0;
    else yy = 0;
    if (yy < -12 || yy > 11) return 0;
    int n = l * l + yy * yy + yy + i1 * i1;
    float w = p.kind == BEARD_JUNCTION ? 0.4f : 0.8f;
    return w * 0.7086f * kernelByN[n] + 0.0001f;   // |d2| <= 1 / sqrt(2), the fast inverse root a little over
}

void Router::beardMost(const BeardPiece& p, int x0, int y0, int z0, int x1, int y1, int z1, float& add, float& take) {
    initKernel();
    add = take = 0;
    if (p.kind == BEARD_BURY || p.kind == BEARD_ENCAPSULATE) {
        add = beardBound(p, x0, y0, z0, x1, y1, z1);
        return;
    }
    // a beard's d2 has the sign of -(y - ground + 0.5): it adds below the ground, takes from it up
    int g = p.kind == BEARD_JUNCTION ? p.y0 : p.groundY;
    if (y0 <= g - 1) add = beardBound(p, x0, y0, z0, x1, y1 < g - 1 ? y1 : g - 1, z1);
    if (y1 >= g) take = beardBound(p, x0, y0 > g ? y0 : g, z0, x1, y1, z1);
}

float Router::beard(int x, int y, int z) const {
    initKernel();
    float d = 0;
    const int n = beardSelN_ >= 0 ? beardSelN_ : beardN_;
    for (int s = 0; s < n; s++) {
        const BeardPiece& p = beard_[beardSelN_ >= 0 ? beardSel_[s] : s];
        if (p.kind == BEARD_JUNCTION) {
            int y2 = y - p.y0;
            d += beardContribution(x - p.x0, y2, z - p.z0, y2) * 0.4f;
            continue;
        }
        int l = p.x0 - x > x - p.x1 ? p.x0 - x : x - p.x1;
        if (l < 0) l = 0;
        int i1 = p.z0 - z > z - p.z1 ? p.z0 - z : z - p.z1;
        if (i1 < 0) i1 = 0;
        int j1 = p.groundY, k1 = y - j1, l1;
        switch (p.kind) {
            case BEARD_BOX: l1 = j1 - y > y - p.y1 ? j1 - y : y - p.y1; if (l1 < 0) l1 = 0; break;
            case BEARD_ENCAPSULATE: l1 = p.y0 - y > y - p.y1 ? p.y0 - y : y - p.y1; if (l1 < 0) l1 = 0; break;
            default: l1 = k1; break;
        }
        switch (p.kind) {
            case BEARD_BURY: d += buryContribution((float)l, (float)l1 / 2.0f, (float)i1); break;
            case BEARD_THIN: case BEARD_BOX: d += beardContribution(l, l1, i1, k1) * 0.8f; break;
            case BEARD_ENCAPSULATE: d += buryContribution((float)l / 2.0f, (float)l1 / 2.0f, (float)i1 / 2.0f) * 0.8f; break;
            default: break;
        }
    }
    return d;
}

bool Router::fillChunk(int cx, int cz, uint8_t* out) {
    cellX0_ = cx * 16;
    cellZ0_ = cz * 16;
    octaveSamples = 0;
    cornersEvaluated = 0;
    int cw = d_->shape.cellW, ch = d_->shape.cellH;
    const int cells = cornersX_ - 1;   // per side
    const int layers = cornersY_ - 1;
    uint64_t t0 = plat::micros();
    // 0) the 2D parts at the corner columns (kept until the next chunk: the biomes read the
    // climate there), and the cell layers that are air for certain
    colFlatReady_ = false;
    corners_ = true;
    for (int c = 0; c < cornersX_ * cornersX_; c++) {
        int x = cellX0_ + (c % cornersX_) * cw, z = cellZ0_ + (c / cornersX_) * cw;
        for (int i = 0; i < d_->numNodes; i++)
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
        for (int i = 0; i < d_->numNodes; i++)
            if (interpSlot_[i] == signSlot_) signNode = d_->nodes[i].a;
        ivInterp_ = nullptr;
        ivY1_ = (float)(d_->shape.minY + layers * ch);
        for (int c = 0; c < cornersX_ * cornersX_; c++) {
            ivCol_ = c;
            int lo = 0, hi = cornersY_;   // the lowest corner from which up the node is not above 0
            while (lo < hi) {
                int mid = (lo + hi) / 2;
                ivY0_ = (float)(d_->shape.minY + mid * ch);
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
    // the structures' beard: the cells it reaches are worked out block by block (and up to
    // its top, whatever the bound says)
    beardBlocks = 0;
    if (beardN_ > 0 && !beardCell_) beardCell_ = (float*)plat::bigAlloc(sizeof(float) * 2 * (size_t)layers * cells * cells);
    if (beardCell_) memset(beardCell_, 0, sizeof(float) * 2 * (size_t)layers * cells * cells);
    if (beardN_ > 0 && beardCell_) {
        for (int i = 0; i < beardN_; i++) {
            int x0, y0, z0, x1, y1, z1;
            beardReach(beard_[i], x0, y0, z0, x1, y1, z1);

            for (int cell = 0; cell < cells * cells; cell++) {
                int bx0 = cellX0_ + (cell % cells) * cw, bz0 = cellZ0_ + (cell / cells) * cw;
                if (bx0 + cw - 1 < x0 || bx0 > x1 || bz0 + cw - 1 < z0 || bz0 > z1) continue;
                int l0 = (y0 - d_->shape.minY) / ch, l1 = (y1 - d_->shape.minY) / ch;
                if (y0 < d_->shape.minY) l0 = 0;
                if (l1 >= layers) l1 = layers - 1;
                if (l1 < 0 || l0 >= layers) continue;
                for (int cy = l0; cy <= l1; cy++) {
                    int by0 = d_->shape.minY + cy * ch;
                    float add, take;
                    beardMost(beard_[i], bx0, by0, bz0, bx0 + cw - 1, by0 + ch - 1, bz0 + cw - 1, add, take);
                    beardCell_[2 * (cy * cells * cells + cell)] += add;
                    beardCell_[2 * (cy * cells * cells + cell) + 1] += take;
                    // above the bound only where the beard adds (taking from air leaves air)
                    if (add > 0 && cellTop_[cell] < cy + 1) cellTop_[cell] = (uint8_t)(cy + 1);
                }
            }
        }
    }
    // the beard's bounds in a cell: what it can add, and take; the shortcuts below hold with
    // that much room (0: no beard)
    auto beardAdd = [&](int cy, int cell) { return beardN_ > 0 && beardCell_ ? beardCell_[2 * (cy * cells * cells + cell)] : 0.0f; };
    auto beardTake = [&](int cy, int cell) { return beardN_ > 0 && beardCell_ ? beardCell_[2 * (cy * cells * cells + cell) + 1] : 0.0f; };
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
        int x = cellX0_ + ix * cw, z = cellZ0_ + iz * cw, y = d_->shape.minY + iy * ch;
        size_t at = cornerAt(iy, iz, ix);
        corners_ = true;
        stamp_ = ++stampCounter_;
        for (int i = 0; i < d_->numNodes; i++) {
            int sl = interpSlot_[i];
            if (sl < 0 || (sl == signSlot_) == all) continue;
            cornerValues_[sl * plane + at] = node(d_->nodes[i].a, x, y, z);
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
        cornerDensity_[at] = node(d_->roots[ROOT_FINAL_DENSITY], x, y, z);
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
                const float add = beardAdd(cy, cell);
                bool air = true;
                for (int k = 0; k < 8 && air; k++)
                    air = cornerValues_[signSlot_ * plane + cornerAt(cy + (k >> 2), czi + ((k >> 1) & 1), cxi + (k & 1))] <= -add;
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
    int root = d_->roots[ROOT_FINAL_DENSITY];
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
                const float add = beardAdd(cy, cell), take = beardTake(cy, cell);
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
                    ivY0_ = (float)(d_->shape.minY + cy * ch);
                    ivY1_ = (float)(d_->shape.minY + cy * ch + ch - 1);
                    ++ivStampN_;
                    Iv d = interval(root);
                    ivInterp_ = nullptr;
                    if (d.lo > take) fill = 1;
                    else if (d.hi <= -add) fill = 0;
                    if (fill >= 0) cellsProven++;
                }
                if (fill < 0 && cellMargin_ > 0) {
                    bool solid = true, air = true;
                    for (int k = 0; k < 8; k++) {
                        float d = cornerDensity_[cornerAt(cy + (k >> 2), cz + ((k >> 1) & 1), cxi + (k & 1))];
                        solid &= d > cellMargin_ + take;
                        air &= d < -cellMargin_ - add;
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
            const float add = beardAdd(cy, cell);
            const bool bearded = add > 0 || beardTake(cy, cell) > 0;
            if (bearded) {   // the pieces that can reach this cell
                int bx0 = cellX0_ + cxi * cw, bz0 = cellZ0_ + cz * cw, by0 = d_->shape.minY + cy * ch;
                beardSelN_ = 0;
                for (int i = 0; i < beardN_ && beardSelN_ < 256; i++) {
                    int x0, y0, z0, x1, y1, z1;
                    beardReach(beard_[i], x0, y0, z0, x1, y1, z1);
                    if (bx0 + cw - 1 < x0 || bx0 > x1 || bz0 + cw - 1 < z0 || bz0 > z1 || by0 + ch - 1 < y0 || by0 > y1) continue;
                    beardSel_[beardSelN_++] = (int16_t)i;
                }
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
                            if (v <= -add) { row[dx] = 0; blocksSignAir++; continue; }
                            interpNow_[signSlot_] = v;
                            interpStamp_[signSlot_] = stamp_;
                        }
                        int bx = cellX0_ + cxi * cw + dx, by = d_->shape.minY + cy * ch + dy, bz = cellZ0_ + cz * cw + dz;
                        blocksEvaluated++;
                        float dens = generated_ ? d_->block(*this, bx, by, bz) : eval(root, bx, by, bz);
                        if (bearded) {
                            dens += beard(bx, by, bz);
                            beardBlocks++;
                        }
                        row[dx] = dens > 0;
                    }
                }
            }
        }
    }
    blockCorners_ = nullptr;
    blockZ_ = nullptr;
    beardSelN_ = -1;
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
