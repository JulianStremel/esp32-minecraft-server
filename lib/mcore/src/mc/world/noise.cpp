#include "mc/world/noise.h"

namespace mc {

void Noise::init(uint64_t seed) {
    uint8_t p[256];
    for (int i = 0; i < 256; i++) p[i] = (uint8_t)i;
    Rng r(seed);
    for (int i = 255; i > 0; i--) {
        int j = r.range(i + 1);
        uint8_t t = p[i]; p[i] = p[j]; p[j] = t;
    }
    for (int i = 0; i < 512; i++) perm_[i] = p[i & 255];
}

static inline float fade(float t) { return t * t * t * (t * (t * 6 - 15) + 10); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

static inline float grad2(int h, float x, float y) {
    switch (h & 7) {
        case 0: return x + y;
        case 1: return x - y;
        case 2: return -x + y;
        case 3: return -x - y;
        case 4: return x;
        case 5: return -x;
        case 6: return y;
        default: return -y;
    }
}

static inline float grad3(int h, float x, float y, float z) {
    switch (h & 15) {
        case 0: return x + y;   case 1: return -x + y;  case 2: return x - y;   case 3: return -x - y;
        case 4: return x + z;   case 5: return -x + z;  case 6: return x - z;   case 7: return -x - z;
        case 8: return y + z;   case 9: return -y + z;  case 10: return y - z;  case 11: return -y - z;
        case 12: return x + y;  case 13: return -y + z; case 14: return -x + y; default: return -y - z;
    }
}

float Noise::noise2(float x, float y) const {
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float xf = x - xi, yf = y - yi;
    xi &= 255; yi &= 255;
    float u = fade(xf), v = fade(yf);
    int aa = perm_[perm_[xi] + yi], ab = perm_[perm_[xi] + yi + 1];
    int ba = perm_[perm_[xi + 1] + yi], bb = perm_[perm_[xi + 1] + yi + 1];
    float x1 = lerpf(grad2(aa, xf, yf), grad2(ba, xf - 1, yf), u);
    float x2 = lerpf(grad2(ab, xf, yf - 1), grad2(bb, xf - 1, yf - 1), u);
    return lerpf(x1, x2, v) * 1.41421356f * 0.7071f * 1.4f;  // approx normalise to [-1,1]
}

float Noise::noise3(float x, float y, float z) const {
    int xi = (int)floorf(x), yi = (int)floorf(y), zi = (int)floorf(z);
    float xf = x - xi, yf = y - yi, zf = z - zi;
    xi &= 255; yi &= 255; zi &= 255;
    float u = fade(xf), v = fade(yf), w = fade(zf);
    int a = perm_[xi] + yi, aa = perm_[a] + zi, ab = perm_[a + 1] + zi;
    int b = perm_[xi + 1] + yi, ba = perm_[b] + zi, bb = perm_[b + 1] + zi;
    float r = lerpf(
        lerpf(lerpf(grad3(perm_[aa], xf, yf, zf), grad3(perm_[ba], xf - 1, yf, zf), u),
              lerpf(grad3(perm_[ab], xf, yf - 1, zf), grad3(perm_[bb], xf - 1, yf - 1, zf), u), v),
        lerpf(lerpf(grad3(perm_[aa + 1], xf, yf, zf - 1), grad3(perm_[ba + 1], xf - 1, yf, zf - 1), u),
              lerpf(grad3(perm_[ab + 1], xf, yf - 1, zf - 1), grad3(perm_[bb + 1], xf - 1, yf - 1, zf - 1), u), v),
        w);
    return r;
}

// ------------------------------------------------------------------ version 2
void LatticeNoise::init(uint64_t seed) {
    uint64_t s = mix64(seed);
    seed_ = (uint32_t)(s >> 32);
    for (int o = 0; o < MAX_OCTAVES; o++)
        for (int a = 0; a < 3; a++) shift_[o][a] = (uint32_t)(mix64(s + (uint64_t)(o * 3 + a + 1)) >> 32);
}

static const uint32_t PRIME_X = 501125321u, PRIME_Y = 1136930381u, PRIME_Z = 1720413743u;

static inline uint32_t saltSeed(uint32_t seed, uint32_t salt) { return seed ^ (salt * 0x9E3779B9u); }
static inline uint32_t cornerHash(uint32_t s, uint32_t xp, uint32_t yp, uint32_t zp) {
    return (s ^ xp ^ yp ^ zp) * 0x27D4EB2Du;
}

float LatticeNoise::noise2(LatticePos x, LatticePos z, uint32_t salt) const {
    float xf = x.frac, zf = z.frac;
    float u = fade(xf), v = fade(zf);
    uint32_t s = saltSeed(seed_, salt);
    uint32_t x0 = (uint32_t)x.cell * PRIME_X, x1 = x0 + PRIME_X;
    uint32_t z0 = (uint32_t)z.cell * PRIME_Z, z1 = z0 + PRIME_Z;
    // gradient: the top 3 bits of the hash
    float a = lerpf(grad2((int)(cornerHash(s, x0, 0, z0) >> 29), xf, zf),
                    grad2((int)(cornerHash(s, x1, 0, z0) >> 29), xf - 1, zf), u);
    float b = lerpf(grad2((int)(cornerHash(s, x0, 0, z1) >> 29), xf, zf - 1),
                    grad2((int)(cornerHash(s, x1, 0, z1) >> 29), xf - 1, zf - 1), u);
    return lerpf(a, b, v) * 1.41421356f * 0.7071f * 1.4f;  // same scale as version 1
}

float LatticeNoise::noise3(LatticePos x, LatticePos y, LatticePos z, uint32_t salt) const {
    float xf = x.frac, yf = y.frac, zf = z.frac;
    float u = fade(xf), v = fade(yf), w = fade(zf);
    uint32_t s = saltSeed(seed_, salt);
    uint32_t xp[2] = {(uint32_t)x.cell * PRIME_X, (uint32_t)x.cell * PRIME_X + PRIME_X};
    uint32_t yp[2] = {(uint32_t)y.cell * PRIME_Y, (uint32_t)y.cell * PRIME_Y + PRIME_Y};
    uint32_t zp[2] = {(uint32_t)z.cell * PRIME_Z, (uint32_t)z.cell * PRIME_Z + PRIME_Z};
    auto g = [&](int dx, int dy, int dz) {   // gradient: the top 4 bits of the hash
        return grad3((int)(cornerHash(s, xp[dx], yp[dy], zp[dz]) >> 28), xf - dx, yf - dy, zf - dz);
    };
    return lerpf(lerpf(lerpf(g(0, 0, 0), g(1, 0, 0), u), lerpf(g(0, 1, 0), g(1, 1, 0), u), v),
                 lerpf(lerpf(g(0, 0, 1), g(1, 0, 1), u), lerpf(g(0, 1, 1), g(1, 1, 1), u), v), w);
}

float LatticeNoise::noise3(int32_t x, int32_t y, int32_t z, const Freq& fxz, const Freq& fy) const {
    return noise3(place(LatticeCursor(x, fxz), 0, 0), place(LatticeCursor(y, fy), 0, 1),
                  place(LatticeCursor(z, fxz), 0, 2));
}

float LatticeNoise::fbm2(int32_t x, int32_t z, const Freq& f, int octaves, float persistence) const {
    float sum = 0, amp = 1, norm = 0;
    LatticeCursor cx(x, f), cz(z, f);
    if (octaves > MAX_OCTAVES) octaves = MAX_OCTAVES;
    for (int i = 0; i < octaves; i++) {
        sum += noise2(place(cx, i, 0), place(cz, i, 2), (uint32_t)i) * amp;
        norm += amp;
        amp *= persistence;
        cx.nextOctave();
        cz.nextOctave();
    }
    return sum / norm;
}

float LatticeNoise::ridged2(int32_t x, int32_t z, const Freq& f, int octaves) const {
    float sum = 0, amp = 1, norm = 0;
    LatticeCursor cx(x, f), cz(z, f);
    if (octaves > MAX_OCTAVES) octaves = MAX_OCTAVES;
    for (int i = 0; i < octaves; i++) {
        float n = 1.0f - fabsf(noise2(place(cx, i, 0), place(cz, i, 2), (uint32_t)i));
        sum += n * n * amp;
        norm += amp;
        amp *= 0.5f;
        cx.nextOctave();
        cz.nextOctave();
    }
    return sum / norm;
}

// ------------------------------------------------------------------ version 1
float Noise::fbm2(float x, float y, int octaves, float persistence, float lacunarity) const {
    float sum = 0, amp = 1, freq = 1, norm = 0;
    for (int i = 0; i < octaves; i++) {
        sum += noise2(x * freq, y * freq) * amp;
        norm += amp;
        amp *= persistence;
        freq *= lacunarity;
    }
    return sum / norm;
}

float Noise::ridged2(float x, float y, int octaves) const {
    float sum = 0, amp = 1, freq = 1, norm = 0;
    for (int i = 0; i < octaves; i++) {
        float n = 1.0f - fabsf(noise2(x * freq, y * freq));
        sum += n * n * amp;
        norm += amp;
        amp *= 0.5f;
        freq *= 2.0f;
    }
    return sum / norm;
}

__attribute__((noinline)) float noiseMulAdd(float a, float b, float c) { return a * b + c; }

}  // namespace mc
