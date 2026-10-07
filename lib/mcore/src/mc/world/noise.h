// Seeded gradient noise and small deterministic hash/RNG helpers (float only:
// the ESP32 FPU is single precision, doubles are emulated and slow).
#pragma once
#include <math.h>
#include <stdint.h>

namespace mc {

inline uint64_t mix64(uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
inline uint64_t hash3(uint64_t seed, int32_t a, int32_t b, int32_t c = 0) {
    return mix64(seed ^ mix64((uint64_t)(uint32_t)a | ((uint64_t)(uint32_t)b << 32)) ^ mix64((uint64_t)(uint32_t)c * 0x632BE59BD9B4E019ull));
}

// xorshift-style RNG, cheap and deterministic.
class Rng {
public:
    explicit Rng(uint64_t seed = 1) : s_(mix64(seed) | 1) {}
    uint64_t next() {
        s_ ^= s_ << 13; s_ ^= s_ >> 7; s_ ^= s_ << 17;
        return s_;
    }
    uint32_t u32() { return (uint32_t)(next() >> 32); }
    int range(int n) { return n <= 0 ? 0 : (int)(u32() % (uint32_t)n); }       // [0, n)
    int between(int lo, int hi) { return lo + range(hi - lo + 1); }              // [lo, hi]
    float unit() { return (u32() >> 8) * (1.0f / 16777216.0f); }                 // [0,1)
    bool chance(float p) { return unit() < p; }

private:
    uint64_t s_;
};

class Noise {
public:
    void init(uint64_t seed);
    float noise2(float x, float y) const;               // ~[-1, 1]
    float noise3(float x, float y, float z) const;      // ~[-1, 1]
    float fbm2(float x, float y, int octaves, float persistence = 0.5f, float lacunarity = 2.0f) const;
    float ridged2(float x, float y, int octaves) const; // [0,1], ridges where noise crosses 0

private:
    uint8_t perm_[512];
};

}  // namespace mc
