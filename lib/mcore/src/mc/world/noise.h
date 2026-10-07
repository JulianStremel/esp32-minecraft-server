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

// A noise frequency (cycles per block) as an exact fraction num / den. Generator
// version 2 computes lattice positions from integer block coordinates with integer
// arithmetic, so they are exact at any distance from the origin; version 1 multiplies
// the float coordinate by `approx` (the same frequency as a float literal).
struct Freq {
    int32_t num, den;
    float approx;
    float inv;   // 1 / den, rounded once at compile time (the same on every platform)
    constexpr Freq(int32_t n, int32_t d, float a) : num(n), den(d), approx(a), inv(1.0f / (float)d) {}
};

// A coordinate in noise lattice units: the cell index and the offset inside it.
struct LatticePos {
    int32_t cell;
    float frac;   // [0, 1)
};

// Exact lattice position of an integer coordinate: cell and remainder of v * num / den,
// stepped through the octaves by doubling (no division after the first, and that one
// fits in 32 bits: cheap on the ESP32). Range: |v| <= 30 000 000 (vanilla's border),
// num <= 71, and den >= 100 for up to 3 doublings (the generator uses den >= 200).
struct LatticeCursor {
    int32_t cell, rem, den;
    float inv;
    LatticeCursor(int32_t v, const Freq& f) : den(f.den), inv(f.inv) {
        int32_t p = v * f.num;
        cell = p / den;
        rem = p % den;
        if (rem < 0) {
            rem += den;
            cell--;
        }
    }
    LatticePos pos() const { return {cell, (float)rem * inv}; }
    void nextOctave() {   // the same coordinate at twice the frequency
        cell *= 2;
        rem *= 2;
        if (rem >= den) {
            rem -= den;
            cell++;
        }
    }
};

inline LatticePos latticePos(int32_t v, const Freq& f, int octave = 0) {
    LatticeCursor c(v, f);
    for (int i = 0; i < octave; i++) c.nextOctave();
    return c.pos();
}

// Version 2 gradient noise: gradients come from a hash of the lattice cell instead of a
// 256-entry table, so the noise does not repeat (the table version repeats every 256
// cells: 160 000 blocks for continents, under 6 000 for the detail noise), and every
// octave gets its own hash (salt), so octaves do not line up at the origin. The hash is
// FastNoise's: coordinates times large primes, xor, one multiply; the gradient comes
// from the well-mixed top bits.
class LatticeNoise {
public:
    void init(uint64_t seed) { seed_ = (uint32_t)(mix64(seed) >> 32); }
    float noise2(LatticePos x, LatticePos z, uint32_t salt = 0) const;      // ~[-1, 1]
    float noise3(LatticePos x, LatticePos y, LatticePos z, uint32_t salt = 0) const;
    float fbm2(int32_t x, int32_t z, const Freq& f, int octaves, float persistence = 0.5f) const;
    float ridged2(int32_t x, int32_t z, const Freq& f, int octaves) const;   // [0,1]

private:
    uint32_t seed_ = 0;
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
