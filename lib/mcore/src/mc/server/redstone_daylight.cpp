#include "mc/server/redstone.h"
#include "mc/server/server.h"
#include "mc/world/light.h"
#include "mc/registry.h"
#include <algorithm>
#include <math.h>
#include <string.h>

namespace mc {
namespace {
bool sampleSky(Server& s, Chunk& center, TileEntity& detector, uint8_t& sky) {
    // Transparent blocks such as glass can raise the motion-blocking heightmap.
    // Testing the column avoids a regional flood fill for open-sky detectors.
    bool direct = true;
    for (int y = detector.y; y < center.height(detector.lx, detector.lz); ++y)
        if (blockOf(center.get(detector.lx, y, detector.lz)).filterLight) {
            direct = false;
            break;
        }
    if (direct) {
        sky = 15;
        return true;
    }
    // Loading a neighbour may evict another chunk. Hold every acquired chunk,
    // including the center, until the computation and cache writes are complete.
    struct Pins {
        Chunk* chunks[9] = {};
        ~Pins() {
            for (Chunk* c : chunks)
                if (c) --c->jobRefs;
        }
    } pins;
    pins.chunks[4] = &center;
    ++center.jobRefs;
    Chunk outside(0, 0);
    const Chunk* nine[9] = {};
    uint64_t stamps[9] = {};
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            int i = (dz + 1) * 3 + dx + 1;
            int cx = center.cx + dx, cz = center.cz + dz;
            if (!s.world.chunkInBounds(cx, cz)) {
                nine[i] = &outside;
                continue;
            }
            Chunk* c = i == 4 ? &center : s.world.load(center.dim, cx, cz);
            if (!c || c->readOnly) return false;
            if (i != 4) {
                pins.chunks[i] = c;
                ++c->jobRefs;
            }
            nine[i] = c;
            stamps[i] = (uint64_t)c->residency << 32 | c->skyVersion;
        }
    }
    if (!detector.daylightValid || memcmp(detector.daylightVersions, stamps, sizeof(stamps))) {
        uint64_t start = plat::micros();
        ChunkLight light;
        bool ok = light.computeRegion(nine);
        uint32_t elapsed = (uint32_t)(plat::micros() - start);
        ++s.redstone.daylightComputations;
        s.redstone.daylightComputeUs += elapsed;
        s.redstone.daylightPeakUs = std::max(s.redstone.daylightPeakUs, elapsed);
        if (!ok) return false;
        // One computation serves all detectors in this chunk. Their power/state
        // updates do not change skyVersion, so they do not invalidate each other.
        for (TileEntity* t = center.tiles(); t; t = t->next) {
            if (t->type != TILE_DAYLIGHT) continue;
            t->daylightSky = light.skyAt(t->lx, t->y, t->lz);
            memcpy(t->daylightVersions, stamps, sizeof(stamps));
            t->daylightValid = true;
        }
    }
    sky = detector.daylightSky;
    return true;
}
// Match Mth.cos's float table indexing without storing a 256 KiB sine table.
float vanillaCos(float angle) {
    int index = (int)(angle * 10430.378f + 16384.0f) & 65535;
    return (float)sin(index * (M_PI * 2.0) / 65536.0);
}
} // namespace

void Redstone::daylightDetector(Server& s, int x, int y, int z, uint16_t state) {
    if (s.curDim != DIM_OVERWORLD || blockIdOf(state) != blk::DaylightDetector) return;
    Chunk* c = s.world.get(s.curDim, x >> 4, z >> 4);
    TileEntity* t = c ? c->tileAt(x & 15, y, z & 15) : nullptr;
    if (!t || t->type != TILE_DAYLIGHT) return;
    uint8_t sky;
    if (!sampleSky(s, *c, *t, sky)) {
        ++failures;
        MC_LOGE("Daylight detector could not sample sky light");
        return;
    }
    int power = sky - skyDarkening(s.meta.timeOfDay, s.meta.raining != 0, s.meta.raining == 2);
    if (getBool(state, "inverted")) {
        power = 15 - power;
    } else if (power > 0) {
        double day = (double)(s.meta.timeOfDay % 24000) / 24000.0 - 0.25;
        day -= floor(day);
        float angle = (float)((day * 2.0 + 0.5 - cos(day * M_PI) / 2.0) / 3.0);
        angle *= (float)M_PI * 2.0f;
        float target = angle < (float)M_PI ? 0.0f : (float)M_PI * 2.0f;
        angle += (target - angle) * .2f;
        power = (int)floorf(power * vanillaCos(angle) + .5f);
    }
    power = std::max(0, std::min(15, power));
    if (getProp(state, "power") != power) s.setBlock(x, y, z, setProp(state, "power", power));
}
} // namespace mc
