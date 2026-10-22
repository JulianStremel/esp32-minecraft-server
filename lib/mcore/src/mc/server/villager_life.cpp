// A villager's day, after vanilla 1.21's villager brain (Schedule.VILLAGER, VillagerGoalPackages):
// idle until 2000, at its workstation until 9000, at the village bell until 11000, idle,
// and from 12000 in its bed, asleep (the bed occupied, the sleeping pose). It claims a
// free bed near it as its home; farmers harvest and replant the ripe crops near their
// composter; villagers pick up food (bread, carrots, potatoes, beetroots) and the ones
// with plenty give some to the hungry; two grown-ups with food for it and a free bed
// near have a child (VillagerMakeLove), which grows up in 20 minutes.
#include <math.h>
#include <string.h>
#include "mc/registry.h"
#include "mc/server/mob_util.h"
#include "mc/server/server.h"

namespace mc {

namespace {

enum Activity { IDLE, WORK, MEET, REST };

// Schedule.VILLAGER (children: no work, no meeting)
Activity activityAt(int64_t timeOfDay, bool child) {
    int t = (int)(timeOfDay % 24000);
    if (t >= 12000 || t < 10) return REST;
    if (child) return IDLE;
    if (t >= 2000 && t < 9000) return WORK;
    if (t >= 9000 && t < 11000) return MEET;
    return IDLE;
}

bool isBedHead(uint16_t st) {
    uint16_t id = blockIdOf(st);
    if (id < blk::WhiteBed || id > blk::BlackBed) return false;
    const char* part = getPropStr(st, "part");
    return part && !strcmp(part, "head");
}

// FarmerWorkTask/HarvestFarmland: a crop at its last age
bool ripe(uint16_t st) {
    uint16_t id = blockIdOf(st);
    if (id == blk::Wheat || id == blk::Carrots || id == blk::Potatoes) return getProp(st, "age") == 7;
    if (id == blk::Beetroots) return getProp(st, "age") == 3;
    return false;
}

int foodValue(uint16_t item) {   // Villager.FOOD_POINTS
    if (item == itm::Bread) return 4;
    if (item == itm::Potato || item == itm::Carrot || item == itm::Beetroot) return 1;
    return 0;
}

double dist2(const Entity& a, double x, double y, double z) {
    double dx = a.x - x, dy = a.y - y, dz = a.z - z;
    return dx * dx + dy * dy + dz * dz;
}

}  // namespace

// The nearest block within rxz blocks across and ry up and down that `want` takes and no
// other villager holds (`taken`); the section palettes say where to look.
template <class Want, class Taken>
static bool nearestBlock(Server& s, const Entity& e, int rxz, int ry, Want want, Taken taken, int& bx, int& by, int& bz) {
    int ex = (int)floor(e.x), ey = (int)floor(e.y), ez = (int)floor(e.z);
    double best = 1e18;
    bool found = false;
    for (int cx = (ex - rxz) >> 4; cx <= (ex + rxz) >> 4; cx++)
        for (int cz = (ez - rxz) >> 4; cz <= (ez + rxz) >> 4; cz++) {
            Chunk* c = s.world.peek(e.dim, cx, cz);
            if (!c) continue;
            for (int y0 = (ey - ry) & ~15; y0 <= ey + ry; y0 += 16) {
                if (!c->hasY(y0)) continue;
                const Section* sec = c->section(c->sectionIndex(y0));
                if (!sec || !sec->anyState(want)) continue;
                for (int i = 0; i < SECTION_BLOCKS; i++) {
                    if (!want(sec->get(i))) continue;
                    int x = cx * 16 + (i & 15), y = y0 + (i >> 8), z = cz * 16 + ((i >> 4) & 15);
                    if (abs(x - ex) > rxz || abs(z - ez) > rxz || abs(y - ey) > ry) continue;
                    double d = dist2(e, x + 0.5, y, z + 0.5);
                    if (d >= best || taken(x, y, z)) continue;
                    best = d;
                    bx = x; by = y; bz = z;
                    found = true;
                }
            }
        }
    return found;
}

bool Server::bellNear(const Entity& e, int r, int& x, int& y, int& z) {
    return nearestBlock(*this, e, r, 16, [](uint16_t st) { return blockIdOf(st) == blk::Bell; },
                        [](int, int, int) { return false; }, x, y, z);
}

bool Server::bedTaken(const Entity& self, int x, int y, int z) {
    for (int k = 0; k < MC_MAX_ENTITIES; k++) {
        const Entity& o = entities[k];
        if (&o != &self && o.kind == EK_MOB && !o.removed && o.type == ent::Villager && o.dim == self.dim &&
            o.villager.hasHome && o.villager.homeX == x && o.villager.homeY == y && o.villager.homeZ == z)
            return true;
    }
    return false;
}

bool Server::freeBedNear(const Entity& e, int& x, int& y, int& z) {
    return nearestBlock(*this, e, 16, 4, isBedHead, [&](int bx, int by, int bz) { return bedTaken(e, bx, by, bz); }, x, y, z);
}

static void setOccupied(Server& s, int x, int y, int z, bool on) {
    uint16_t st = s.blockAt(x, y, z);
    if (isBedHead(st) && getBool(st, "occupied") != on) s.setBlock(x, y, z, setBool(st, "occupied", on));
}

void Server::wakeVillager(Entity& e) {
    if (e.pose != POSE_SLEEPING) return;
    e.pose = POSE_STANDING;
    e.metaDirty = true;
    const VillagerState& v = e.villager;
    if (v.hasHome) {
        setOccupied(*this, v.homeX, v.homeY, v.homeZ, false);
        // up beside the bed (LivingEntity#stopSleeping finds a place next to it)
        static const int DX[4] = {1, -1, 0, 0}, DZ[4] = {0, 0, 1, -1};
        for (int k = 0; k < 4; k++) {
            int x = v.homeX + DX[k], z = v.homeZ + DZ[k];
            if (!stateCollides(blockAt(x, v.homeY, z)) && !stateCollides(blockAt(x, v.homeY + 1, z))) {
                e.x = x + 0.5;
                e.z = z + 0.5;
                e.y = v.homeY;
                break;
            }
        }
    }
}

// Where the villager walks now: its bed, its workstation (a farmer: a ripe crop), the
// bell; false: nowhere in particular (it wanders). arrive: how near is there.
bool Server::villagerGoal(Entity& e, double& x, double& y, double& z, double& arrive) {
    VillagerState& v = e.villager;
    if (e.type == ent::WanderingTrader) {   // its goal (WanderToPositionGoal), else it wanders
        if (!v.hasWander) return false;
        x = v.wanderX + 0.5; y = v.wanderY; z = v.wanderZ + 0.5;
        arrive = 2;
        return true;
    }
    switch (activityAt(meta.timeOfDay, v.growAge < 0)) {
        case REST:
            if (!v.hasHome || e.pose == POSE_SLEEPING) return false;
            x = v.homeX + 0.5; y = v.homeY; z = v.homeZ + 0.5;
            arrive = 0.9;
            return true;
        case WORK:
            if (e.hasTask) {
                x = e.taskX + 0.5; y = e.taskY; z = e.taskZ + 0.5;
                arrive = 1.2;
                return true;
            }
            if (!v.hasJob) return false;
            x = v.jobX + 0.5; y = v.jobY; z = v.jobZ + 0.5;
            arrive = 1.8;
            return true;
        case MEET:
            if (!e.hasMeet) return false;
            x = e.meetX + 0.5; y = e.meetY; z = e.meetZ + 0.5;
            arrive = 3.5;
            return true;
        default: return false;
    }
}

// A villager's life each tick (called from tickVillager); true: asleep, it lies still.
bool Server::villagerLife(Entity& e) {
    VillagerState& v = e.villager;
    uint32_t phase = (uint32_t)e.id * 11u + e.age;
    // growing up
    if (v.growAge < 0 && ++v.growAge == 0) {
        e.width = ENTITY_TYPES[ent::Villager].width;
        e.height = ENTITY_TYPES[ent::Villager].height;
        e.metaDirty = true;
    }
    if (v.breedCooldown > 0) v.breedCooldown--;
    Activity act = activityAt(meta.timeOfDay, v.growAge < 0);
    // its bed: kept while it stands, a free one claimed now and then
    if (v.hasHome && phase % 40 == 0 && world.isResident(e.dim, v.homeX >> 4, v.homeZ >> 4) &&
        !isBedHead(world.getBlock(e.dim, v.homeX, v.homeY, v.homeZ))) {
        wakeVillager(e);
        v.hasHome = false;
    }
    if (!v.hasHome && phase % 100 == 75) {
        int x = 0, y = 0, z = 0;
        if (freeBedNear(e, x, y, z)) {
            v.hasHome = true;
            v.homeX = x; v.homeY = y; v.homeZ = z;
        }
    }
    // sleeping and waking (Villager#startSleeping, #stopSleeping)
    if (e.pose == POSE_SLEEPING) {
        if (act != REST || !v.hasHome || e.lastAttacker >= 0) {
            wakeVillager(e);
        } else {
            e.x = v.homeX + 0.5;
            e.y = v.homeY + 0.6875;
            e.z = v.homeZ + 0.5;
            e.vx = e.vy = e.vz = 0;
            return true;
        }
    } else if (act == REST && v.hasHome && e.lastAttacker < 0) {
        double dx = v.homeX + 0.5 - e.x, dz = v.homeZ + 0.5 - e.z;
        uint16_t bed = blockAt(v.homeX, v.homeY, v.homeZ);
        bool player = false;   // (its own bed shows occupied after a restart: only a player in it counts)
        for (int i = 0; i < MC_MAX_PLAYERS; i++) {
            const Player& p = players[i];
            player |= p.inPlay() && p.sleeping && p.sleepX == v.homeX && p.sleepY == v.homeY && p.sleepZ == v.homeZ;
        }
        if (dx * dx + dz * dz < 1.2 * 1.2 && fabs(e.y - v.homeY) < 1.5 && isBedHead(bed) && !player) {
            e.pose = POSE_SLEEPING;
            e.metaDirty = true;
            setOccupied(*this, v.homeX, v.homeY, v.homeZ, true);
            return true;
        }
    }
    // the bell to meet at (looked for once a while, 32 blocks around)
    if (act == MEET && phase % 200 == 100) {
        int x = 0, y = 0, z = 0;
        e.hasMeet = bellNear(e, 32, x, y, z);
        if (e.hasMeet) { e.meetX = x; e.meetY = y; e.meetZ = z; }
    }
    // food lying near it
    if (phase % 10 == 5 && v.food < 48) {
        for (int i = 0; i < MC_MAX_ENTITIES; i++) {
            Entity& it = entities[i];
            if (it.kind != EK_ITEM || it.removed || it.dim != e.dim || it.pickupDelay > 0 || !foodValue(it.item.id)) continue;
            if (dist2(e, it.x, it.y, it.z) > 1.5 * 1.5 + 1) continue;
            int take = 0;
            while (take < it.item.count && v.food + foodValue(it.item.id) <= 64) {
                v.food = (uint8_t)(v.food + foodValue(it.item.id));
                take++;
            }
            it.item.count = (uint8_t)(it.item.count - take);
            if (take) {
                Packet pk(pkt::s2c::Collect);   // it picks it up
                pk.w.varint(it.id);
                pk.w.varint(e.id);
                pk.w.varint(take);
                broadcastNear(pk, (int)floor(e.x) >> 4, (int)floor(e.z) >> 4, nullptr);
            }
            if (!it.item.count) removeEntity(it);
        }
    }
    // a farmer's harvest near its composter (HarvestFarmland): the crop, replanted
    if (v.profession == 5 && v.hasJob && act == WORK) {
        if (e.hasTask && !ripe(blockAt(e.taskX, e.taskY, e.taskZ))) e.hasTask = false;
        if (!e.hasTask && phase % 40 == 20) {
            Entity probe = Entity();
            probe.x = v.jobX + 0.5; probe.y = v.jobY; probe.z = v.jobZ + 0.5;
            probe.dim = e.dim;
            int x = 0, y = 0, z = 0;
            e.hasTask = nearestBlock(*this, probe, 8, 2, ripe, [](int, int, int) { return false; }, x, y, z);
            if (e.hasTask) { e.taskX = x; e.taskY = y; e.taskZ = z; }
        }
        if (e.hasTask && dist2(e, e.taskX + 0.5, e.taskY, e.taskZ + 0.5) < 1.6 * 1.6 + 1) {
            uint16_t st = blockAt(e.taskX, e.taskY, e.taskZ);
            uint16_t id = blockIdOf(st);
            int gain = id == blk::Wheat ? 1 : id == blk::Beetroots ? 1 : 1 + mobs::rng().range(4);
            v.food = (uint8_t)(v.food + gain > 64 ? 64 : v.food + gain);
            Packet pk(pkt::s2c::WorldEvent);   // broken (the particles and sound)
            pk.w.i32(2001);
            pk.w.u64(packPos(e.taskX, e.taskY, e.taskZ));
            pk.w.i32(st);
            pk.w.boolean(false);
            broadcastNear(pk, e.taskX >> 4, e.taskZ >> 4, nullptr);
            setBlock(e.taskX, e.taskY, e.taskZ, setProp(st, "age", 0));   // and planted again
            e.hasTask = false;
        }
    } else {
        e.hasTask = false;
    }
    // sharing (TradeWithVillager): one with plenty gives some to a hungry one near it
    if (phase % 100 == 50 && v.food >= 24) {
        for (int i = 0; i < MC_MAX_ENTITIES; i++) {
            Entity& o = entities[i];
            if (&o == &e || o.kind != EK_MOB || o.removed || o.type != ent::Villager || o.dim != e.dim || o.villager.food >= 12)
                continue;
            if (dist2(e, o.x, o.y, o.z) > 4 * 4) continue;
            v.food = (uint8_t)(v.food - 8);
            o.villager.food = (uint8_t)(o.villager.food + 8);
            break;
        }
    }
    // a child: two grown-ups with 12 food each, not tired of it, a free bed near
    if (phase % 100 == 25 && v.growAge >= 0 && v.food >= 12 && v.breedCooldown == 0 && act != REST && e.tradingWith < 0) {
        for (int i = 0; i < MC_MAX_ENTITIES; i++) {
            Entity& o = entities[i];
            if (&o == &e || o.kind != EK_MOB || o.removed || o.health <= 0 || o.type != ent::Villager || o.dim != e.dim) continue;
            const VillagerState& w = o.villager;
            if (w.growAge < 0 || w.food < 12 || w.breedCooldown > 0 || o.pose == POSE_SLEEPING || dist2(e, o.x, o.y, o.z) > 8 * 8)
                continue;
            int bx = 0, by = 0, bz = 0;
            if (!freeBedNear(e, bx, by, bz)) break;
            Entity* child = spawnMob(ent::Villager, (e.x + o.x) / 2, e.y, (e.z + o.z) / 2);
            if (!child) break;
            v.food = (uint8_t)(v.food - 12);
            o.villager.food = (uint8_t)(o.villager.food - 12);
            v.breedCooldown = o.villager.breedCooldown = 6000;
            child->villager.growAge = -24000;
            child->villager.hasHome = true;   // the free bed: the child's
            child->villager.homeX = bx; child->villager.homeY = by; child->villager.homeZ = bz;
            child->variant = e.variant;
            child->width = ENTITY_TYPES[ent::Villager].width * 0.5f;
            child->height = ENTITY_TYPES[ent::Villager].height * 0.5f;
            child->metaDirty = true;
            broadcastStatus(e, 12);   // hearts
            broadcastStatus(o, 12);
            playSound("entity.villager.celebrate", e.x, e.y, e.z, 1, 1, 5);
            break;
        }
    }
    return false;
}

}  // namespace mc
