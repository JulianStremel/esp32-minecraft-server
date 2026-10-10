#include "mc/world/vanilla/carvers.h"
#include <math.h>
#include <stdlib.h>
#include "mc/platform.h"
#include "mc/registry.h"
#include "mc/world/vanilla/aquifer.h"
#include "mc/world/vanilla/density.h"
#include "mc/world/vanilla/fastmath.h"
#include "mc/world/vanilla/surface.h"

namespace mc {
namespace vanilla {

CarverSteps carverSteps;

namespace {
constexpr float PI_F = 3.1415927f;
constexpr int RANGE = (4 * 2 - 1) * 16;   // WorldCarver#getRange 4: a tunnel's steps
constexpr int SEA = 63, LAVA_LEVEL = -54;

// this generator's random numbers (vanilla: a Java LCG per start): xoshiro128** in 32-bit
// arithmetic (the ESP32's multiplications are 32-bit), floats throughout
struct Rand {
    uint32_t s[4];
    explicit Rand(uint64_t seed) {
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
    uint64_t next() { uint64_t hi = next32(); return hi << 32 | next32(); }
    int nextInt(int n) { return n <= 0 ? 0 : (int)(((uint64_t)next32() * (uint32_t)n) >> 32); }
    float nextFloat() { return (float)(next32() >> 8) * (1.0f / 16777216.0f); }
    int between(int lo, int hi) { return lo + nextInt(hi - lo + 1); }   // inclusive
    float uniform(float lo, float hi) { return lo + nextFloat() * (hi - lo); }
    // (a - b) * c with the three drawn in this order (C++ leaves the order of operands open)
    float wobble() {
        float a = nextFloat(), b = nextFloat(), c = nextFloat();
        return (a - b) * c;
    }
};

struct Carver {
    uint64_t seed;
    int cx, cz, minY, height;
    uint8_t* mask;
    AquiferHandle* aq;
    int grassCode = -1, dirtCode = -1, water, lava, air;
    CarverSteps st = {};
    const float* widths = nullptr;   // a canyon's width factors per y
    bool fast = false;               // generator version 8 on: polynomial sin and cos
    float sn(float v) const { return fast ? fastSin(v) : sinf(v); }
    float cs(float v) const { return fast ? fastCos(v) : cosf(v); }

    // the overworld carver's replaceables: stone, dirt, sand, terracotta, ores, snow, gravel, ...
    bool replaceable(uint8_t code) const {
        if (code == 0) return false;
        if (code == 1) return true;
        uint16_t s = maskState(code);
        return s != 0 && s != bs::Water && s != bs::Lava && s != bs::Ice;
    }
    // WorldCarver#carveBlock
    void block(int lx, int y, int lz, bool& surface) {
        uint8_t& code = mask[(size_t)(y - minY) * 256 + lz * 16 + lx];
        if (!replaceable(code)) return;
        uint16_t was = code == 1 ? bs::Stone : maskState(code);
        if (was == bs::GrassBlock || was == bs::Mycelium) surface = true;
        int sub = y <= minY + 8 ? (int)bs::Lava : aquiferCarve(aq, cx, cz, lx, y, lz);
        if (sub == 1) return;   // a barrier: the aquifer keeps the block
        uint16_t global = y < LAVA_LEVEL ? bs::Lava : y < SEA ? bs::Water : 0;
        code = (uint16_t)sub == global ? 0 : (uint8_t)(sub == bs::Water ? water : sub == bs::Lava ? lava : air);
        st.carved++;
        // cut through grass: the dirt under it becomes the surface
        if (surface && y - 1 > minY && grassCode >= 0) {
            uint8_t& below = mask[(size_t)(y - 1 - minY) * 256 + lz * 16 + lx];
            if (below >= 2 && (int)below == dirtCode) below = (uint8_t)grassCode;
        }
    }
    // WorldCarver#carveEllipsoid
    template <class Skip>
    void ellipsoid(float x, float y, float z, float hr, float vr, Skip skip) {
        float midX = cx * 16 + 8.0f, midZ = cz * 16 + 8.0f, reach = 16 + hr * 2;
        if (fabsf(x - midX) > reach || fabsf(z - midZ) > reach) return;
        st.ellipsoids++;
        int minX = cx * 16, minZ = cz * 16;
        int x0 = (int)floorf(x - hr) - minX - 1, x1 = (int)floorf(x + hr) - minX;
        int y0 = (int)floorf(y - vr) - 1, y1 = (int)floorf(y + vr) + 1;
        int z0 = (int)floorf(z - hr) - minZ - 1, z1 = (int)floorf(z + hr) - minZ;
        if (x0 < 0) x0 = 0;
        if (x1 > 15) x1 = 15;
        if (z0 < 0) z0 = 0;
        if (z1 > 15) z1 = 15;
        if (y0 < minY + 1) y0 = minY + 1;
        if (y1 > minY + height - 1 - 7) y1 = minY + height - 1 - 7;
        const float ih = 1.0f / hr, iv = 1.0f / vr;
        for (int lx = x0; lx <= x1; lx++) {
            float rx = ((float)(minX + lx) + 0.5f - x) * ih;
            for (int lz = z0; lz <= z1; lz++) {
                float rz = ((float)(minZ + lz) + 0.5f - z) * ih;
                if (rx * rx + rz * rz >= 1.0f) continue;
                bool surface = false;
                for (int yy = y1; yy > y0; yy--) {
                    st.cells++;
                    float ry = ((float)yy - 0.5f - y) * iv;
                    if (!skip(rx, ry, rz, yy)) block(lx, yy, lz, surface);
                }
            }
        }
    }
    // WorldCarver#canReach: can the rest of the tunnel still come to this chunk?
    bool canReach(float x, float z, int step, int steps, float thickness) const {
        float f = x - (cx * 16 + 8.0f), g = z - (cz * 16 + 8.0f), h = (float)(steps - step), i = thickness + 2.0f + 16.0f;
        return f * f + g * g - h * h <= i * i;
    }

    // CaveWorldCarver#createTunnel
    void tunnel(uint64_t s, float x, float y, float z, float hMul, float vMul, float thickness, float yaw, float pitch,
                int step, int steps, float yScale, float floorLevel) {
        Rand rs(s);
        int split = rs.nextInt(steps / 2) + steps / 4;
        bool steep = rs.nextInt(6) == 0;
        float f = 0, g = 0;
        auto skip = [floorLevel](float rx, float ry, float rz, int) { return ry <= floorLevel || rx * rx + ry * ry + rz * rz >= 1.0f; };
        for (int j = step; j < steps; j++) {
            st.steps++;
            float d = 1.5f + sn(PI_F * (float)j / (float)steps) * thickness;
            float e = d * yScale;
            float h = cs(pitch);
            x += cs(yaw) * h;
            y += sn(pitch);
            z += sn(yaw) * h;
            pitch *= steep ? 0.92f : 0.7f;
            pitch += g * 0.1f;
            yaw += f * 0.1f;
            g *= 0.9f;
            f *= 0.75f;
            g += rs.wobble() * 2.0f;
            f += rs.wobble() * 4.0f;
            if (j == split && thickness > 1.0f) {
                uint64_t s1 = rs.next();
                float t1 = rs.nextFloat() * 0.5f + 0.5f;
                tunnel(s1, x, y, z, hMul, vMul, t1, yaw - PI_F / 2, pitch / 3.0f, j, steps, 1.0f, floorLevel);
                uint64_t s2 = rs.next();
                float t2 = rs.nextFloat() * 0.5f + 0.5f;
                tunnel(s2, x, y, z, hMul, vMul, t2, yaw + PI_F / 2, pitch / 3.0f, j, steps, 1.0f, floorLevel);
                return;
            }
            if (rs.nextInt(4) != 0) {
                if (!canReach(x, z, j, steps, thickness)) return;
                ellipsoid(x, y, z, d * hMul, e * vMul, skip);
            }
        }
    }
    // CaveWorldCarver#carve
    void cave(Rand& rnd, int sx, int sz, int yMin, int yMax) {
        int count = rnd.nextInt(rnd.nextInt(rnd.nextInt(15) + 1) + 1);
        for (int k = 0; k < count; k++) {
            float x = (float)(sx * 16 + rnd.nextInt(16));
            float y = (float)rnd.between(yMin, yMax);
            float z = (float)(sz * 16 + rnd.nextInt(16));
            float hMul = rnd.uniform(0.7f, 1.4f), vMul = rnd.uniform(0.8f, 1.3f), floorLevel = rnd.uniform(-1.0f, -0.4f);
            int tunnels = 1;
            if (rnd.nextInt(4) == 0) {   // a room
                float yScale = rnd.uniform(0.1f, 0.9f);
                float radius = 1.0f + rnd.nextFloat() * 6.0f;
                float hr = 1.5f + radius;   // 1.5 + sin(pi / 2) * radius
                ellipsoid(x + 1.0f, y, z, hr, hr * yScale,
                          [floorLevel](float rx, float ry, float rz, int) { return ry <= floorLevel || rx * rx + ry * ry + rz * rz >= 1.0f; });
                tunnels += rnd.nextInt(4);
            }
            for (int p = 0; p < tunnels; p++) {
                float yaw = rnd.nextFloat() * (PI_F * 2);
                float pitch = (rnd.nextFloat() - 0.5f) / 4.0f;
                float thickness = rnd.nextFloat() * 2.0f;
                thickness += rnd.nextFloat();
                if (rnd.nextInt(10) == 0) {
                    float a = rnd.nextFloat(), b = rnd.nextFloat();
                    thickness *= a * b * 3.0f + 1.0f;
                }
                int steps = RANGE - rnd.nextInt(RANGE / 4);
                uint64_t s = rnd.next();
                tunnel(s, x, y, z, hMul, vMul, thickness, yaw, pitch, 0, steps, 1.0f, floorLevel);
            }
        }
    }
    // CanyonWorldCarver#carve and #doCarve
    void canyon(Rand& rnd, int sx, int sz, float* widthBuf) {
        float x = (float)(sx * 16 + rnd.nextInt(16));
        float y = (float)rnd.between(10, 67);
        float z = (float)(sz * 16 + rnd.nextInt(16));
        float yaw = rnd.nextFloat() * (PI_F * 2);
        float pitch = rnd.uniform(-0.125f, 0.125f);
        const float yScale = 3.0f;
        // TrapezoidFloat(0, 6, plateau 2)
        float span = 6.0f, side = (span - 2.0f) / 2.0f, top = span - side;
        float thickness = rnd.nextFloat() * top;
        thickness += rnd.nextFloat() * side;
        int steps = (int)((float)RANGE * rnd.uniform(0.75f, 1.0f));
        Rand rs(rnd.next());
        float w = 1.0f;   // initWidthFactors
        for (int j = 0; j < height; j++) {
            if (j == 0 || rs.nextInt(3) == 0) {
                float a = rs.nextFloat(), b = rs.nextFloat();
                w = 1.0f + a * b;
            }
            widthBuf[j] = w * w;
        }
        widths = widthBuf;
        const int base = minY;
        auto skip = [this, base](float rx, float ry, float rz, int yy) {
            int i = yy - base;
            return (rx * rx + rz * rz) * widths[i - 1] + ry * ry / 6.0f >= 1.0f;
        };
        float f = 0, g = 0;
        for (int i = 0; i < steps; i++) {
            st.steps++;
            float d = 1.5f + sn((float)i * PI_F / (float)steps) * thickness;
            float e = d * yScale;
            d *= rs.uniform(0.75f, 1.0f);
            float centre = 1.0f - fabsf(0.5f - (float)i / (float)steps) * 2.0f;
            (void)centre;   // vertical_radius_center_factor 0
            e = 1.0f * e * rs.uniform(0.75f, 1.0f);
            float h = cs(pitch), j = sn(pitch);
            x += cs(yaw) * h;
            y += j;
            z += sn(yaw) * h;
            pitch *= 0.7f;
            pitch += g * 0.05f;
            yaw += f * 0.05f;
            g *= 0.8f;
            f *= 0.5f;
            g += rs.wobble() * 2.0f;
            f += rs.wobble() * 4.0f;
            if (rs.nextInt(4) != 0) {
                if (!canReach(x, z, i, steps, thickness)) return;
                ellipsoid(x, y, z, d, e, skip);
            }
        }
    }
};

uint64_t startSeed(uint64_t seed, int carver, int sx, int sz) {
    uint64_t z = seed ^ ((uint64_t)(uint32_t)sx * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)(uint32_t)sz * 0xC2B2AE3D27D4EB4Full) ^
                 ((uint64_t)(carver + 1) * 0x165667B19E3779F9ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
}  // namespace

void carveChunk(uint64_t seed, int cx, int cz, uint8_t* mask, AquiferHandle* aquifer, bool fastTrig) {
    uint64_t t0 = plat::micros();
    Carver* c = new Carver();   // not on the stack
    float* widthBuf = (float*)malloc(sizeof(float) * DF_SHAPE.height);
    if (!c || !widthBuf) {
        delete c;
        free(widthBuf);
        return;
    }
    c->seed = seed;
    c->fast = fastTrig;
    c->cx = cx;
    c->cz = cz;
    c->minY = DF_SHAPE.minY;
    c->height = DF_SHAPE.height;
    c->mask = mask;
    c->aq = aquifer;
    c->water = maskExtraCode(AQ_WATER);
    c->lava = maskExtraCode(AQ_LAVA);
    c->air = maskExtraCode(AQ_AIR);
    for (int i = 0; i < SR_NUM_STATES; i++) {
        if (SR_STATES[i] == bs::GrassBlock) c->grassCode = 2 + i;
        if (SR_STATES[i] == bs::Dirt) c->dirtCode = 2 + i;
    }
    // the carvers of every overworld biome: cave (0.15 of the chunks), cave_extra_underground
    // (0.07), canyon (0.01); from chunks up to 8 away
    const float probability[3] = {0.15f, 0.07f, 0.01f};
    for (int sx = cx - 8; sx <= cx + 8; sx++)
        for (int sz = cz - 8; sz <= cz + 8; sz++)
            for (int k = 0; k < 3; k++) {
                Rand rnd(startSeed(seed, k, sx, sz));
                if (rnd.nextFloat() > probability[k]) continue;
                c->st.starts++;
                if (k == 0) c->cave(rnd, sx, sz, DF_SHAPE.minY + 8, 180);
                else if (k == 1) c->cave(rnd, sx, sz, DF_SHAPE.minY + 8, 47);
                else c->canyon(rnd, sx, sz, widthBuf);
            }
    c->st.us = (uint32_t)(plat::micros() - t0);
    carverSteps = c->st;
    free(widthBuf);
    delete c;
}

}  // namespace vanilla
}  // namespace mc
