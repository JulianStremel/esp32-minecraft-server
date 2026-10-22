// The End's mobs without clients: shulkers' shells, endermen dodging projectiles.
#include <math.h>
#include "testing.h"
#include "mc/registry.h"
#include "mc/server/mob_util.h"
#include "mc/server/server.h"

using namespace mc;

static Server* endMobServer() {
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

TEST(shulker_shell_is_armour_while_shut) {
    Server* s = endMobServer();
    CHECK(s);
    if (!s) return;
    Server::InDim in(*s, DIM_END);
    int y = s->world.heightAt(DIM_END, 30, 30);
    Entity* sh = s->spawnMob(ent::Shulker, 30.5, y, 30.5);
    CHECK(sh);
    if (!sh) return;
    CHECK_EQ(sh->size, 0);   // shut
    // arrows bounce off, a sword hit loses most of its damage
    CHECK(!s->damageEntity(*sh, 9, DC_ARROW, -1));
    CHECK(sh->health == 30);
    CHECK(fabsf(mobs::shulkerDamage(*sh, 7, DC_ATTACK) - 7 * (1 - 16.5f / 25)) < 1e-4f);
    sh->size = 20;   // open
    CHECK(mobs::shulkerDamage(*sh, 7, DC_ARROW) == 7);
    CHECK(s->damageEntity(*sh, 9, DC_ARROW, -1));
    CHECK(sh->health == 21);
    s->removeEntity(*sh);
}

TEST(enderman_dodges_projectiles) {
    Server* s = endMobServer();
    CHECK(s);
    if (!s) return;
    Server::InDim in(*s, DIM_END);
    int y = s->world.heightAt(DIM_END, 20, -20);
    Entity* em = s->spawnMob(ent::Enderman, 20.5, y, -20.5);
    CHECK(em);
    if (!em) return;
    CHECK(em->hostile);
    double x0 = em->x, z0 = em->z;
    CHECK(!s->damageEntity(*em, 6, DC_ARROW, -1));   // no damage: it teleported away
    CHECK(em->health == 40);
    double moved = sqrt((em->x - x0) * (em->x - x0) + (em->z - z0) * (em->z - z0));
    printf("    an arrow: teleported %.1f blocks to %.1f %.1f %.1f\n", moved, em->x, em->y, em->z);
    CHECK(moved > 0.5);
    // standing on something, room for its 3 blocks
    int bx = (int)floor(em->x), by = (int)floor(em->y), bz = (int)floor(em->z);
    CHECK(stateCollides(s->world.getBlock(DIM_END, bx, by - 1, bz)));
    for (int dy = 0; dy < 3; dy++) CHECK(!stateCollides(s->world.getBlock(DIM_END, bx, by + dy, bz)));
    s->removeEntity(*em);
}
