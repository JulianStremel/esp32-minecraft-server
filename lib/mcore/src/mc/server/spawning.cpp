// Natural mob spawning by light level, after vanilla 1.16.5 (NaturalSpawner, Monster,
// Animal). The game loop walks vanilla's spawn packs over the live chunk with cheap
// block checks only; a background job computes the chunk's light from a snapshot and
// applies the light rules; the game loop spawns what passed (after checking again).
#include <math.h>
#include "mc/registry.h"
#include "mc/server/server.h"
#include "mc/world/light.h"
#include "mc/world/noise.h"

namespace mc {

static Rng s_rng(0x5A3E5EED);

// Vanilla Level#updateSkyBrightness: how much the sky light is darkened (0 at noon, 11
// at midnight; rain and thunder darken it further).
int skyDarkening(int64_t timeOfDay, bool raining, bool thundering) {
    // DimensionType#timeOfDay: the celestial angle
    double d0 = (double)(timeOfDay % 24000) / 24000.0 - 0.25;
    d0 -= floor(d0);
    double d1 = 0.5 - cos(d0 * M_PI) / 2.0;
    double angle = (d0 * 2.0 + d1) / 3.0;
    double c = cos(angle * 2.0 * M_PI);
    if (c < -0.25) c = -0.25;
    if (c > 0.25) c = 0.25;
    double d2 = 0.5 + 2.0 * c;
    double rain = raining ? 1.0 - 5.0 / 16.0 : 1.0;
    double thunder = thundering ? 1.0 - 5.0 / 16.0 : 1.0;
    return (int)((1.0 - d2 * rain * thunder) * 11.0);
}

// Monster#isDarkEnoughToSpawn
bool darkEnoughForMonster(int sky, int block, int darkening, bool thundering, Rng& r) {
    if (sky > r.range(32)) return false;
    int s = sky - (thundering ? 10 : darkening);
    if (s < 0) s = 0;
    int light = block > s ? block : s;
    return light <= r.range(8);
}

// Animal#checkAnimalSpawnRules (the grass below is checked by the caller)
bool brightEnoughForAnimal(int sky, int block) { return (sky > block ? sky : block) > 8; }

namespace {

struct Candidate {
    int8_t x, z;        // local to the chunk
    uint8_t y;
    bool hostile;
    uint16_t type;
};

const uint16_t HOSTILE[] = {ent::Zombie, ent::Skeleton, ent::Creeper, ent::Spider};   // weight 100 each
struct Weighted { uint16_t type; int weight; };
const Weighted PASSIVE[] = {{ent::Sheep, 12}, {ent::Pig, 10}, {ent::Chicken, 10}, {ent::Cow, 8}};

uint16_t pickType(bool hostile) {
    if (hostile) return HOSTILE[s_rng.range(4)];
    int r = s_rng.range(40);
    for (const Weighted& w : PASSIVE) {
        if (r < w.weight) return w.type;
        r -= w.weight;
    }
    return ent::Pig;
}

}  // namespace

// Light rules for one chunk's spawn candidates, on a worker.
class SpawnJob : public Job {
public:
    Server* srv = nullptr;
    ChunkSnap* snap = nullptr;
    NeighbourEdges edges;
    int cx = 0, cz = 0;
    int darkening = 0;
    bool thundering = false;
    uint64_t seed = 1;
    static const int MAX = 12;
    Candidate cand[MAX];
    bool pass[MAX] = {};
    int n = 0;

    ~SpawnJob() override {
        if (snap) snap->release();
    }
    void run(WorkerScratch& ws) override {
        if (!ws.light.compute(*snap->chunk, edges)) {
            n = 0;
            return;
        }
        Rng r(seed);
        for (int i = 0; i < n; i++) {
            const Candidate& c = cand[i];
            int sky = ws.light.skyAt(c.x, c.y, c.z), block = ws.light.blockAt(c.x, c.y, c.z);
            pass[i] = c.hostile ? darkEnoughForMonster(sky, block, darkening, thundering, r)
                                : brightEnoughForAnimal(sky, block);
        }
    }
    void finish() override { srv->spawnFinished(*this); }
    const char* kind() const override { return "spawn"; }
};

// Can a mob stand at (x, y, z) (world coordinates)? Vanilla's ON_GROUND placement: a
// solid block below (not bedrock), and room for its feet and head without fluid.
static bool standable(Server& s, int x, int y, int z, bool passive) {
    if (y < 1 || y > 254) return false;
    uint16_t below = s.blockAt(x, y - 1, z), feet = s.blockAt(x, y, z), head = s.blockAt(x, y + 1, z);
    if (!stateCollides(below) || blockIdOf(below) == blk::Bedrock || stateIsFluid(below)) return false;
    if (stateCollides(feet) || stateIsFluid(feet) || stateCollides(head) || stateIsFluid(head)) return false;
    return !passive || blockIdOf(below) == blk::GrassBlock;
}

// Vanilla: not within 24 blocks of a player, and within 128 of one.
static bool rightDistance(Server& s, double x, double y, double z) {
    double best = 1e18;
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        const Player& p = s.players[i];
        if (!p.inPlay() || p.gamemode == GM_SPECTATOR || p.e.dim != s.curDim) continue;
        double dx = p.e.x - x, dy = p.e.y - y, dz = p.e.z - z, d = dx * dx + dy * dy + dz * dz;
        if (d < best) best = d;
    }
    return best > 24.0 * 24.0 && best < 128.0 * 128.0;
}

void Server::tickMobSpawning() {
    // up to 8 attempts every other tick (block checks only, on the live chunks); the
    // first chunk with candidates gets a light job, one in flight (at most 10 jobs/s,
    // a few percent of the workers). Vanilla makes one attempt per chunk and tick; most
    // fail the block checks, which is why more than one attempt is needed here.
    if (!cfg.spawnMobs || ticks % 2 != 0) return;
    spawnInNether();
    if (spawnInFlight_) return;
    const Player* active[MC_MAX_PLAYERS];
    int np = 0;
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        const Player& p = players[i];
        // overworld players (the Nether: spawnInNether; the End's mobs come later)
        if (p.inPlay() && !p.dead && p.positionReady && p.gamemode != GM_SPECTATOR && p.e.dim == DIM_OVERWORLD)
            active[np++] = &p;
    }
    if (!np) return;
    InDim in(*this, DIM_OVERWORLD);
    int hostile = 0, passive = 0;
    for (const Entity& e : entities)
        if (e.kind == EK_MOB && !e.removed && e.health > 0 && e.dim == DIM_OVERWORLD) (e.hostile ? hostile : passive)++;
    // vanilla caps per 17 x 17 chunks around the players (monsters 70, creatures 10),
    // within the entity budget of this server
    int monsterCap = 70 * np, creatureCap = 10 * np;
    if (monsterCap > cfg.maxMobs * 3 / 4) monsterCap = cfg.maxMobs * 3 / 4;
    if (creatureCap > cfg.maxMobs / 4) creatureCap = cfg.maxMobs / 4;
    bool wantHostile = cfg.difficulty > 0 && hostile < monsterCap;
    bool wantPassive = worldTick() % 400 < 40 && passive < creatureCap;   // vanilla: animals every 400 ticks
    if (!wantHostile && !wantPassive) return;
    bool doHostile = wantHostile && (!wantPassive || s_rng.range(2) == 0);

    SpawnJob* j = new SpawnJob();
    int cx = 0, cz = 0;
    for (int attempt = 0; attempt < 8 && j->n == 0; attempt++) {
        // a random chunk within 8 chunks of a random player
        const Player& p = *active[s_rng.range(np)];
        cx = ((int)floor(p.e.x) >> 4) + s_rng.between(-8, 8);
        cz = ((int)floor(p.e.z) >> 4) + s_rng.between(-8, 8);
        Chunk* c = world.peek(curDim, cx, cz);
        if (!c || !world.chunkInBounds(cx, cz)) continue;
        // vanilla NaturalSpawner#spawnCategoryForPosition: a random column and height up
        // to the surface, then 3 packs of up to 4 tries, each moving up to 5 blocks; here
        // the packs stay in the chunk (its light is what the job computes)
        int sx = s_rng.range(16), sz = s_rng.range(16);
        int sy = s_rng.between(0, c->height(sx, sz) + 1);
        if (stateCollides(c->get(sx, sy, sz))) continue;
        for (int pack = 0; pack < 3; pack++) {
            int x = sx, z = sz;
            uint16_t type = pickType(doHostile);
            for (int k = 0; k < 4 && j->n < SpawnJob::MAX; k++) {
                x += s_rng.range(6) - s_rng.range(6);
                z += s_rng.range(6) - s_rng.range(6);
                if (x < 0 || x > 15 || z < 0 || z > 15) continue;
                int wx = cx * 16 + x, wz = cz * 16 + z;
                if (!standable(*this, wx, sy, wz, !doHostile) || !rightDistance(*this, wx + 0.5, sy, wz + 0.5)) continue;
                j->cand[j->n++] = Candidate{(int8_t)x, (int8_t)z, (uint8_t)sy, doHostile, type};
            }
        }
    }
    if (j->n == 0 || !(j->snap = world.snapshot(curDim, cx, cz))) {
        delete j;
        return;
    }
    j->srv = this;
    j->cx = cx;
    j->cz = cz;
    j->edges.gather(world, DIM_OVERWORLD, cx, cz);
    j->darkening = skyDarkening(meta.timeOfDay, meta.raining != 0, meta.raining == 2);
    j->thundering = meta.raining == 2;
    j->seed = ((uint64_t)s_rng.u32() << 32) | s_rng.u32();
    spawnInFlight_ = true;
    chunkJobs.queue().submit(j, PRIO_BACKGROUND);
}

// The Nether (vanilla's nether_wastes spawners): zombified piglins (weight 100, packs of
// 4), ghasts (50, alone, and only 1 attempt in 20 succeeds: Ghast#checkGhastSpawnRules)
// and magma cubes (2, packs of 4). Their rules do not depend on light, so this needs no
// light job: block checks on the game loop only, a few per call.
void Server::spawnInNether() {
    const Player* active[MC_MAX_PLAYERS];
    int np = 0;
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        const Player& p = players[i];
        if (p.inPlay() && !p.dead && p.positionReady && p.gamemode != GM_SPECTATOR && p.e.dim == DIM_NETHER)
            active[np++] = &p;
    }
    if (!np || cfg.difficulty == 0) return;
    InDim in(*this, DIM_NETHER);
    int hostile = 0, total = 0;
    for (const Entity& e : entities)
        if (e.kind == EK_MOB && !e.removed && e.health > 0) {
            total++;
            if (e.dim == DIM_NETHER && e.hostile) hostile++;
        }
    int cap = 70 * np;
    if (cap > cfg.maxMobs * 3 / 4) cap = cfg.maxMobs * 3 / 4;
    if (hostile >= cap || total >= cfg.maxMobs) return;
    for (int attempt = 0; attempt < 4; attempt++) {
        const Player& p = *active[s_rng.range(np)];
        int cx = ((int)floor(p.e.x) >> 4) + s_rng.between(-8, 8);
        int cz = ((int)floor(p.e.z) >> 4) + s_rng.between(-8, 8);
        if (!world.peek(curDim, cx, cz) || !world.chunkInBounds(cx, cz)) continue;
        int x = cx * 16 + s_rng.range(16), z = cz * 16 + s_rng.range(16), y = s_rng.between(1, 126);
        if (stateCollides(blockAt(x, y, z))) continue;
        int roll = s_rng.range(152);
        uint16_t type = roll < 100 ? ent::ZombifiedPiglin : (roll < 150 ? ent::Ghast : ent::MagmaCube);
        if (type == ent::Ghast && s_rng.range(20) != 0) continue;
        int n = 0;
        for (int pack = 0; pack < 3 && n < 4; pack++) {
            int px = x, pz = z;
            for (int k = 0; k < 4 && n < 4; k++) {
                px += s_rng.range(6) - s_rng.range(6);
                pz += s_rng.range(6) - s_rng.range(6);
                if (!world.peek(curDim, px >> 4, pz >> 4)) continue;
                if (!standable(*this, px, y, pz, false) || !rightDistance(*this, px + 0.5, y, pz + 0.5)) continue;
                if (type == ent::Ghast) {   // room for its 4 x 4 x 4 body
                    bool room = true;
                    for (int dx = -2; dx <= 1 && room; dx++)
                        for (int dz = -2; dz <= 1 && room; dz++)
                            for (int dy = 0; dy < 4 && room; dy++) room = !stateCollides(blockAt(px + dx, y + dy, pz + dz));
                    if (!room) continue;
                }
                Entity* m = spawnMob(type, px + 0.5, y, pz + 0.5);
                if (!m) return;
                n++;
                spawnStats.spawned++;
                if (type == ent::Ghast) return;   // vanilla's maximum pack size for ghasts: 1
            }
        }
        if (n) return;
    }
}

void Server::spawnFinished(SpawnJob& j) {
    spawnInFlight_ = false;
    spawnStats.jobs++;
    spawnStats.us += j.runUs;
    InDim in(*this, DIM_OVERWORLD);
    if (j.cancelled() || !world.peek(curDim, j.cx, j.cz)) return;
    int hostile = 0, passive = 0;
    for (const Entity& e : entities)
        if (e.kind == EK_MOB && !e.removed && e.health > 0) (e.hostile ? hostile : passive)++;
    for (int i = 0; i < j.n; i++) {
        if (!j.pass[i]) continue;
        const Candidate& c = j.cand[i];
        if (hostile + passive >= cfg.maxMobs) break;
        int wx = j.cx * 16 + c.x, wz = j.cz * 16 + c.z;
        // the world may have changed while the job ran
        if (!standable(*this, wx, c.y, wz, !c.hostile) || !rightDistance(*this, wx + 0.5, c.y, wz + 0.5)) continue;
        if (spawnMob(c.type, wx + 0.5, c.y, wz + 0.5)) {
            (c.hostile ? hostile : passive)++;
            spawnStats.spawned++;
        }
    }
}

}  // namespace mc
