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
constexpr int SET_CAP = 4096;   // a power of two
uint32_t* g_set = nullptr;      // PSRAM (24 KB with the list), taken on the first explosion
int32_t* g_list = nullptr;      // the positions in the order found
int g_listLen = 0;

uint32_t keyOf(int dx, int dy, int dz) { return (uint32_t)(dx + 512) << 20 | (uint32_t)(dy + 512) << 10 | (uint32_t)(dz + 512); }

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

// Is the straight line from a to b free of collision shapes (ClipContext COLLIDER)?
bool clear(Server& s, double ax, double ay, double az, double bx, double by, double bz) {
    double dx = bx - ax, dy = by - ay, dz = bz - az;
    double len = sqrt(dx * dx + dy * dy + dz * dz);
    int steps = (int)(len * 8) + 1;
    int lastX = INT32_MIN, lastY = 0, lastZ = 0;
    uint16_t st = 0;
    for (int i = 1; i < steps; i++) {
        double t = (double)i / steps;
        double x = ax + dx * t, y = ay + dy * t, z = az + dz * t;
        int ix = (int)floor(x), iy = (int)floor(y), iz = (int)floor(z);
        if (ix != lastX || iy != lastY || iz != lastZ) {
            st = dimHasY(s.curDim, iy) ? s.blockAt(ix, iy, iz) : 0;
            lastX = ix; lastY = iy; lastZ = iz;
        }
        if (!stateCollides(st)) continue;
        const int8_t* p = COLLISION_SHAPES + COLLISION_SHAPE_OFFSETS[st];
        int n = *p++;
        double fx = (x - ix) * 32, fy = (y - iy) * 32, fz = (z - iz) * 32;
        for (int k = 0; k < n; k++, p += 6)
            if (fx >= p[0] && fx <= p[3] && fy >= p[1] && fy <= p[4] && fz >= p[2] && fz <= p[5]) return false;
    }
    return true;
}

// Explosion#getSeenPercent: the share of points spread over the entity's box that see
// the centre
float seenPercent(Server& s, double cx, double cy, double cz, const Entity& e) {
    double w = e.width, h = e.height;
    double sx = 1.0 / (w * 2 + 1), sy = 1.0 / (h * 2 + 1), sz = 1.0 / (w * 2 + 1);
    double ox = (1.0 - floor(1.0 / sx) * sx) / 2.0, oz = (1.0 - floor(1.0 / sz) * sz) / 2.0;
    int seen = 0, total = 0;
    for (double fx = 0; fx <= 1; fx += sx)
        for (double fy = 0; fy <= 1; fy += sy)
            for (double fz = 0; fz <= 1; fz += sz) {
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
    }
    int cx = (int)floor(x), cy = (int)floor(y), cz = (int)floor(z);
    g_listLen = 0;
    if (g_set) {
        memset(g_set, 0, sizeof(uint32_t) * SET_CAP);
        // the rays (ServerExplosion#calculateExplodedPositions)
        for (int j = 0; j < 16; j++)
            for (int k = 0; k < 16; k++)
                for (int l = 0; l < 16; l++) {
                    if (!(j == 0 || j == 15 || k == 0 || k == 15 || l == 0 || l == 15)) continue;
                    double dx = j / 15.0 * 2 - 1, dy = k / 15.0 * 2 - 1, dz = l / 15.0 * 2 - 1;
                    double d = sqrt(dx * dx + dy * dy + dz * dz);
                    dx /= d; dy /= d; dz /= d;
                    float f = power * (0.7f + (float)mobs::rng().unit() * 0.6f);
                    double px = x, py = y, pz = z;
                    int lx = INT32_MIN, ly = 0, lz = 0;
                    float res = -1;
                    for (; f > 0.0f; f -= 0.22500001f) {
                        int bx = (int)floor(px), by = (int)floor(py), bz = (int)floor(pz);
                        if (!dimHasY(curDim, by) || !world.blockInBounds(bx, bz)) break;
                        if (bx != lx || by != ly || bz != lz) {
                            res = resistanceAt(blockAt(bx, by, bz));
                            lx = bx; ly = by; lz = bz;
                        }
                        if (res >= 0) f -= (res + 0.3f) * 0.3f;
                        if (f > 0.0f && res >= 0 && !stateIsFluid(blockAt(bx, by, bz))) setAdd(keyOf(bx - cx, by - cy, bz - cz));
                        px += dx * 0.3f;
                        py += dy * 0.3f;
                        pz += dz * 0.3f;
                    }
                }
    }
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
    // the blocks, in a random order (vanilla shuffles them); TNT is primed with a short fuse
    for (int i = g_listLen - 1; i > 0; i--) {
        int j = (int)mobs::rng().range(i + 1);
        int32_t t = g_list[i];
        g_list[i] = g_list[j];
        g_list[j] = t;
    }
    for (int k = 0; k < g_listLen; k++) {
        uint32_t key = (uint32_t)g_list[k];
        int bx = cx + (int)((key >> 20) & 1023) - 512, by = cy + (int)((key >> 10) & 1023) - 512, bz = cz + (int)(key & 1023) - 512;
        uint16_t st = blockAt(bx, by, bz);
        if (stateIsAir(st)) continue;
        if (blockIdOf(st) == blk::Tnt) {
            primeTnt(bx, by, bz, source, true);
            continue;
        }
        bool drop = kind == EXPLODE_TNT || mobs::rng().unit() * power < 1.0;
        breakBlock(bx, by, bz, nullptr, drop);
    }
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
    explosionUsThisTick_ += us;
    explosionStats.count++;
    explosionStats.blocks += (uint32_t)g_listLen;
    explosionStats.totalUs += us;
    if (us > explosionStats.maxUs) explosionStats.maxUs = us;
}

}  // namespace mc
