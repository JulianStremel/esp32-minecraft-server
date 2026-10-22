// The classic generator's structures inside one chunk: dungeons and ruins (classic.h).
#include <string.h>
#include "mc/registry.h"
#include "mc/world/classic/classic.h"
#include "mc/world/generator.h"
#include "mc/world/noise.h"
#include "mc/world/vanilla/fastmath.h"
#include "mc/world/vanilla/structure_build.h"

namespace mc {
namespace classic {

namespace {

bool solid(uint16_t st) { return !stateIsAir(st) && !stateIsFluid(st); }

void chest(Chunk& c, int lx, int y, int lz, int facingDir, const char* loot, uint64_t seed) {
    static const char* const DIRS[4] = {"north", "east", "south", "west"};
    c.set(lx, y, lz, setPropStr(bs::Chest, "facing", DIRS[facingDir & 3]));
    TileEntity* t = c.addTile(TILE_CHEST, lx, y, lz);
    if (t) vanilla::sb::fillLoot(t, loot, (int64_t)hash3(seed, c.cx * 16 + lx, y, c.cz * 16 + lz));
}

void spawner(Chunk& c, int lx, int y, int lz, const char* mob) {
    c.set(lx, y, lz, bs::Spawner);
    TileEntity* t = c.addTile(TILE_SPAWNER, lx, y, lz);
    int type = findEntityType(mob);
    if (t && type >= 0) t->spawnEntity = (uint16_t)type;
}

bool isOcean(uint8_t b) {
    return b == biome::Ocean || b == biome::DeepOcean || b == biome::FrozenOcean || b == biome::ColdOcean ||
           b == biome::DeepColdOcean || b == biome::LukewarmOcean || b == biome::WarmOcean || b == biome::River ||
           b == biome::FrozenRiver || b == biome::Beach || b == biome::SnowyBeach;
}

}  // namespace

// ------------------------------------------------------------------ dungeons
void placeDungeons(Chunk& c, uint64_t seed, const ColumnInfo* cols) {
    Rng r(hash3(seed, c.cx, c.cz, 0xD06E0));
    int lowest = 1 << 20;
    for (int i = 0; i < 256; i++)
        if (cols[i].height < lowest) lowest = cols[i].height;
    const int yMin = c.minY() + 6, yMax = lowest - 12;
    if (yMax <= yMin) return;
    for (int attempt = 0; attempt < 6; attempt++) {
        int wx = r.between(2, 3), wz = r.between(2, 3);   // half the inside
        int x = r.between(wx + 1, 14 - wx), z = r.between(wz + 1, 14 - wz), y = r.between(yMin, yMax);
        const int h = 3;                                      // inside: y .. y + 3
        // vanilla's rule: solid floor and ceiling, 1 to 5 openings at the floor's level
        bool ok = true;
        int openings = 0;
        for (int dx = -wx - 1; dx <= wx + 1 && ok; dx++)
            for (int dz = -wz - 1; dz <= wz + 1 && ok; dz++)
                for (int dy = -1; dy <= h + 1 && ok; dy++) {
                    uint16_t st = c.get(x + dx, y + dy, z + dz);
                    if ((dy == -1 || dy == h + 1) && !solid(st)) ok = false;
                    bool wall = dx == -wx - 1 || dx == wx + 1 || dz == -wz - 1 || dz == wz + 1;
                    if (wall && dy == 0 && stateIsAir(st) && stateIsAir(c.get(x + dx, y + 1, z + dz))) openings++;
                }
        if (!ok || openings < 1 || openings > 5) continue;
        bool crypt = r.range(5) == 0;
        for (int dx = -wx - 1; dx <= wx + 1; dx++)
            for (int dz = -wz - 1; dz <= wz + 1; dz++)
                for (int dy = h + 1; dy >= -1; dy--) {
                    int X = x + dx, Y = y + dy, Z = z + dz;
                    bool wall = dx == -wx - 1 || dx == wx + 1 || dz == -wz - 1 || dz == wz + 1;
                    uint16_t cur = c.get(X, Y, Z);
                    if (!wall && dy >= 0 && dy <= h) {
                        c.set(X, Y, Z, bs::CaveAir);
                    } else if (dy == -1) {   // the floor
                        c.set(X, Y, Z, crypt ? (r.range(3) ? bs::StoneBricks : bs::CrackedStoneBricks)
                                             : (r.range(4) ? bs::MossyCobblestone : bs::Cobblestone));
                    } else if (solid(cur)) {   // walls and ceiling where there is rock (the openings stay)
                        c.set(X, Y, Z, crypt ? (r.range(4) ? bs::StoneBricks : bs::MossyStoneBricks) : bs::Cobblestone);
                    }
                }
        // two chests against one wall each, the spawner in the middle
        for (int k = 0; k < 2; k++)
            for (int tries = 0; tries < 3; tries++) {
                int cx = x + r.between(-wx, wx), cz = z + r.between(-wz, wz);
                int walls = 0, away = 0;
                static const int DX[4] = {0, 1, 0, -1}, DZ[4] = {-1, 0, 1, 0};
                for (int d = 0; d < 4; d++)
                    if (solid(c.get(cx + DX[d], y, cz + DZ[d]))) {
                        walls++;
                        away = (d + 2) & 3;
                    }
                if (walls != 1 || !stateIsAir(c.get(cx, y, cz))) continue;
                chest(c, cx, y, cz, away, crypt ? "stronghold_corridor" : "simple_dungeon", seed);
                break;
            }
        static const char* const MOBS[4] = {"skeleton", "zombie", "zombie", "spider"};
        spawner(c, x, y, z, crypt ? "skeleton" : MOBS[r.range(4)]);
        if (crypt)   // cobwebs in the upper corners
            for (int sx = -1; sx <= 1; sx += 2)
                for (int sz = -1; sz <= 1; sz += 2)
                    if (r.range(3)) c.set(x + sx * wx, y + h, z + sz * wz, bs::Cobweb);
        return;   // one room a chunk
    }
}

// ------------------------------------------------------------------ ruins
void placeRuins(Chunk& c, uint64_t seed, const ColumnInfo* cols) {
    Rng r(hash3(seed, c.cx, c.cz, 0x5A1B5));
    if (r.range(36) != 0) return;
    const ColumnInfo& mid = cols[8 * 16 + 8];
    if (isOcean(mid.biome) || mid.river) return;
    // the material by the biome
    uint16_t main, alt, rare, rubble;
    bool snowy = false, overgrown = false;
    switch (mid.biome) {
        case biome::Desert: case biome::Badlands:
            main = bs::Sandstone; alt = bs::CutSandstone; rare = bs::ChiseledSandstone; rubble = bs::Sandstone;
            break;
        case biome::Jungle: case biome::Swamp: case biome::DarkForest:
            main = bs::MossyStoneBricks; alt = bs::MossyCobblestone; rare = bs::CrackedStoneBricks; rubble = bs::MossyCobblestone;
            overgrown = true;
            break;
        default:
            main = bs::StoneBricks; alt = bs::MossyStoneBricks; rare = bs::CrackedStoneBricks; rubble = bs::Cobblestone;
            snowy = mid.biome == biome::SnowyPlains || mid.biome == biome::SnowyTaiga;
            break;
    }
    auto pick = [&]() { int k = r.range(10); return k < 6 ? main : k < 9 ? alt : rare; };
    int kind = r.range(3);   // 0 a tower's stump, 1 walls, 2 a ring of stones
    int R = kind == 0 ? r.between(2, 3) : r.between(3, 5);
    int x0 = 8, z0 = 8;
    // the floor: the lowest ground under the footprint
    int base = 1 << 20;
    for (int dx = -R; dx <= R; dx++)
        for (int dz = -R; dz <= R; dz++) {
            int h = cols[(z0 + dz) * 16 + x0 + dx].height;
            if (h < base) base = h;
        }
    if (base < SEA_LEVEL || !c.hasY(base + 12)) return;
    // a foundation under the footprint, down to the ground
    for (int dx = -R; dx <= R; dx++)
        for (int dz = -R; dz <= R; dz++) {
            int X = x0 + dx, Z = z0 + dz;
            for (int y = base; y > base - 6 && !solid(c.get(X, y, Z)); y--) c.set(X, y, Z, rubble);
            c.set(X, base, Z, r.range(5) ? pick() : rubble);   // the floor, some stones missing
            for (int y = base + 1; y <= cols[Z * 16 + X].height + 1; y++) c.set(X, y, Z, bs::Air);
        }
    auto column = [&](int X, int Z, int height) {
        for (int y = base + 1; y <= base + height; y++) c.set(X, y, Z, pick());
        if (snowy && r.range(2)) c.set(X, base + height + 1, Z, bs::Snow);
        if (overgrown && r.range(3) == 0) {   // vines down one side
            int vz = Z + 1;
            if (vz < 16 && stateIsAir(c.get(X, base + height, vz)))
                for (int y = base + height; y > base + 1 && stateIsAir(c.get(X, y, vz)); y--)
                    c.set(X, y, vz, setPropStr(bs::Vine, "north", "true"));
        }
    };
    if (kind == 0) {   // a round tower's stump, higher on one side
        int top = r.between(5, 10), dirX = r.range(2) ? 1 : -1;
        for (int dx = -R; dx <= R; dx++)
            for (int dz = -R; dz <= R; dz++) {
                int d2 = dx * dx + dz * dz;
                if (d2 > R * R + 1 || d2 < (R - 1) * (R - 1) + 1) continue;
                int h = top - (dirX * dx + R) * top / (2 * R + 1) - r.range(3);
                if (h > 0) column(x0 + dx, z0 + dz, h);
            }
        if (r.range(2) == 0) chest(c, x0, base - 1, z0, r.range(4), "simple_dungeon", seed);   // under the floor
    } else if (kind == 1) {   // the walls of a house: gaps, a doorway, falling away to one end
        int doorSide = r.range(4);
        for (int dx = -R; dx <= R; dx++)
            for (int dz = -R; dz <= R; dz++) {
                bool edge = dx == -R || dx == R || dz == -R || dz == R;
                if (!edge) continue;
                int side = dz == -R ? 0 : dx == R ? 1 : dz == R ? 2 : 3;
                if (side == doorSide && (dx == 0 || dz == 0)) continue;   // the doorway
                int h = 1 + r.range(4) + ((dx + R) * 2) / (R + 1);
                if (r.range(6) == 0) continue;   // a gap
                column(x0 + dx, z0 + dz, h);
            }
    } else {   // a ring of standing stones, some with a lintel
        int n = 6 + r.range(4);
        for (int i = 0; i < n; i++) {
            float a = 6.2831853f * (float)i / (float)n;
            float px = vanilla::fastCos(a) * (float)R, pz = vanilla::fastSin(a) * (float)R;
            int dx = (int)(px + (px >= 0 ? 0.5f : -0.5f)), dz = (int)(pz + (pz >= 0 ? 0.5f : -0.5f));
            int h = r.between(2, 4);
            column(x0 + dx, z0 + dz, h);
            if (r.range(3) == 0) c.set(x0 + dx, base + h + 1, z0 + dz, rare);   // a lintel stone
        }
    }
    // rubble around
    for (int i = 0; i < 6 + r.range(6); i++) {
        int X = x0 + r.between(-R - 2, R + 2), Z = z0 + r.between(-R - 2, R + 2);
        if (X < 0 || X > 15 || Z < 0 || Z > 15) continue;
        int y = cols[Z * 16 + X].height + 1;
        if (c.hasY(y) && stateIsAir(c.get(X, y, Z)) && solid(c.get(X, y - 1, Z))) c.set(X, y, Z, rubble);
    }
}

}  // namespace classic
}  // namespace mc
