// A villager's day without a client: claiming a bed and sleeping in it at night, where it
// walks by the time of day, picking up food, a farmer's harvest, sharing, a child and its
// growing up, and its life saved with its chunk.
#include "testing.h"
#include "mc/item.h"
#include "mc/registry.h"
#include "mc/server/server.h"
#include "mc/storage/block_device.h"
#include "mc/storage/world_store.h"

using namespace mc;

namespace {

Server* server() {
    static Server* s = nullptr;
    if (s) return s;
    s = new Server();
    ServerConfig cfg;
    cfg.port = 0;
    cfg.worldType = WORLD_FLAT;
    cfg.spawnMobs = false;
    cfg.seed = 44;
    if (!s->begin(cfg, nullptr)) return nullptr;
    for (int cx = 0; cx < 3; cx++)
        for (int cz = 0; cz < 3; cz++) s->world.load(DIM_OVERWORLD, cx, cz);
    for (int x = 0; x < 40; x++)
        for (int z = 0; z < 40; z++) s->setBlock(x, 69, z, bs::Stone);
    return s;
}

void clearMobs(Server& s) {
    for (int i = 0; i < MC_MAX_ENTITIES; i++)
        if (s.entities[i].kind != EK_NONE) s.entities[i].kind = EK_NONE;
}

void live(Server& s, Entity& v, int ticks) {
    for (int i = 0; i < ticks; i++) {
        v.age++;
        s.tickVillager(v);
    }
}

uint16_t bedHead() { return setPropStr(bs::RedBed, "part", "head"); }

int count(Server& s, uint16_t type) {
    int n = 0;
    for (int i = 0; i < MC_MAX_ENTITIES; i++)
        n += s.entities[i].kind == EK_MOB && !s.entities[i].removed && s.entities[i].type == type;
    return n;
}

}  // namespace

TEST(villager_sleeps_in_its_bed_at_night) {
    Server* s = server();
    CHECK(s);
    if (!s) return;
    Server::InDim in(*s, DIM_OVERWORLD);
    clearMobs(*s);
    s->setBlock(10, 70, 10, bedHead());
    s->setBlock(10, 70, 11, setPropStr(bs::RedBed, "part", "foot"));
    Entity* v = s->spawnMob(ent::Villager, 12.5, 70, 10.5);
    CHECK(v);
    if (!v) return;
    s->meta.timeOfDay = 13000;   // night: rest
    live(*s, *v, 120);
    CHECK(v->villager.hasHome);
    CHECK_EQ(v->villager.homeX, 10);
    double gx, gy, gz, arrive;
    CHECK(s->villagerGoal(*v, gx, gy, gz, arrive));
    CHECK(gx == 10.5 && gz == 10.5);
    v->x = 10.6; v->z = 10.4;   // (as if it had walked there)
    live(*s, *v, 1);
    CHECK_EQ(v->pose, POSE_SLEEPING);
    CHECK(getBool(s->blockAt(10, 70, 10), "occupied"));
    live(*s, *v, 50);
    CHECK_EQ(v->pose, POSE_SLEEPING);
    s->meta.timeOfDay = 24000 + 100;   // morning
    live(*s, *v, 1);
    CHECK_EQ(v->pose, POSE_STANDING);
    CHECK(!getBool(s->blockAt(10, 70, 10), "occupied"));
    // a second villager does not take that bed
    Entity* w = s->spawnMob(ent::Villager, 12.5, 70, 12.5);
    CHECK(w);
    if (w) {
        live(*s, *w, 120);
        CHECK(!w->villager.hasHome);
    }
    // the bed broken: no home
    s->setBlock(10, 70, 10, 0);
    live(*s, *v, 50);
    CHECK(!v->villager.hasHome);
    clearMobs(*s);
}

TEST(villager_goes_where_its_day_says) {
    Server* s = server();
    CHECK(s);
    if (!s) return;
    Server::InDim in(*s, DIM_OVERWORLD);
    clearMobs(*s);
    Entity* v = s->spawnMob(ent::Villager, 20.5, 70, 20.5);
    CHECK(v);
    if (!v) return;
    v->villager.profession = 9;
    v->villager.hasJob = true;
    v->villager.jobX = 25; v->villager.jobY = 70; v->villager.jobZ = 20;
    v->villager.hasHome = true;
    v->villager.homeX = 15; v->villager.homeY = 70; v->villager.homeZ = 20;
    double gx, gy, gz, arrive;
    s->meta.timeOfDay = 5000;
    CHECK(s->villagerGoal(*v, gx, gy, gz, arrive));
    CHECK(gx == 25.5);   // work
    s->meta.timeOfDay = 15000;
    CHECK(s->villagerGoal(*v, gx, gy, gz, arrive));
    CHECK(gx == 15.5);   // bed
    s->meta.timeOfDay = 1000;
    CHECK(!s->villagerGoal(*v, gx, gy, gz, arrive));   // idle: it wanders
    s->setBlock(20, 70, 30, bs::Bell);
    s->meta.timeOfDay = 10000;
    live(*s, *v, 220);
    CHECK(s->villagerGoal(*v, gx, gy, gz, arrive));
    CHECK(gx == 20.5 && gz == 30.5);   // the bell
    s->setBlock(20, 70, 30, 0);
    clearMobs(*s);
}

TEST(villagers_eat_farm_share_and_have_children) {
    Server* s = server();
    CHECK(s);
    if (!s) return;
    Server::InDim in(*s, DIM_OVERWORLD);
    clearMobs(*s);
    s->meta.timeOfDay = 5000;   // work
    // food on the ground: picked up
    Entity* v = s->spawnMob(ent::Villager, 5.5, 70, 5.5);
    CHECK(v);
    if (!v) return;
    Entity* bread = s->dropItem(5.5, 70.2, 5.5, ItemStack::of(itm::Bread, 3), false);
    CHECK(bread);
    if (bread) bread->pickupDelay = 0;   // (only the villager ticks here)
    live(*s, *v, 20);
    CHECK_EQ(v->villager.food, 12);
    // a farmer by its composter harvests a ripe crop near it, and plants it again
    Entity* f = s->spawnMob(ent::Villager, 30.5, 70, 30.5);
    CHECK(f);
    if (!f) return;
    s->setBlock(30, 70, 33, bs::Composter);
    s->setBlock(32, 69, 31, bs::Farmland);
    s->setBlock(32, 70, 31, setProp(bs::Wheat, "age", 7));
    f->villager.profession = 5;
    f->villager.hasJob = true;
    f->villager.jobX = 30; f->villager.jobY = 70; f->villager.jobZ = 33;
    live(*s, *f, 60);
    CHECK(f->hasTask);
    f->x = 32.5; f->z = 31.5;   // (there)
    live(*s, *f, 2);
    CHECK_EQ(getProp(s->blockAt(32, 70, 31), "age"), 0);
    CHECK(f->villager.food >= 1);
    // one with plenty gives to a hungry one near it
    f->villager.food = 30;
    Entity* h = s->spawnMob(ent::Villager, 33.5, 70, 31.5);
    CHECK(h);
    if (h) {
        live(*s, *f, 100);
        CHECK_EQ(h->villager.food, 8);
        CHECK_EQ(f->villager.food, 22);
        h->removed = true;
        h->kind = EK_NONE;
    }
    f->kind = EK_NONE;
    // two grown-ups with food and a free bed: a child
    s->setBlock(8, 70, 8, bedHead());
    s->setBlock(8, 70, 9, setPropStr(bs::RedBed, "part", "foot"));
    Entity* w = s->spawnMob(ent::Villager, 6.5, 70, 5.5);
    CHECK(w);
    if (!w) return;
    v->villager.food = 20;
    w->villager.food = 20;
    int before = count(*s, ent::Villager);
    live(*s, *v, 100);
    CHECK_EQ(count(*s, ent::Villager), before + 1);
    CHECK_EQ(v->villager.food, 8);
    CHECK_EQ(w->villager.food, 8);
    CHECK(v->villager.breedCooldown > 5800 && w->villager.breedCooldown > 5800);
    Entity* child = nullptr;
    for (int i = 0; i < MC_MAX_ENTITIES; i++)
        if (s->entities[i].kind == EK_MOB && s->entities[i].type == ent::Villager && s->entities[i].villager.growAge < 0)
            child = &s->entities[i];
    CHECK(child);
    if (!child) return;
    CHECK(child->villager.hasHome && child->villager.homeX == 8);
    CHECK(child->width < 0.5f);
    // a child takes no job and has no trades; it grows up in 20 minutes
    s->setBlock(child->x > 0 ? (int)child->x + 2 : 2, 70, (int)child->z, bs::Lectern);
    live(*s, *child, 200);
    CHECK_EQ(child->villager.profession, 0);
    child->villager.growAge = -1;
    live(*s, *child, 1);
    CHECK_EQ(child->villager.growAge, 0);
    CHECK(child->width > 0.5f);
    clearMobs(*s);
}

TEST(villager_life_saved_with_its_chunk) {
    MemDevice dev(32 * 1024 * 1024);
    WorldStore store(&dev);
    StoreParams params;
    params.radius = 1;
    params.playerSlots = 4;
    CHECK(store.open(params));
    Chunk c(0, 0);
    SavedEntity e;
    e.kind = SAVED_KIND_MOB;
    e.type = ent::Villager;
    e.x = 3.5; e.y = 70; e.z = 3.5;
    e.health = 20;
    e.villager.hasHome = true;
    e.villager.homeX = 7; e.villager.homeY = 71; e.villager.homeZ = -3;
    e.villager.food = 17;
    e.villager.breedCooldown = 4000;
    e.villager.growAge = -12000;
    CHECK(c.addEntity(e));
    CHECK(store.saveChunk(c));
    Chunk loaded(0, 0);
    CHECK_EQ(store.loadChunk(loaded), LOAD_OK);
    CHECK_EQ(loaded.entCount, 1);
    if (loaded.entCount != 1) return;
    const VillagerState& v = loaded.ents[0].villager;
    CHECK(v.hasHome);
    CHECK_EQ(v.homeZ, -3);
    CHECK_EQ(v.food, 17);
    CHECK_EQ(v.breedCooldown, 4000);
    CHECK_EQ(v.growAge, -12000);
}
