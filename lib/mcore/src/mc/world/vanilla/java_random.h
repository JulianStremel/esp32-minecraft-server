// Java's java.util.Random (vanilla's LegacyRandomSource) and WorldgenRandom's seeding:
// where vanilla's structures stand is decided by these, so a seed's candidate places for
// structures are vanilla's (whether one is built there depends on this generator's biomes
// and terrain).
#pragma once
#include <stdint.h>

namespace mc {
namespace vanilla {

struct JavaRandom {
    uint64_t seed = 0;
    JavaRandom() = default;
    explicit JavaRandom(int64_t s) { setSeed(s); }
    void setSeed(int64_t s) { seed = ((uint64_t)s ^ 0x5DEECE66DULL) & ((1ULL << 48) - 1); }
    int32_t next(int bits) {
        seed = (seed * 0x5DEECE66DULL + 0xBULL) & ((1ULL << 48) - 1);
        return (int32_t)(uint32_t)(seed >> (48 - bits));
    }
    int32_t nextInt() { return next(32); }
    int32_t nextInt(int32_t bound) {
        if (bound <= 0) return 0;
        if ((bound & -bound) == bound) return (int32_t)(((int64_t)bound * (int64_t)next(31)) >> 31);
        int32_t bits, val;
        do {
            bits = next(31);
            val = bits % bound;
        } while (bits - val + (bound - 1) < 0);
        return val;
    }
    int32_t nextInt(int32_t lo, int32_t hi) { return lo >= hi ? lo : lo + nextInt(hi - lo); }   // [lo, hi)
    int64_t nextLong() {
        int64_t hi = next(32), lo = next(32);
        return (int64_t)((uint64_t)hi << 32) + lo;
    }
    bool nextBoolean() { return next(1) != 0; }
    float nextFloat() { return (float)next(24) * (1.0f / 16777216.0f); }
    double nextDouble() { return (double)(((int64_t)next(26) << 27) + next(27)) * (1.0 / 9007199254740992.0); }
    // WorldgenRandom
    void setLargeFeatureSeed(int64_t world, int32_t x, int32_t z) {
        setSeed(world);
        int64_t a = nextLong(), b = nextLong();
        setSeed((int64_t)((uint64_t)(int64_t)x * (uint64_t)a ^ (uint64_t)(int64_t)z * (uint64_t)b ^ (uint64_t)world));
    }
    void setLargeFeatureWithSalt(int64_t world, int32_t x, int32_t z, int32_t salt) {
        setSeed((int64_t)((uint64_t)(int64_t)x * 341873128712ULL + (uint64_t)(int64_t)z * 132897987541ULL + (uint64_t)world +
                          (uint64_t)(int64_t)salt));
    }
    JavaRandom fork() { return JavaRandom(nextLong()); }
};

}  // namespace vanilla
}  // namespace mc
