// A Nether fortress's mobs without clients: blazes' small fireballs, wither skeletons.
#include <math.h>
#include "testing.h"
#include "mc/registry.h"
#include "mc/server/server.h"

using namespace mc;

static Server* fortressServer() {
    static Server* s = nullptr;
    if (s) return s;
    s = new Server();
    ServerConfig cfg;
    cfg.port = 0;
    cfg.worldType = WORLD_NORMAL;
    cfg.spawnMobs = false;
    cfg.seed = 42;
    if (!s->begin(cfg, nullptr)) return nullptr;
    for (int cx = -2; cx <= 2; cx++)
        for (int cz = -2; cz <= 2; cz++) s->world.load(DIM_NETHER, cx, cz);
    // a room: air over a netherrack floor at y 99, a wall at x 20
    Server::InDim in(*s, DIM_NETHER);
    for (int x = 0; x <= 20; x++)
        for (int z = 0; z <= 6; z++)
            for (int y = 99; y <= 106; y++)
                s->setBlock(x, y, z, y == 99 || x == 20 ? bs::Netherrack : bs::Air);
    return s;
}

// Flies the fireball until it is gone (at most 5 s).
static void fly(Server& s, Entity* f) {
    for (int t = 0; t < 100 && f->kind != EK_NONE && !f->removed; t++) s.tickFireball(*f);
}

TEST(small_fireball_burns_what_it_hits) {
    Server* s = fortressServer();
    CHECK(s);
    if (!s) return;
    Server::InDim in(*s, DIM_NETHER);
    Entity* blaze = s->spawnMob(ent::Blaze, 2.5, 100, 3.5);
    Entity* sk = s->spawnMob(ent::Skeleton, 10.5, 100, 3.5);
    CHECK(blaze && sk);
    if (!blaze || !sk) return;
    CHECK(blaze->fireImmune);
    CHECK(!sk->fireImmune);
    Entity* f = s->shootFireball(*blaze, blaze->x, blaze->y + 1.4, blaze->z, sk->x - blaze->x, 0, 0, ent::SmallFireball);
    CHECK(f);
    if (!f) return;
    CHECK(fabsf(f->width - 0.3125f) < 1e-6f);
    fly(*s, f);
    CHECK(f->removed);
    CHECK(sk->health == 15);   // 5 damage
    CHECK(sk->fireTicks > 0);  // and on fire
    // a blaze is not hurt by another's fireball (fire immune): it only stops it
    Entity* other = s->spawnMob(ent::Blaze, 7.5, 100, 3.5);
    CHECK(other);
    if (!other) return;
    f = s->shootFireball(*blaze, blaze->x, blaze->y + 1.4, blaze->z, 1, 0, 0, ent::SmallFireball);
    fly(*s, f);
    CHECK(f->removed);
    CHECK(other->health == 20);
    CHECK(other->fireTicks == 0);
    s->removeEntity(*other);
    s->removeEntity(*sk);
    s->removeEntity(*blaze);
}

TEST(small_fireball_sets_fire_where_it_lands) {
    Server* s = fortressServer();
    CHECK(s);
    if (!s) return;
    Server::InDim in(*s, DIM_NETHER);
    Entity* blaze = s->spawnMob(ent::Blaze, 12.5, 100, 5.5);
    CHECK(blaze);
    if (!blaze) return;
    // at the wall (x 20), just above the floor: fire in front of it, on the floor
    Entity* f = s->shootFireball(*blaze, 12.5, 100.2, 5.5, 1, 0, 0, ent::SmallFireball);
    CHECK(f);
    if (!f) return;
    fly(*s, f);
    CHECK(f->removed);
    CHECK_EQ(blockIdOf(s->blockAt(19, 100, 5)), blk::Fire);
    s->setBlock(19, 100, 5, bs::Air);
    s->removeEntity(*blaze);
}

TEST(fortress_mobs_are_fire_immune) {
    Server* s = fortressServer();
    CHECK(s);
    if (!s) return;
    Server::InDim in(*s, DIM_NETHER);
    Entity* ws = s->spawnMob(ent::WitherSkeleton, 4.5, 100, 1.5);
    CHECK(ws);
    if (!ws) return;
    CHECK(ws->hostile);
    CHECK(ws->fireImmune);
    CHECK(!ws->burnsInDay);
    CHECK(ws->health == 20);
    s->removeEntity(*ws);
}
