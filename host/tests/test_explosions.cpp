// Explosions as vanilla's ServerExplosion: rays weakened by blast resistance, drops by
// kind, damage by the share of an entity the blast sees, chain reactions.
#include <math.h>
#include <memory>
#include "testing.h"
#include "mc/platform.h"
#include "mc/registry.h"
#include "mc/server/server.h"

using namespace mc;

namespace {
struct BlastWorld {
    std::unique_ptr<Server> s{new Server};
    explicit BlastWorld(uint16_t fill = bs::Dirt) {
        ServerConfig cfg;
        cfg.port = 0;
        cfg.worldType = WORLD_FLAT;
        cfg.spawnMobs = false;
        cfg.seed = 1;
        cfg.workerThreads = 0;
        CHECK(s->begin(cfg, nullptr));
        // a block of `fill` from y 60 to 79, air above
        for (int x = -12; x <= 12; x++)
            for (int z = -12; z <= 12; z++) {
                for (int y = 60; y < 80; y++) s->world.setBlock(0, x, y, z, fill, false);
                for (int y = 80; y < 100; y++) s->world.setBlock(0, x, y, z, bs::Air, false);
            }
    }
    int count(uint16_t id, int r = 10) {
        int n = 0;
        for (int x = -r; x <= r; x++)
            for (int z = -r; z <= r; z++)
                for (int y = 60; y < 100; y++) n += blockIdOf(s->blockAt(x, y, z)) == id;
        return n;
    }
    int items() {
        int n = 0;
        for (int i = 0; i < MC_MAX_ENTITIES; i++) {
            const Entity& e = s->entities[i];
            if (e.kind == EK_ITEM && !e.removed) n += e.item.count;
        }
        return n;
    }
};
}  // namespace

TEST(tnt_craters_dirt_and_drops_every_block) {
    BlastWorld w;
    int before = w.count(blk::Dirt);
    uint64_t t0 = plat::micros();
    w.s->explode(0.5, 80.0, 0.5, 4.0f, -1, false, Server::EXPLODE_TNT);
    uint32_t us = (uint32_t)(plat::micros() - t0);
    int gone = before - w.count(blk::Dirt);
    int dropped = w.items();
    printf("    TNT on dirt: %d blocks destroyed, %d items, %u us on the PC\n", gone, dropped, us);
    CHECK(gone > 30 && gone < 300);
    // every destroyed block drops (tntExplosionDropDecay is off), as far as the 128
    // entities hold them
    CHECK(dropped >= std::min(gone, 100));
}

TEST(mob_explosions_drop_one_in_power) {
    BlastWorld w;
    int before = w.count(blk::Dirt);
    w.s->explode(0.5, 80.0, 0.5, 4.0f, -1, false, Server::EXPLODE_MOB);
    int gone = before - w.count(blk::Dirt);
    int dropped = w.items();
    printf("    a creeper-sized blast: %d destroyed, %d dropped\n", gone, dropped);
    CHECK(gone > 30);
    CHECK(dropped < gone / 2);   // about a quarter
}

TEST(tnt_under_water_breaks_nothing_and_obsidian_holds) {
    BlastWorld water(bs::Water);
    int before = water.count(blk::Water);
    water.s->explode(0.5, 70.0, 0.5, 4.0f, -1, false, Server::EXPLODE_TNT);
    CHECK_EQ(water.count(blk::Water), before);
    BlastWorld obsidian(bs::Obsidian);
    int ob = obsidian.count(blk::Obsidian);
    obsidian.s->explode(0.5, 80.0, 0.5, 6.0f, -1, false, Server::EXPLODE_TNT);
    CHECK_EQ(obsidian.count(blk::Obsidian), ob);
    // stone (resistance 6) loses much less than dirt (0.5)
    BlastWorld stone(bs::Stone);
    int st = stone.count(blk::Stone);
    stone.s->explode(0.5, 80.0, 0.5, 4.0f, -1, false, Server::EXPLODE_TNT);
    int stoneGone = st - stone.count(blk::Stone);
    printf("    TNT on stone: %d destroyed\n", stoneGone);
    CHECK(stoneGone > 0 && stoneGone < 40);
}

TEST(explosions_hurt_what_they_see) {
    BlastWorld w(bs::Stone);
    Entity* open = w.s->spawnMob(ent::Zombie, 3.5, 80, 0.5);
    Entity* hidden = w.s->spawnMob(ent::Zombie, -3.5, 80, 0.5);
    CHECK(open && hidden);
    if (!open || !hidden) return;
    for (int y = 80; y < 84; y++)
        for (int z = -3; z <= 3; z++) w.s->setBlock(-2, y, z, bs::Obsidian);   // a wall between it and the blast
    float h0 = open->health, h1 = hidden->health;
    w.s->explode(0.5, 80.0, 0.5, 4.0f, -1, false, Server::EXPLODE_TNT);
    float lostOpen = h0 - open->health, lostHidden = h1 - hidden->health;
    printf("    zombie in the open lost %.1f, behind obsidian %.1f\n", lostOpen, lostHidden);
    CHECK(lostOpen > 8);
    CHECK(lostHidden <= 1.01f);   // it sees nothing of the blast: the 1 every hit does
    CHECK(open->vx > 0.1);        // pushed away
}

TEST(explosions_prime_tnt_and_destroy_items) {
    BlastWorld w(bs::Stone);
    w.s->setBlock(2, 80, 0, bs::Tnt);
    Entity* item = w.s->dropItem(-1.5, 80.2, 0.5, ItemStack::of(itm::Diamond, 1), false);
    CHECK(item != nullptr);
    w.s->explode(0.5, 80.0, 0.5, 4.0f, -1, false, Server::EXPLODE_TNT);
    int tnt = 0, fuse = -1;
    for (int i = 0; i < MC_MAX_ENTITIES; i++) {
        const Entity& e = w.s->entities[i];
        if (e.kind == EK_TNT && !e.removed) { tnt++; fuse = e.fuse; }
    }
    CHECK_EQ(tnt, 1);
    CHECK(fuse >= 10 && fuse < 30);   // a chain reaction's short fuse
    CHECK(item && item->removed);
}

TEST(explosions_reach_the_edge_of_their_range) {
    BlastWorld w(bs::Stone);
    Entity* far = w.s->spawnMob(ent::Zombie, 6.5, 80, 0.5);
    Entity* side = w.s->spawnMob(ent::Zombie, 0.5, 80, -6.5);
    CHECK(far && side);
    if (!far || !side) return;
    float h0 = far->health, h1 = side->health;
    w.s->explode(0.5, 80.06, 0.5, 4.0f, -1, false, Server::EXPLODE_TNT);
    printf("    6 blocks east lost %.1f, 7 blocks north lost %.1f\n", h0 - far->health, h1 - side->health);
    CHECK(h0 - far->health > 1.5f);
    CHECK(h1 - side->health > 1.0f);
}

// A blast at the corner of four chunks destroys blocks in all four (reported from play:
// only two of them).
TEST(explosions_at_a_chunk_corner_reach_all_four_chunks) {
    BlastWorld w;
    int before[4] = {};
    auto quadrant = [](int x, int z) { return (x < 0 ? 0 : 1) + (z < 0 ? 0 : 2); };
    for (int x = -10; x <= 9; x++)
        for (int z = -10; z <= 9; z++)
            for (int y = 60; y < 80; y++) before[quadrant(x, z)] += blockIdOf(w.s->blockAt(x, y, z)) == blk::Dirt;
    w.s->explode(0.0, 80.0, 0.0, 4.0f, -1, false, Server::EXPLODE_TNT);   // exactly on the corner
    int gone[4] = {};
    for (int x = -10; x <= 9; x++)
        for (int z = -10; z <= 9; z++)
            for (int y = 60; y < 80; y++) gone[quadrant(x, z)] += blockIdOf(w.s->blockAt(x, y, z)) != blk::Dirt;
    printf("    destroyed per chunk: %d %d %d %d\n", gone[0], gone[1], gone[2], gone[3]);
    for (int q = 0; q < 4; q++) CHECK(gone[q] > 10);
    (void)before;
}
