// Nether mobs without clients: magma cubes splitting, zombified piglins angering as a
// group, ghast fireballs exploding and being sent back.
#include <math.h>
#include "testing.h"
#include "mc/registry.h"
#include "mc/server/server.h"

using namespace mc;

static Server* mobServer() {
    static Server* s = nullptr;
    if (s) return s;
    s = new Server();
    ServerConfig cfg;
    cfg.port = 0;
    cfg.worldType = WORLD_FLAT;
    cfg.spawnMobs = false;
    cfg.seed = 9;
    if (!s->begin(cfg, nullptr)) return nullptr;
    Server::InDim in(*s, DIM_NETHER);
    for (int cx = -2; cx <= 2; cx++)
        for (int cz = -2; cz <= 2; cz++) s->world.load(DIM_NETHER, cx, cz);
    // an open box of air with a netherrack floor at y 99
    for (int x = -20; x <= 20; x++)
        for (int z = -20; z <= 20; z++) {
            s->world.setBlock(DIM_NETHER, x, 99, z, bs::Netherrack);
            for (int y = 100; y < 112; y++) s->world.setBlock(DIM_NETHER, x, y, z, 0);
        }
    return s;
}

static int countMobs(Server& s, uint16_t type, int* sizes = nullptr) {
    int n = 0;
    for (const Entity& e : s.entities)
        if (e.kind == EK_MOB && !e.removed && e.health > 0 && e.type == type) {
            if (sizes) sizes[n] = e.size;
            n++;
        }
    return n;
}

static void clearMobs(Server& s) {
    for (Entity& e : s.entities)
        if (e.kind != EK_NONE) e.removed = true, e.kind = EK_NONE;
}

TEST(magma_cubes_split_when_killed) {
    Server* s = mobServer();
    CHECK(s);
    if (!s) return;
    clearMobs(*s);
    Server::InDim in(*s, DIM_NETHER);
    Entity* m = s->spawnMob(ent::MagmaCube, 0.5, 100, 0.5);
    CHECK(m);
    s->setMagmaCubeSize(*m, 4);
    CHECK(m->health == 16);
    CHECK(fabs(m->width - 2.04 * 0.255 * 4) < 1e-4);
    CHECK(m->fireImmune);
    s->damageEntity(*m, 100, DC_ATTACK, -1);
    int sizes[8] = {};
    int n = countMobs(*s, ent::MagmaCube, sizes);
    printf("    a size 4 cube split into %d\n", n);
    CHECK(n >= 2 && n <= 4);
    for (int i = 0; i < n; i++) CHECK_EQ(sizes[i], 2);
    // the smallest do not split
    for (Entity& e : s->entities)
        if (e.kind == EK_MOB && !e.removed && e.health > 0 && e.type == ent::MagmaCube) s->setMagmaCubeSize(e, 1);
    int before = countMobs(*s, ent::MagmaCube);
    for (Entity& e : s->entities)
        if (e.kind == EK_MOB && !e.removed && e.health > 0 && e.type == ent::MagmaCube) {
            s->damageEntity(e, 100, DC_ATTACK, -1);
            break;
        }
    CHECK_EQ(countMobs(*s, ent::MagmaCube), before - 1);
}

TEST(zombified_piglins_anger_as_a_group) {
    Server* s = mobServer();
    if (!s) return;
    clearMobs(*s);
    Server::InDim in(*s, DIM_NETHER);
    Entity* a = s->spawnMob(ent::ZombifiedPiglin, 0.5, 100, 0.5);
    Entity* b = s->spawnMob(ent::ZombifiedPiglin, 20.5, 100, 10.5);    // within 35 blocks
    Entity* far = s->spawnMob(ent::ZombifiedPiglin, 60.5, 100, 0.5);  // beyond
    CHECK(a && b && far);
    CHECK(a->fireImmune && a->hostile);
    CHECK_EQ(a->angryAt, -1);
    s->angerPiglins(*a, 4242);
    CHECK_EQ(a->angryAt, 4242);
    CHECK_EQ(b->angryAt, 4242);
    CHECK_EQ(far->angryAt, -1);
    CHECK(a->angerTicks >= 400 && a->angerTicks < 780);
}

TEST(ghast_fireballs_explode_and_can_be_sent_back) {
    Server* s = mobServer();
    if (!s) return;
    clearMobs(*s);
    Server::InDim in(*s, DIM_NETHER);
    // a fireball towards a wall at x 10
    for (int y = 99; y < 105; y++)
        for (int z = -3; z <= 3; z++) s->world.setBlock(DIM_NETHER, 10, y, z, bs::Netherrack);
    Entity* ghast = s->spawnMob(ent::Ghast, -10.5, 101, 0.5);
    CHECK(ghast);
    Entity* f = s->shootFireball(*ghast, -6, 102, 0.5, 1, 0, 0);
    CHECK(f);
    CHECK(fabs(f->px - 0.1) < 1e-9 && f->py == 0);
    int ticks = 0;
    while (!f->removed && ticks < 200) {
        s->tickFireball(*f);
        ticks++;
    }
    CHECK(f->removed);
    int holes = 0, fire = 0;
    for (int y = 99; y < 106; y++)
        for (int z = -3; z <= 3; z++)
            for (int x = 8; x <= 11; x++) {
                uint16_t b = s->world.getBlock(DIM_NETHER, x, y, z);
                holes += x == 10 && y < 105 && b == 0;
                fire += blockIdOf(b) == blk::Fire;
            }
    printf("    hit the wall after %d ticks: %d blocks gone, %d fires\n", ticks, holes, fire);
    CHECK(ticks > 5 && ticks < 60);
    CHECK(holes > 0);
    // sent back by a player, it kills the ghast
    Player& p = s->players[1];
    ConnState was = p.state;
    p.state = CS_PLAY;
    p.e.id = s->newEntityId();
    p.e.dim = DIM_NETHER;
    p.e.x = 0.5; p.e.y = 100; p.e.z = 0.5;
    p.e.yaw = 90;   // looking towards -x, at the ghast
    p.e.pitch = 0;
    Entity* g = s->shootFireball(*ghast, -7, 101.6, 0.5, 1, 0, 0);
    CHECK(g);
    s->deflectFireball(*g, p);
    CHECK_EQ(g->owner, p.e.id);
    CHECK(g->px < -0.09);
    ticks = 0;
    while (!g->removed && ticks < 200) {
        s->tickFireball(*g);
        ticks++;
    }
    CHECK(g->removed);
    printf("    sent back: the ghast has %.0f health after %d ticks\n", ghast->health, ticks);
    CHECK(ghast->health <= 0);
    p.state = was;
}
