// Explosions as vanilla's ServerExplosion.
//
// - Blocks: 1352 rays from the centre (the surface of a 16 x 16 x 16 grid), each with a
//   strength of power x (0.7 .. 1.3), stepping 0.3 blocks and losing 0.225 a step plus
//   (blast resistance + 0.3) x 0.3 in every block it passes (fluids: 100). The blocks a
//   ray still has strength in are destroyed, TNT among them primed with a short fuse.
// - Drops: TNT (and TNT minecarts) drop every block they destroy (tntExplosionDropDecay
//   is off); other explosions drop each with a chance of 1 in their power.
// - Fire (beds, ghast fireballs): one in three destroyed spots that are air above a solid
//   block catch fire.
// - Entities within twice the power: hurt by (impact^2 + impact) / 2 x 7 x 2 power + 1,
//   where impact is (1 - distance / range) x the share of the entity the blast can see
//   (rays from points spread over its box), and pushed away by impact. A player's push
//   travels in the explosion packet. Items are destroyed, boats and minecarts take the
//   damage as from a hit (a TNT minecart primes), primed TNT is pushed (TNT cannons).
// - Primed TNT explodes in a tick while that tick's explosions took less than
//   EXPLOSION_BUDGET_US (20 ms; the first always): the rest waits for the next tick.
//   A check before each TNT blast, not a cap: the blast that crosses it runs to the end,
//   and other explosions (TNT minecarts, creepers, beds, end crystal chains) are not
//   held back.
#include <math.h>
#include <string.h>
#include "mc/platform.h"
#include "mc/registry.h"
#include "mc/server/mob_util.h"
#include "mc/server/server.h"

namespace mc {

namespace {
// the destroyed positions: offsets from the centre's block, deduplicated in a hash set
inline int ifloor(float v) { int i = (int)v; return i - (v < (float)i); }   // floorf without libm

constexpr int SET_CAP = 4096;   // a power of two
uint32_t* g_set = nullptr;      // PSRAM (24 KB with the list), taken on the first explosion
int32_t* g_list = nullptr;      // the positions in the order found
int g_listLen = 0;
// the blocks the rays read, each read from the world once: a cube around the centre,
// filled as the rays reach it (0xFFFF: not read yet); 64 KB of PSRAM
constexpr int CACHE_CELLS = 32768;
uint16_t* g_cache = nullptr;
// beside each cached state, what a ray loses per step in it: -2 not known yet, -1 nothing
// (air), else (resistance + 0.3) x 0.3, plus 1000 for a fluid (never destroyed)
float* g_cost = nullptr;
// the 1352 ray directions, 0.3 blocks long (computed once)
constexpr int RAYS = 1352;
float* g_dirs = nullptr;

uint32_t keyOf(int dx, int dy, int dz) { return (uint32_t)(dx + 512) << 20 | (uint32_t)(dy + 512) << 10 | (uint32_t)(dz + 512); }

bool setHas(uint32_t key) {
    uint32_t h = (key * 2654435761u) & (SET_CAP - 1);
    while (g_set[h]) {
        if (g_set[h] == key + 1) return true;
        h = (h + 1) & (SET_CAP - 1);
    }
    return false;
}

// a key into the set without the list (the crater's edge); false if there already
bool setMark(uint32_t key) {
    uint32_t h = (key * 2654435761u) & (SET_CAP - 1);
    int probes = 0;
    while (g_set[h]) {
        if (g_set[h] == key + 1) return false;
        h = (h + 1) & (SET_CAP - 1);
        if (++probes > SET_CAP / 2) return false;
    }
    g_set[h] = key + 1;
    return true;
}

bool setAdd(uint32_t key) {
    uint32_t h = (key * 2654435761u) & (SET_CAP - 1);
    while (g_set[h]) {
        if (g_set[h] == key + 1) return false;
        h = (h + 1) & (SET_CAP - 1);
    }
    if (g_listLen >= SET_CAP / 2) return false;   // full enough: keep probing short
    g_set[h] = key + 1;
    g_list[g_listLen++] = (int32_t)key;
    return true;
}

float resistanceAt(uint16_t st) {
    if (stateIsAir(st)) return -1;   // nothing: no loss
    if (stateIsFluid(st)) return 100.0f;
    float r = blockOf(st).resistance;
    if (getBool(st, "waterlogged")) r = r > 100.0f ? r : 100.0f;
    return r;
}

// the block cache's place: around the centre's block (cx, cy, cz), `reach` each way
int g_cx = 0, g_cy = 0, g_cz = 0, g_reach = 0, g_side = 0;
bool g_cached = false;

uint16_t cachedState(Server& s, int bx, int by, int bz) {
    int ix = bx - g_cx + g_reach, iy = by - g_cy + g_reach, iz = bz - g_cz + g_reach;
    if (!g_cached || ix < 0 || iy < 0 || iz < 0 || ix >= g_side || iy >= g_side || iz >= g_side) {
        return dimHasY(s.curDim, by) ? s.blockAt(bx, by, bz) : 0;
    }
    uint16_t& c = g_cache[(iy * g_side + iz) * g_side + ix];
    if (c == 0xFFFF) c = dimHasY(s.curDim, by) ? s.blockAt(bx, by, bz) : 0;
    return c;
}

// does the segment p + t d (t in 0..1, block-local coordinates) cross the box (in 1/32)?
bool segmentHitsBox(float px, float py, float pz, float dx, float dy, float dz, const int8_t* b) {
    float t0 = 0, t1 = 1;
    const float lo[3] = {b[0] / 32.0f, b[1] / 32.0f, b[2] / 32.0f}, hi[3] = {b[3] / 32.0f, b[4] / 32.0f, b[5] / 32.0f};
    const float p[3] = {px, py, pz}, d[3] = {dx, dy, dz};
    for (int k = 0; k < 3; k++) {
        if (d[k] == 0) {
            if (p[k] < lo[k] || p[k] > hi[k]) return false;
            continue;
        }
        float a = (lo[k] - p[k]) / d[k], c = (hi[k] - p[k]) / d[k];
        if (a > c) { float t = a; a = c; c = t; }
        if (a > t0) t0 = a;
        if (c < t1) t1 = c;
        if (t0 > t1) return false;
    }
    return true;
}

// Is the straight line from a to b free of collision shapes (ClipContext COLLIDER)? A
// voxel walk in floats, relative to the centre's block (the ESP32-S3 has no
// double-precision FPU): one step per block passed.
bool clear(Server& s, double ax, double ay, double az, double bx, double by, double bz) {
    float x = (float)(ax - g_cx), y = (float)(ay - g_cy), z = (float)(az - g_cz);
    float dx = (float)(bx - ax), dy = (float)(by - ay), dz = (float)(bz - az);
    int ix = ifloor(x), iy = ifloor(y), iz = ifloor(z);
    int ex = ifloor((float)(bx - g_cx)), ey = ifloor((float)(by - g_cy)), ez = ifloor((float)(bz - g_cz));
    int sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1, sz = dz > 0 ? 1 : -1;
    const float BIG = 1e30f;
    float tdx = dx != 0 ? fabsf(1.0f / dx) : BIG, tdy = dy != 0 ? fabsf(1.0f / dy) : BIG, tdz = dz != 0 ? fabsf(1.0f / dz) : BIG;
    float tx = dx != 0 ? ((dx > 0 ? (ix + 1 - x) : (x - ix)) * tdx) : BIG;
    float ty = dy != 0 ? ((dy > 0 ? (iy + 1 - y) : (y - iy)) * tdy) : BIG;
    float tz = dz != 0 ? ((dz > 0 ? (iz + 1 - z) : (z - iz)) * tdz) : BIG;
    for (int guard = 0; guard < 256; guard++) {
        uint16_t st = cachedState(s, g_cx + ix, g_cy + iy, g_cz + iz);
        if (stateCollides(st)) {
            if (collisionFullBlock(st)) return false;
            const int8_t* p = COLLISION_SHAPES + COLLISION_SHAPE_OFFSETS[st];
            int n = *p++;
            for (int k = 0; k < n; k++, p += 6)
                if (segmentHitsBox(x - ix, y - iy, z - iz, dx, dy, dz, p)) return false;
        }
        if (ix == ex && iy == ey && iz == ez) return true;
        if (tx <= ty && tx <= tz) {
            if (tx > 1) return true;
            ix += sx; tx += tdx;
        } else if (ty <= tz) {
            if (ty > 1) return true;
            iy += sy; ty += tdy;
        } else {
            if (tz > 1) return true;
            iz += sz; tz += tdz;
        }
    }
    return true;
}

// Explosion#getSeenPercent: the share of points spread over the entity's box that see
// the centre
float seenPercent(Server& s, double cx, double cy, double cz, const Entity& e) {
    float w = e.width, h = e.height;
    float sx = 1.0f / (w * 2 + 1), sy = 1.0f / (h * 2 + 1), sz = 1.0f / (w * 2 + 1);
    float ox = (1.0f - floorf(1.0f / sx) * sx) / 2.0f, oz = (1.0f - floorf(1.0f / sz) * sz) / 2.0f;
    int seen = 0, total = 0;
    for (float fx = 0; fx <= 1; fx += sx)
        for (float fy = 0; fy <= 1; fy += sy)
            for (float fz = 0; fz <= 1; fz += sz) {
                double px = e.x - w / 2 + fx * w + ox, py = e.y + fy * h, pz = e.z - w / 2 + fz * w + oz;
                if (clear(s, px, py, pz, cx, cy, cz)) seen++;
                total++;
            }
    return total ? (float)seen / total : 0.0f;
}
}  // namespace

void Server::explode(double x, double y, double z, float power, int32_t source, bool fire, uint8_t kind) {
    if (power > 32) power = 32;
    uint64_t t0 = plat::micros();
    if (worldTick() != explosionTick_) {
        explosionTick_ = worldTick();
        explosionsThisTick_ = 0;
        explosionUsThisTick_ = 0;
    }
    explosionsThisTick_++;
    vibration(x, y, z, GE_EXPLODE);
    if (!g_set) {
        g_set = (uint32_t*)plat::bigAlloc(sizeof(uint32_t) * SET_CAP + sizeof(int32_t) * SET_CAP / 2);
        if (g_set) g_list = (int32_t*)(g_set + SET_CAP);
        g_cache = (uint16_t*)plat::bigAlloc(sizeof(uint16_t) * CACHE_CELLS);
        g_cost = (float*)plat::bigAlloc(sizeof(float) * CACHE_CELLS);
        g_dirs = (float*)plat::bigAlloc(sizeof(float) * 3 * RAYS);
        if (g_dirs) {
            int n = 0;
            for (int j = 0; j < 16; j++)
                for (int k = 0; k < 16; k++)
                    for (int l = 0; l < 16; l++) {
                        if (!(j == 0 || j == 15 || k == 0 || k == 15 || l == 0 || l == 15)) continue;
                        float dx = j / 15.0f * 2 - 1, dy = k / 15.0f * 2 - 1, dz = l / 15.0f * 2 - 1;
                        float d = sqrtf(dx * dx + dy * dy + dz * dz);
                        g_dirs[n * 3] = dx / d * 0.3f;
                        g_dirs[n * 3 + 1] = dy / d * 0.3f;
                        g_dirs[n * 3 + 2] = dz / d * 0.3f;
                        n++;
                    }
        }
    }
    int cx = (int)floor(x), cy = (int)floor(y), cz = (int)floor(z);
    // how far a ray can get: its strength over the loss per 0.3 blocks in air
    // entities are up to 2 x power away: the cache reaches that far too
    int reach = (int)ceilf(power * 1.3f / 0.22500001f * 0.3f) + 1;
    if (reach < (int)ceilf(power * 2) + 2) reach = (int)ceilf(power * 2) + 2;
    int side = 2 * reach + 1;
    // every chunk it reaches is in memory (a missing one would read as air: the blast
    // went through it); normally they are, as the simulation area stays resident
    for (int qx = (cx - reach) >> 4; qx <= (cx + reach) >> 4; qx++)
        for (int qz = (cz - reach) >> 4; qz <= (cz + reach) >> 4; qz++)
            if (world.chunkInBounds(qx, qz) && !world.isResident(curDim, qx, qz)) world.load(curDim, qx, qz);
    g_cx = cx; g_cy = cy; g_cz = cz; g_reach = reach; g_side = side;
    g_cached = g_cache && side * side * side <= CACHE_CELLS;
    if (g_cached) memset(g_cache, 0xFF, sizeof(uint16_t) * side * side * side);
    g_listLen = 0;
    if (g_set) {
        memset(g_set, 0, sizeof(uint32_t) * SET_CAP);
        // the rays (ServerExplosion#calculateExplodedPositions)
        bool costs = g_cost && g_dirs && g_cached;
        if (costs) for (int i = 0; i < g_side * g_side * g_side; i++) g_cost[i] = -2;
        const float ox = (float)(x - cx), oy = (float)(y - cy), oz = (float)(z - cz);
        for (int r = 0; r < RAYS; r++) {
            // in floats, as offsets from the centre's block: the ESP32-S3 has no
            // double-precision FPU, and the offsets stay small (exact far out too)
            float dx, dy, dz;
            if (g_dirs) {
                dx = g_dirs[r * 3]; dy = g_dirs[r * 3 + 1]; dz = g_dirs[r * 3 + 2];
            } else {   // (no PSRAM for the table: the same, computed here)
                int j = 0, k = 0, l = 0, n = -1;
                for (int a2 = 0; a2 < 4096 && n < r; a2++) {
                    j = a2 >> 8; k = (a2 >> 4) & 15; l = a2 & 15;
                    if (j == 0 || j == 15 || k == 0 || k == 15 || l == 0 || l == 15) n++;
                }
                dx = j / 15.0f * 2 - 1; dy = k / 15.0f * 2 - 1; dz = l / 15.0f * 2 - 1;
                float d = sqrtf(dx * dx + dy * dy + dz * dz);
                dx = dx / d * 0.3f; dy = dy / d * 0.3f; dz = dz / d * 0.3f;
            }
            float f = power * (0.7f + (float)mobs::rng().unit() * 0.6f);
            float px = ox, py = oy, pz = oz;
            int lx = INT32_MIN, ly = 0, lz = 0;
            float cost = -1;
            bool fluid = false;
            for (; f > 0.0f; f -= 0.22500001f) {
                int ix = ifloor(px), iy = ifloor(py), iz = ifloor(pz);
                bool entered = ix != lx || iy != ly || iz != lz;
                if (entered) {
                    lx = ix; ly = iy; lz = iz;
                    int ci = -1;
                    if (costs) {
                        int ax = ix + g_reach, ay = iy + g_reach, az = iz + g_reach;
                        if (ax >= 0 && ay >= 0 && az >= 0 && ax < g_side && ay < g_side && az < g_side)
                            ci = (ay * g_side + az) * g_side + ax;
                    }
                    float c = ci >= 0 ? g_cost[ci] : -2;
                    if (c == -2) {
                        int bx = cx + ix, by = cy + iy, bz = cz + iz;
                        if (!dimHasY(curDim, by) || !world.blockInBounds(bx, bz)) {
                            c = -3;   // out of the world: the ray ends
                        } else {
                            uint16_t st = cachedState(*this, bx, by, bz);
                            float res = resistanceAt(st);
                            c = res < 0 ? -1 : (res + 0.3f) * 0.3f + (stateIsFluid(st) ? 1000.0f : 0.0f);
                        }
                        if (ci >= 0) g_cost[ci] = c;
                    }
                    if (c == -3) break;
                    fluid = c >= 1000.0f;
                    cost = fluid ? c - 1000.0f : c;
                }
                if (cost >= 0) f -= cost;
                // only where the ray enters a block: further steps in it only weaken it
                if (entered && f > 0.0f && cost >= 0 && !fluid) setAdd(keyOf(ix, iy, iz));
                px += dx;
                py += dy;
                pz += dz;
            }
        }
    }
    uint64_t tRays = plat::micros();
    // the effect: particles, sound, and a player's push (1.21.2+: the blocks follow as
    // block changes); the entities first, so the pushes are known
    double range = power * 2.0;
    double pushX[MC_MAX_PLAYERS] = {}, pushY[MC_MAX_PLAYERS] = {}, pushZ[MC_MAX_PLAYERS] = {};
    bool pushed[MC_MAX_PLAYERS] = {};
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& p = players[i];
        if (!p.inPlay() || p.dead || p.e.dim != curDim || p.gamemode == GM_SPECTATOR) continue;
        double dist = sqrt((p.e.x - x) * (p.e.x - x) + (p.e.y - y) * (p.e.y - y) + (p.e.z - z) * (p.e.z - z)) / range;
        if (dist > 1.0) continue;
        double dx = p.e.x - x, dy = p.e.y + 1.62 - y, dz = p.e.z - z;
        double len = sqrt(dx * dx + dy * dy + dz * dz);
        if (len == 0) continue;
        float seen = seenPercent(*this, x, y, z, p.e);
        double impact = (1 - dist) * seen;
        float dmg = (float)((impact * impact + impact) / 2 * 7 * range + 1);
        if (p.isSurvivalLike()) damagePlayer(p, dmg, DC_EXPLOSION, source);
        if (p.gamemode == GM_CREATIVE && p.flying) continue;
        pushX[i] = dx / len * impact;
        pushY[i] = dy / len * impact;
        pushZ[i] = dz / len * impact;
        pushed[i] = true;
    }
    for (int i = 0; i < MC_MAX_ENTITIES; i++) {
        Entity& m = entities[i];
        if (m.kind == EK_NONE || m.removed || m.id == source || m.dim != curDim) continue;
        double dist = sqrt((m.x - x) * (m.x - x) + (m.y - y) * (m.y - y) + (m.z - z) * (m.z - z)) / range;
        if (dist > 1.0) continue;
        if (m.kind == EK_CRYSTAL) {   // a chain of crystal explosions
            hitCrystal(m, source);
            continue;
        }
        bool living = m.kind == EK_MOB;
        double dx = m.x - x, dy = (living ? m.y + m.height * 0.85 : m.y) - y, dz = m.z - z;
        double len = sqrt(dx * dx + dy * dy + dz * dz);
        if (len == 0) continue;
        float seen = seenPercent(*this, x, y, z, m);
        double impact = (1 - dist) * seen;
        float dmg = (float)((impact * impact + impact) / 2 * 7 * range + 1);
        if (living) damageEntity(m, dmg, DC_EXPLOSION, source);
        else if (m.kind == EK_ITEM) {   // items burn up (5 health), but nether stars
            if (m.item.id != itm::NetherStar && dmg >= 5) {
                removeEntity(m);
                continue;
            }
        } else if (m.kind == EK_BOAT || m.kind == EK_MINECART) {
            m.hurtDir = (int8_t)-m.hurtDir;
            m.hurtTicks = 10;
            m.vehicleDamage += dmg * 10;
            m.metaDirty = true;
            if (m.kind == EK_MINECART && m.type == ent::TntMinecart) {
                primeTntMinecart(m, (int)(mobs::rng().range(20) + mobs::rng().range(20)));
            } else if (m.vehicleDamage > 40) {
                breakVehicle(m, true);
                continue;
            }
        }
        if (m.kind == EK_FALLING_BLOCK || m.kind == EK_CLOUD || m.kind == EK_FIREBALL) continue;
        m.vx += dx / len * impact;
        m.vy += dy / len * impact;
        m.vz += dz / len * impact;
        m.velDirty = true;
    }
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& p = players[i];
        if (!p.inPlay() || p.e.dim != curDim || !p.hasChunk(curDim, cx >> 4, cz >> 4)) continue;
        Packet pk(pkt::s2c::Explosion);
        pk.w.f64(x);
        pk.w.f64(y);
        pk.w.f64(z);
        pk.w.boolean(pushed[i]);   // this player's push
        if (pushed[i]) {
            pk.w.f64(pushX[i]);
            pk.w.f64(pushY[i]);
            pk.w.f64(pushZ[i]);
        }
        pk.w.varint(power >= 2 ? particle::ExplosionEmitter : particle::Explosion);
        pk.w.varint(0);   // the sound by name
        pk.w.string("entity.generic.explode");
        pk.w.boolean(false);
        p.conn.send(pk);
    }
    uint64_t tEntities = plat::micros();
    // the blocks, in a random order (vanilla shuffles them); TNT is primed with a short fuse
    for (int i = g_listLen - 1; i > 0; i--) {
        int j = (int)mobs::rng().range(i + 1);
        int32_t t = g_list[i];
        g_list[i] = g_list[j];
        g_list[j] = t;
    }
    // removed all at once: each block becomes air (or the water it held) without updates
    // of its own; the drops merge into stacks; then the blocks around the crater react
    // once (support, falling, fluids, shapes, redstone). Breaking them one by one took
    // about 1 ms a block on the ESP32-S3.
    if (!collected_) collected_ = new CollectedDrop[MAX_COLLECTED];
    nCollected_ = 0;
    collectDrops_ = collected_ != nullptr;
    int primed = 0;
    for (int k = 0; k < g_listLen; k++) {
        uint32_t key = (uint32_t)g_list[k];
        int bx = cx + (int)((key >> 20) & 1023) - 512, by = cy + (int)((key >> 10) & 1023) - 512, bz = cz + (int)(key & 1023) - 512;
        uint16_t st = blockAt(bx, by, bz);
        if (stateIsAir(st)) continue;
        if (blockIdOf(st) == blk::Tnt) {   // primed after the rest is gone
            g_list[primed++] = (int32_t)key;   // (the list's front is done with)
            continue;
        }
        Chunk* c = world.get(curDim, bx >> 4, bz >> 4);
        TileEntity* t = c ? c->tileAt(bx & 15, by, bz & 15) : nullptr;
        if (t) {   // a container spills
            for (int i = 0; i < t->slotCount(); i++)
                if (!t->items[i].empty()) dropItem(bx + 0.5, by + 0.5, bz + 0.5, t->items[i]);
            c->removeTile(bx & 15, by, bz & 15);
            c->dirty = true;
        }
        if (kind == EXPLODE_TNT || mobs::rng().unit() * power < 1.0) dropBlockItems(bx, by, bz, st);
        world.setBlock(curDim, bx, by, bz, getBool(st, "waterlogged") ? bs::Water : 0, true, 2);
    }
    collectDrops_ = false;
    for (int i = 0; i < nCollected_; i++) {
        dropItem(collected_[i].x, collected_[i].y, collected_[i].z, collected_[i].st, true);
        collected_[i].st.clear();
    }
    nCollected_ = 0;
    for (int k = 0; k < primed; k++) {
        uint32_t key = (uint32_t)g_list[k];
        primeTnt(cx + (int)((key >> 20) & 1023) - 512, cy + (int)((key >> 10) & 1023) - 512, cz + (int)(key & 1023) - 512,
                 source, true);
    }
    // the crater's edge: the blocks next to it that are still there
    {
        int32_t (*edge)[3] = (int32_t (*)[3])plat::bigAlloc(sizeof(int32_t) * 3 * 2048);
        int ne = 0;
        for (int k = primed; k < g_listLen && edge; k++) {
            uint32_t key = (uint32_t)g_list[k];
            int dx0 = (int)((key >> 20) & 1023) - 512, dy0 = (int)((key >> 10) & 1023) - 512, dz0 = (int)(key & 1023) - 512;
            checkPortalsAround(cx + dx0, cy + dy0, cz + dz0);
            for (int f = 0; f < 6 && ne < 2048; f++) {
                int ex = dx0 + FACE_DX[f], ey = dy0 + FACE_DY[f], ez = dz0 + FACE_DZ[f];
                uint32_t nk = keyOf(ex, ey, ez);
                if (setHas(nk) || !setMark(nk | 0x40000000u)) continue;   // in the crater, or seen
                if (stateIsAir(blockAt(cx + ex, cy + ey, cz + ez))) continue;
                edge[ne][0] = cx + ex; edge[ne][1] = cy + ey; edge[ne][2] = cz + ez;
                ne++;
            }
        }
        if (edge) {
            updateBlocks(edge, ne, 4096, 8192);
            for (int i = 0; i < ne; i++) redstone.updateAt(*this, edge[i][0], edge[i][1], edge[i][2]);
            plat::bigFree(edge);
        }
    }
    uint64_t tBlocks = plat::micros();
    if (fire)
        for (int k = 0; k < g_listLen; k++) {
            uint32_t key = (uint32_t)g_list[k];
            int bx = cx + (int)((key >> 20) & 1023) - 512, by = cy + (int)((key >> 10) & 1023) - 512, bz = cz + (int)(key & 1023) - 512;
            if (mobs::rng().range(3) == 0 && stateIsAir(blockAt(bx, by, bz)) && stateOpaque(blockAt(bx, by - 1, bz))) {
                setBlock(bx, by, bz, bs::Fire);
                scheduleTick(bx, by, bz, 200);   // burns out
            }
        }
    uint32_t us = (uint32_t)(plat::micros() - t0);
    explosionStats.raysUs += tRays - t0;
    explosionStats.entitiesUs += tEntities - tRays;
    explosionStats.blocksUs += tBlocks - tEntities;
    explosionUsThisTick_ += us;
    if (explosionUsThisTick_ > explosionStats.tickMaxUs) explosionStats.tickMaxUs = explosionUsThisTick_;
    explosionStats.count++;
    explosionStats.blocks += (uint32_t)g_listLen;
    explosionStats.totalUs += us;
    if (us > explosionStats.maxUs) explosionStats.maxUs = us;
}

}  // namespace mc
