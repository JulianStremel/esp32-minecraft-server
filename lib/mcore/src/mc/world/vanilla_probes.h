// The terrain as the structures see it (structures.h's TerrainProbe), from a generator's
// router: never a generated chunk's blocks (a structure must not depend on which of its
// chunks was generated first).
#pragma once
#include <limits.h>
#include "mc/world/chunk.h"
#include "mc/world/vanilla/biomes.h"
#include "mc/world/vanilla/density.h"
#include "mc/world/vanilla/structures.h"

namespace mc {

// The overworld's: the router's final density down a column (the first free y above the
// ground or the sea) and vanilla's biome at the quart (versions 10 on).
struct RouterProbe : vanilla::TerrainProbe {
    vanilla::Router& r;
    struct Col { int x, z; int16_t ground; bool used; } cols[8] = {};
    int next = 0;
    explicit RouterProbe(vanilla::Router& router) : r(router) {}
    bool solid(int x, int y, int z) override { return r.sample(vanilla::ROOT_FINAL_DENSITY, x, y, z) > 0; }
    int ground(int x, int z) {   // the highest solid y, or minY - 1
        for (const Col& c : cols)
            if (c.used && c.x == x && c.z == z) return c.ground;
        const vanilla::RouterShape& sh = r.def().shape;
        const int top = sh.minY + sh.height - 1;
        int p = r.preliminarySurface(x, z);
        int y = p == INT32_MAX ? top : p + 48;
        if (y > top) y = top;
        while (y < top && solid(x, y, z)) y = y + 8 > top ? top : y + 8;
        while (y >= sh.minY && !solid(x, y, z)) y -= 4;   // down in steps of 4
        if (y >= sh.minY)
            for (int k = 0; k < 3 && y < top && solid(x, y + 1, z); k++) y++;   // the step's top
        else y = sh.minY - 1;
        cols[next] = {x, z, (int16_t)y, true};
        next = (next + 1) & 7;
        return y;
    }
    int height(int x, int z, bool oceanFloor) override {
        int h = ground(x, z) + 1;
        return !oceanFloor && h < SEA_LEVEL ? SEA_LEVEL : h;
    }
    uint8_t biome(int x, int y, int z) override {
        using namespace vanilla;
        int qx = x & ~3, qy = y & ~3, qz = z & ~3;   // QuartPos.toBlock(QuartPos.fromBlock)
        const float climate[5] = {r.sample(ROOT_TEMPERATURE, qx, 0, qz), r.sample(ROOT_VEGETATION, qx, 0, qz),
                                  r.sample(ROOT_CONTINENTS, qx, 0, qz), r.sample(ROOT_EROSION, qx, 0, qz),
                                  r.sample(ROOT_RIDGES, qx, 0, qz)};
        ColumnClimate cc;
        columnClimate(climate, cc);
        return biomeAtDepth(cc, quantizeClimate(r.sample(ROOT_DEPTH, qx, qy, qz)));
    }
};

// The Nether's biome parameter list: the nearest of these (temperature, humidity; offset)
struct NetherPoint { float t, h, offset; uint8_t biome; };
constexpr NetherPoint NETHER_POINTS[5] = {
    {0.0f, 0.0f, 0.0f, biome::NetherWastes},   {0.0f, -0.5f, 0.0f, biome::SoulSandValley},
    {0.4f, 0.0f, 0.0f, biome::CrimsonForest},  {0.0f, 0.5f, 0.375f, biome::WarpedForest},
    {-0.5f, 0.0f, 0.175f, biome::BasaltDeltas},
};
inline uint8_t netherBiome(float t, float h) {
    float best = 1e9f;
    uint8_t out = biome::NetherWastes;
    for (const NetherPoint& p : NETHER_POINTS) {
        float d = (t - p.t) * (t - p.t) + (h - p.h) * (h - p.h) + p.offset * p.offset;
        if (d < best) {
            best = d;
            out = p.biome;
        }
    }
    return out;
}

// The Nether's: its biomes from the temperature and vegetation at the quart (the
// fortresses need no heights)
struct NetherProbe : vanilla::TerrainProbe {
    vanilla::Router& r;
    explicit NetherProbe(vanilla::Router& router) : r(router) {}
    int height(int, int, bool) override { return 64; }
    bool solid(int x, int y, int z) override { return r.sample(vanilla::ROOT_FINAL_DENSITY, x, y, z) > 0; }
    uint8_t biome(int x, int, int z) override {
        int qx = x & ~3, qz = z & ~3;
        return netherBiome(r.sample(vanilla::ROOT_TEMPERATURE, qx, 0, qz), r.sample(vanilla::ROOT_VEGETATION, qx, 0, qz));
    }
};

}  // namespace mc
