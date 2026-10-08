// The End: spikes where vanilla puts them, and the dragon fight without clients.
#include <math.h>
#include "testing.h"
#include "mc/registry.h"
#include "mc/server/server.h"
#include "mc/world/generator.h"

using namespace mc;

TEST(end_spikes_match_vanilla) {
    // heights in ring order as vanilla 1.16.5 computes them (java.util.Random, checked
    // with the JRE): seed -> 10 heights
    struct { uint64_t seed; int h[10]; } golden[] = {
        {42, {100, 103, 85, 82, 79, 88, 91, 94, 97, 76}},
        {1, {85, 94, 76, 82, 100, 91, 103, 88, 97, 79}},
        {3735928559ull, {85, 76, 88, 97, 94, 91, 100, 103, 82, 79}},
    };
    for (const auto& g : golden) {
        EndSpike s[10];
        endSpikes(g.seed, s);
        int guarded = 0;
        for (int i = 0; i < 10; i++) {
            CHECK_EQ(s[i].height, g.h[i]);
            CHECK_EQ(s[i].radius, 2 + (s[i].height - 76) / 3 / 3);
            CHECK(fabs(sqrt((double)(s[i].x * s[i].x + s[i].z * s[i].z)) - 42) < 1.5);
            guarded += s[i].guarded;
            CHECK_EQ(s[i].guarded, s[i].height == 79 || s[i].height == 82);
        }
        CHECK_EQ(guarded, 2);
    }
}

TEST(end_generator_builds_the_spikes) {
    Generator g;
    CHECK(g.init(42, WORLD_NORMAL, GENERATOR_LATEST, DIM_END));
    EndSpike s[10];
    endSpikes(42, s);
    for (int i = 0; i < 10; i++) {
        Chunk* c = new Chunk(s[i].x >> 4, s[i].z >> 4, DIM_END);
        g.generate(*c);
        int lx = s[i].x & 15, lz = s[i].z & 15;
        CHECK_EQ(c->get(lx, 10, lz), bs::Obsidian);              // through the island
        CHECK_EQ(c->get(lx, s[i].height - 1, lz), bs::Obsidian);
        CHECK_EQ(c->get(lx, s[i].height, lz), bs::Bedrock);      // where the crystal stands
        if (s[i].guarded) CHECK_EQ(blockIdOf(c->get(lx, s[i].height + 3, lz)), blk::IronBars);
        else CHECK_EQ(c->get(lx, s[i].height + 3, lz), 0);
        delete c;
    }
}

static Server* endServer() {
    static Server* s = nullptr;
    if (s) return s;
    s = new Server();
    ServerConfig cfg;
    cfg.port = 0;
    cfg.worldType = WORLD_NORMAL;
    cfg.spawnMobs = false;
    cfg.seed = 42;
    if (!s->begin(cfg, nullptr)) return nullptr;
    for (int cx = -4; cx <= 4; cx++)
        for (int cz = -4; cz <= 4; cz++) s->world.load(DIM_END, cx, cz);
    return s;
}

static int count(Server& s, uint8_t kind, uint16_t type) {
    int n = 0;
    for (const Entity& e : s.entities) n += e.kind == kind && !e.removed && e.type == type;
    return n;
}

TEST(dragon_fight_from_start_to_the_open_portal) {
    Server* s = endServer();
    CHECK(s);
    if (!s) return;
    s->startDragonFight();
    Entity* d = s->dragon();
    CHECK(d);
    if (!d) return;
    const DragonFight& f = s->wstate.dragon;
    CHECK_EQ(f.state, DragonFight::DRAGON_ALIVE);
    CHECK(f.portalY > 50 && f.portalY < 80);
    CHECK_EQ(count(*s, EK_CRYSTAL, ent::EndCrystal), 10);
    CHECK_EQ(s->crystalsAlive(), 10);
    CHECK(d->health == 200);
    // the inactive exit portal: a bedrock pillar, no portal blocks yet
    CHECK_EQ(s->world.getBlock(DIM_END, 0, f.portalY + 3, 0), bs::Bedrock);
    CHECK_EQ(s->world.getBlock(DIM_END, 1, f.portalY, 1), 0);
    CHECK_EQ(s->world.getBlock(DIM_END, 3, f.portalY, 0), bs::Bedrock);
    // parts: the 8 ids after the dragon's are its parts, not other entities
    int part = -1;
    CHECK(s->dragonByPart(d->id + 1, part) == d);
    CHECK_EQ(part, 0);
    CHECK(s->dragonByPart(d->id + 8, part) == d);
    CHECK_EQ(part, 7);
    CHECK(!s->dragonByPart(d->id, part));
    Entity* other = s->spawnEntity(EK_ITEM, ent::Item, 0, 100, 0);
    CHECK(other && other->id > d->id + 8);
    if (other) s->removeEntity(*other);
    // damage: the head takes all of it, the rest a quarter plus 1
    s->dragonPart_ = 0;
    s->damageEntity(*d, 20, DC_ATTACK, -1);
    CHECK(fabs(d->health - 180) < 1e-3);
    d->invuln = 0;
    s->dragonPart_ = 2;
    s->damageEntity(*d, 20, DC_ATTACK, -1);
    CHECK(fabs(d->health - 174) < 1e-3);
    s->dragonPart_ = -1;
    // a crystal destroyed: one fewer; the one healing it costs the dragon 10
    Entity* c = nullptr;
    for (Entity& e : s->entities)
        if (e.kind == EK_CRYSTAL && !e.removed) { c = &e; break; }
    CHECK(c);
    d->target = c->id;
    d->invuln = 0;
    s->hitCrystal(*c, -1);
    CHECK_EQ(s->crystalsAlive(), 9);
    CHECK(fabs(d->health - 164) < 1e-3);
    // flying for a while: it moves and stays in the air above the island
    Server::InDim in(*s, DIM_END);
    double x0 = d->x, z0 = d->z;
    for (int i = 0; i < 400; i++) s->tickDragon(*d);
    printf("    after 20 s: at %.1f %.1f %.1f, phase %d\n", d->x, d->y, d->z, d->phase);
    CHECK(fabs(d->x - x0) + fabs(d->z - z0) > 10);
    CHECK(d->y > 20 && d->y < 220);
    // killed: dying, then the portal opens with the egg on it
    d->invuln = 0;
    s->dragonPart_ = 0;
    s->damageEntity(*d, 1000, DC_ATTACK, -1);
    s->dragonPart_ = -1;
    CHECK_EQ(d->phase, 9);   // DYING
    int t = 0;
    while (!d->removed && t < 2000) {
        s->tickDragon(*d);
        t++;
    }
    printf("    died and the portal opened after %d ticks\n", t);
    CHECK(d->removed);
    CHECK_EQ(f.state, DragonFight::KILLED);
    CHECK(f.previouslyKilled);
    CHECK_EQ(s->world.getBlock(DIM_END, 1, f.portalY, 1), bs::EndPortal);
    CHECK_EQ(blockIdOf(s->world.getBlock(DIM_END, 0, f.portalY + 4, 0)), blk::DragonEgg);
    // it stays dead: the fight does not start again
    s->tickDragonFight();
    CHECK(!s->dragon());
    // /dragon reset: as never fought, the portal closed and at the same height
    int py = f.portalY;
    s->resetDragonFight(true);
    CHECK_EQ(f.state, DragonFight::NOT_STARTED);
    CHECK(!f.previouslyKilled);
    CHECK_EQ(s->world.getBlock(DIM_END, 0, py + 4, 0), 0);
    CHECK_EQ(s->world.getBlock(DIM_END, 1, py, 1), 0);
    s->startDragonFight();
    CHECK_EQ(f.portalY, py);
    CHECK(s->dragon());
    CHECK_EQ(s->crystalsAlive(), 10);
    s->resetDragonFight(true);
}

TEST(world_state_reads_version_1) {
    // a record written before the dragon fight existed (portals only)
    uint8_t v1[] = {1, 1, 0, 1, 0, 0, 0, 5, 0, 70, 0xFF, 0xFF, 0xFF, 0xFB, 0, 0, 0x43, 0x48, 0, 0, 0, 0};
    WorldState w;
    CHECK(w.decode(v1, sizeof(v1)));
    CHECK_EQ(w.nPortals, 1);
    CHECK_EQ(w.portals[0].x, 5);
    CHECK_EQ(w.portals[0].y, 70);
    CHECK_EQ(w.portals[0].z, -5);
    CHECK_EQ(w.dragon.crystals, 0x3FF);
    CHECK_EQ(w.dragon.portalY, 0);
    CHECK(w.dragon.dragonHealth == 200);
    w.dragon.portalY = 61;
    w.dragon.crystals = 0x155;
    uint8_t buf[64];
    size_t n = w.encode(buf, sizeof(buf));
    WorldState r;
    CHECK(r.decode(buf, n));
    CHECK_EQ(r.dragon.portalY, 61);
    CHECK_EQ(r.dragon.crystals, 0x155);
}
