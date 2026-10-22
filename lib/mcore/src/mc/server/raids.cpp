// Illager patrols and raids, after vanilla 1.21 (PatrolSpawner, Raid, Raids, RaidOmen,
// OminousBottleItem): from the fifth day patrols of pillagers led by a captain come by day
// near players outside villages; a patrol captain killed by a player drops an ominous
// bottle; drunk, it gives Bad Omen; a player with Bad Omen in a village gets Raid Omen,
// and 30 s later the raid begins there: waves of pillagers, vindicators, witches, evokers
// and ravagers (Raid.RaiderType's numbers, by difficulty) come from 32 blocks away, with a
// boss bar; once all waves are beaten the players there get Hero of the Village (cheaper
// trades); with no villagers left the raid is lost. Witches throw potions, evokers summon
// fangs (no vexes). Raids are not saved (a restart ends them).
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "mc/registry.h"
#include "mc/server/mob_util.h"
#include "mc/server/server.h"
#include "mc/text.h"

namespace mc {

namespace {

bool isRaider(uint16_t t) {
    return t == ent::Pillager || t == ent::Vindicator || t == ent::Evoker || t == ent::Witch || t == ent::Ravager;
}

// Raid.RaiderType: how many of each in waves 1 to 7 (index 0 unused)
struct RaiderType { uint16_t type; uint8_t counts[8]; };
const RaiderType RAIDERS[] = {
    {ent::Vindicator, {0, 0, 2, 0, 1, 4, 2, 5}}, {ent::Evoker, {0, 0, 0, 0, 0, 1, 1, 2}},
    {ent::Pillager, {0, 4, 3, 3, 4, 4, 4, 2}},   {ent::Witch, {0, 0, 0, 0, 3, 0, 0, 1}},
    {ent::Ravager, {0, 0, 0, 1, 0, 1, 0, 2}},
};

double dist2(double ax, double az, double bx, double bz) { return (ax - bx) * (ax - bx) + (az - bz) * (az - bz); }

const uint8_t RAID_BAR_UUID[16] = {0x52, 0x41, 0x49, 0x44, 0x42, 0x41, 0x52, 0x00,
                                   0x52, 0x41, 0x49, 0x44, 0x42, 0x41, 0x52, 0x01};

}  // namespace

// ------------------------------------------------------------------ villages
// Is (x, z) in a village (Level#isVillage, simplified): a bell within 48 blocks, or two
// villagers with a bed or a workstation within 32.
bool Server::inVillage(uint8_t dim, double x, double y, double z) {
    int homes = 0;
    for (int i = 0; i < MC_MAX_ENTITIES; i++) {
        const Entity& o = entities[i];
        if (o.kind != EK_MOB || o.removed || o.type != ent::Villager || o.dim != dim) continue;
        if ((o.villager.hasHome || o.villager.hasJob) && dist2(o.x, o.z, x, z) < 32 * 32) homes++;
    }
    if (homes >= 2) return true;
    Entity probe = Entity();
    probe.x = x; probe.y = y; probe.z = z;
    probe.dim = dim;
    int bx, by, bz;
    return bellNear(probe, 48, bx, by, bz);
}

int Server::raidAt(uint8_t dim, double x, double z, double range) {
    for (int i = 0; i < MAX_RAIDS; i++) {
        const Raid& r = raids[i];
        if (r.active && r.dim == dim && dist2(r.cx + 0.5, r.cz + 0.5, x, z) < range * range) return i;
    }
    return -1;
}

// ------------------------------------------------------------------ omens
// OminousBottleItem#finishUsingItem: Bad Omen, its level the bottle's, 100 minutes.
void Server::drinkOminousBottle(Player& p, ItemStack& it, int slot) {
    int amp = ominousAmplifier(it);
    addEffect(p, eff::BadOmen, amp, 120000);
    playSound("item.ominous_bottle.dispose", p.e.x, p.e.y, p.e.z, 1, 1, 7);
    if (p.gamemode != GM_CREATIVE) {
        if (--it.count == 0) it.clear();
        p.sendSlot(slot);
    }
}

// Every second: a player with Bad Omen in a village gets Raid Omen (30 s) there instead
// (BadOmenMobEffect#applyEffectTick).
void Server::tickOmens(Player& p) {
    if (ticks % 20 != 0 || p.dead || !p.isSurvivalLike() || cfg.difficulty == 0) return;
    int bad = p.effect(eff::BadOmen);
    if (!bad || p.effect(eff::RaidOmen) || !inVillage(p.e.dim, p.e.x, p.e.y, p.e.z)) return;
    if (raidAt(p.e.dim, p.e.x, p.e.z, 96) >= 0) return;   // (a raid there already)
    removeEffect(p, eff::BadOmen);
    addEffect(p, eff::RaidOmen, bad - 1, 600);
    p.raidX = (int)floor(p.e.x);
    p.raidY = (int)floor(p.e.y);
    p.raidZ = (int)floor(p.e.z);
}

// RaidOmenMobEffect, when it ends: the raid starts where the player was.
void Server::raidOmenEnded(Player& p, int amp) {
    if (raidAt(p.e.dim, p.raidX, p.raidZ, 96) >= 0) return;
    for (int i = 0; i < MAX_RAIDS; i++) {
        Raid& r = raids[i];
        if (r.active) continue;
        r = Raid();
        r.active = true;
        r.dim = p.e.dim;
        r.cx = p.raidX; r.cy = p.raidY; r.cz = p.raidZ;
        r.omen = amp + 1;
        r.waves = cfg.difficulty == 1 ? 3 : cfg.difficulty == 2 ? 5 : 7;
        if (r.omen > 1) r.waves++;   // the bonus wave
        r.cooldown = 300;
        MC_LOGI("raid %d at %d %d %d: %d waves (omen %d)", i, (int)r.cx, (int)r.cy, (int)r.cz, r.waves, r.omen);
        return;
    }
}

// ------------------------------------------------------------------ the raid
// Raid#findRandomSpawnPos: 32 blocks from the centre, on the ground (16 if none).
bool Server::raidSpawnPos(const Raid& r, int& x, int& y, int& z) {
    Rng& rng = mobs::rng();
    for (int dist = 32; dist >= 16; dist -= 16)
        for (int t = 0; t < 20; t++) {
            float a = rng.unit() * 6.2831853f;
            int px = r.cx + (int)floorf(cosf(a) * dist), pz = r.cz + (int)floorf(sinf(a) * dist);
            if (!world.isResident(r.dim, px >> 4, pz >> 4)) continue;
            int py = world.heightAt(r.dim, px, pz);
            if (!stateCollides(world.getBlock(r.dim, px, py - 1, pz)) || stateIsFluid(world.getBlock(r.dim, px, py, pz))) continue;
            x = px; y = py; z = pz;
            return true;
        }
    return false;
}

// Raid#spawnGroup: the wave's raiders (Raid.RaiderType and the bonus spawns), the first a
// captain with the banner.
void Server::spawnRaidWave(int index) {
    Raid& r = raids[index];
    InDim in(*this, r.dim);
    r.wave++;
    int w = r.wave > 7 ? 7 : r.wave;
    bool easy = cfg.difficulty == 1, normal = cfg.difficulty == 2;
    bool bonusWave = r.omen > 1 && r.wave == r.waves;
    Rng& rng = mobs::rng();
    int x, y, z;
    if (!raidSpawnPos(r, x, y, z)) { x = r.cx; y = r.cy; z = r.cz; }
    r.waveHealth = 0;
    bool captain = false;
    for (const RaiderType& rt : RAIDERS) {
        int n = rt.counts[w];
        int bonus = 0;   // Raid#getPotentialBonusSpawns
        if (rt.type == ent::Witch) bonus = easy || r.wave <= 2 || r.wave == 4 ? 0 : 1;
        else if (rt.type == ent::Pillager || rt.type == ent::Vindicator) bonus = easy ? rng.range(2) : normal ? 1 : 2;
        else if (rt.type == ent::Ravager) bonus = !easy && bonusWave ? 1 : 0;
        n += bonus > 0 ? rng.range(bonus + 1) : 0;
        for (int k = 0; k < n; k++) {
            Entity* e = spawnMob(rt.type, x + 0.5 + rng.between(-2, 2), y, z + 0.5 + rng.between(-2, 2));
            if (!e) break;
            e->raid = (int8_t)index;
            e->persistent = true;
            if (!captain && (rt.type == ent::Pillager || rt.type == ent::Vindicator)) {
                e->captain = true;   // Raid#setLeader: the ominous banner on its head
                captain = true;
            }
            r.waveHealth += e->maxHealth;
        }
    }
    // the horn, for the players near the village
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& p = players[i];
        if (!p.inPlay() || p.e.dim != r.dim || dist2(p.e.x, p.e.z, r.cx, r.cz) > 96 * 96) continue;
        Packet pk(pkt::s2c::SoundEffect);
        pk.w.varint(0);
        pk.w.string("event.raid.horn");
        pk.w.boolean(true);
        pk.w.f32(64.0f);
        pk.w.varint(4);   // hostile
        pk.w.i32((int32_t)(p.e.x * 8 + (x - p.e.x) / 4));
        pk.w.i32((int32_t)(p.e.y * 8));
        pk.w.i32((int32_t)(p.e.z * 8 + (z - p.e.z) / 4));
        pk.w.f32(64.0f);
        pk.w.f32(1.0f);
        pk.w.i64((int64_t)rng.next());
        p.conn.send(pk);
    }
}

void Server::sendRaidBar(Player& p, int action, const Raid* r) {
    Packet pk(pkt::s2c::BossBar);
    pk.w.uuid(RAID_BAR_UUID);
    pk.w.varint(action);
    const char* title = !r ? "event.minecraft.raid" : r->status == Raid::VICTORY ? "event.minecraft.raid.victory.full"
                      : r->status == Raid::LOSS ? "event.minecraft.raid.defeat.full" : "event.minecraft.raid";
    float progress = r ? r->progress : 0;
    if (action == 0) {
        char json[96];
        snprintf(json, sizeof(json), "{\"translate\":\"%s\"}", title);
        writeTextNbt(pk.w, json);
        pk.w.f32(progress);
        pk.w.varint(2);   // red
        pk.w.varint(2);   // 10 notches
        pk.w.u8(0x04);    // fog
        p.raidBar = true;
    } else if (action == 2) {
        pk.w.f32(progress);
    } else if (action == 3) {
        char json[96];
        snprintf(json, sizeof(json), "{\"translate\":\"%s\"}", title);
        writeTextNbt(pk.w, json);
    } else {
        p.raidBar = false;
    }
    p.conn.send(pk);
}

// Raid#tick for each raid, every tick.
void Server::tickRaids() {
    for (int i = 0; i < MAX_RAIDS; i++) {
        Raid& r = raids[i];
        if (!r.active) continue;
        InDim in(*this, r.dim);
        r.age++;
        // the players there (the heroes, if it is won)
        bool anyone = false;
        for (int k = 0; k < MC_MAX_PLAYERS; k++) {
            Player& p = players[k];
            bool there = p.inPlay() && p.e.dim == r.dim && dist2(p.e.x, p.e.z, r.cx, r.cz) < 96 * 96;
            if (there) {
                anyone = true;
                if (p.isSurvivalLike()) r.heroes |= 1u << k;
            }
        }
        // the raiders left in it
        int alive = 0;
        float health = 0;
        for (int k = 0; k < MC_MAX_ENTITIES; k++) {
            const Entity& e = entities[k];
            if (e.kind == EK_MOB && !e.removed && e.health > 0 && e.raid == i) {
                alive++;
                health += e.health;
            }
        }
        if (r.status == Raid::ONGOING) {
            if (r.age > 48000 || (!anyone && r.age > 2400 && r.wave == 0)) {   // too long, or nobody came
                r.status = Raid::STOPPED;
            } else if (r.wave > 0 && ticks % 20 == 0) {   // lost: no villager left near it
                int villagers = 0;
                for (int k = 0; k < MC_MAX_ENTITIES; k++) {
                    const Entity& e = entities[k];
                    villagers += e.kind == EK_MOB && !e.removed && e.health > 0 && e.type == ent::Villager && e.dim == r.dim &&
                                 dist2(e.x, e.z, r.cx, r.cz) < 64 * 64;
                }
                if (!villagers) {
                    r.status = Raid::LOSS;
                    r.endTicks = 600;
                }
            }
        }
        if (r.status == Raid::ONGOING && alive == 0) {
            if (r.wave >= r.waves) {   // won: Hero of the Village for those who were there
                r.status = Raid::VICTORY;
                r.endTicks = 600;
                for (int k = 0; k < MC_MAX_PLAYERS; k++) {
                    Player& p = players[k];
                    if ((r.heroes >> k & 1) && p.inPlay() && !p.dead && p.e.dim == r.dim) {
                        addEffect(p, eff::HeroOfTheVillage, r.omen - 1, 48000);
                    }
                }
                playSound("entity.villager.celebrate", r.cx + 0.5, r.cy, r.cz + 0.5, 1, 1, 7);
            } else if (anyone) {   // the next wave after 15 s
                if (--r.cooldown <= 0) {
                    spawnRaidWave(i);
                    r.cooldown = 300;
                }
            }
        }
        // the bar: the wave's health left, or the countdown filling it
        if (r.status == Raid::ONGOING)
            r.progress = alive ? (r.waveHealth > 0 ? health / r.waveHealth : 0) : 1.0f - r.cooldown / 300.0f;
        else
            r.progress = r.status == Raid::VICTORY ? 1.0f : 0.0f;
        bool over = false;
        if (r.status != Raid::ONGOING && (r.status == Raid::STOPPED || --r.endTicks <= 0)) over = true;
        for (int k = 0; k < MC_MAX_PLAYERS; k++) {
            Player& p = players[k];
            if (!p.inPlay()) continue;
            bool want = !over && p.e.dim == r.dim && dist2(p.e.x, p.e.z, r.cx, r.cz) < 96 * 96;
            bool shown = p.raidBar && p.raidBarOf == i;
            if (want && !shown) {
                if (p.raidBar) sendRaidBar(p, 1, nullptr);
                p.raidBarOf = (int8_t)i;
                sendRaidBar(p, 0, &r);
                p.raidBarStatus = r.status;
            } else if (!want && shown) {
                sendRaidBar(p, 1, nullptr);
            } else if (shown && ticks % 10 == 0) {
                if (p.raidBarStatus != r.status) {
                    sendRaidBar(p, 3, &r);
                    p.raidBarStatus = r.status;
                }
                sendRaidBar(p, 2, &r);
            }
        }
        if (over) {
            for (int k = 0; k < MC_MAX_ENTITIES; k++)
                if (entities[k].raid == i) entities[k].raid = -1;
            r.active = false;
        }
    }
}

// ------------------------------------------------------------------ patrols
// PatrolSpawner#tick: every 10 to 11 minutes, by day from the fifth day, one in five: a
// patrol 24 to 48 blocks from a random player outside villages, its captain first.
void Server::tickPatrols() {
    if (!cfg.spawnMobs || cfg.difficulty == 0) return;
    if (patrolDelay == 0) patrolDelay = 12000 + mobs::rng().range(1200);
    if (--patrolDelay > 0) return;
    patrolDelay = 12000 + mobs::rng().range(1200);
    spawnPatrol(nullptr, false);
}

bool Server::spawnPatrol(Player* near, bool force) {
    Rng& rng = mobs::rng();
    if (!force) {
        int64_t day = meta.timeOfDay / 24000, t = meta.timeOfDay % 24000;
        if (day < 5 || t >= 12000 || rng.range(5) != 0) return false;
    }
    Player* cands[MC_MAX_PLAYERS];
    int n = 0;
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& p = players[i];
        if (p.inPlay() && !p.dead && p.gamemode != GM_SPECTATOR && p.e.dim == DIM_OVERWORLD) cands[n++] = &p;
    }
    Player* p = near ? near : (n ? cands[rng.range(n)] : nullptr);
    if (!p) return false;
    if (!force && inVillage(p->e.dim, p->e.x, p->e.y, p->e.z)) return false;
    InDim in(*this, p->e.dim);
    int dx = (24 + rng.range(24)) * (rng.range(2) ? -1 : 1), dz = (24 + rng.range(24)) * (rng.range(2) ? -1 : 1);
    int x = (int)floor(p->e.x) + dx, z = (int)floor(p->e.z) + dz;
    if (!world.isResident(p->e.dim, x >> 4, z >> 4)) return false;
    int count = cfg.difficulty + 1;
    Entity* captain = nullptr;
    float goalA = rng.unit() * 6.2831853f;
    for (int k = 0; k < count; k++) {
        int px = x + (k ? rng.between(-3, 3) : 0), pz = z + (k ? rng.between(-3, 3) : 0);
        if (!world.isResident(p->e.dim, px >> 4, pz >> 4)) continue;
        int py = world.heightAt(p->e.dim, px, pz);
        if (!stateCollides(world.getBlock(p->e.dim, px, py - 1, pz)) || stateIsFluid(world.getBlock(p->e.dim, px, py, pz))) continue;
        Entity* e = spawnMob(ent::Pillager, px + 0.5, py, pz + 0.5);
        if (!e) break;
        if (!captain) {
            captain = e;
            e->captain = true;
            e->goalX = (float)(px + cosf(goalA) * 300);   // PatrollingMonster: somewhere far
            e->goalZ = (float)(pz + sinf(goalA) * 300);
        }
        e->leader = captain->id;
    }
    return captain != nullptr;
}

// Where a raider walks when it has nobody to fight: a raid's village, its patrol's way
// (the captain) or its captain.
bool Server::raiderGoal(Entity& e, double& x, double& y, double& z, double& arrive) {
    if (e.raid >= 0 && e.raid < MAX_RAIDS && raids[e.raid].active) {
        const Raid& r = raids[e.raid];
        x = r.cx + 0.5; y = r.cy; z = r.cz + 0.5;
        arrive = 6;
        return true;
    }
    if (e.leader < 0) return false;
    if (e.leader == e.id) {
        x = e.goalX; y = e.y; z = e.goalZ;
        arrive = 4;
        return true;
    }
    Entity* c = findEntity(e.leader);
    if (!c || c->kind != EK_MOB || c->health <= 0) {
        e.leader = -1;
        return false;
    }
    x = c->x; y = c->y; z = c->z;
    arrive = 3;
    return true;
}

// ------------------------------------------------------------------ witches and evokers
// Witch#performRangedAttack: harming, poison, slowness or weakness by what the target lacks.
void Server::witchPotion(Entity& w, Entity& t) {
    double dx = t.x + t.vx - w.x, dz = t.z + t.vz - w.z;
    double dy = t.y + t.height * 0.85 - 1.1 - w.y;
    double dh = sqrt(dx * dx + dz * dz);
    int potionId = potion::Harming;
    Player* tp = t.kind == EK_PLAYER ? &players[t.playerSlot] : nullptr;
    if (dh >= 8 && !(tp && tp->effect(eff::Slowness))) potionId = potion::Slowness;
    else if (t.health >= 8 && !(tp && tp->effect(eff::Poison))) potionId = potion::Poison;
    else if (dh <= 3 && !(tp && tp->effect(eff::Weakness)) && mobs::rng().unit() < 0.25f) potionId = potion::Weakness;
    Entity* e = spawnEntity(EK_POTION, ent::SplashPotion, w.x, w.y + w.height - 0.1, w.z);
    if (!e) return;
    double vy = dy + dh * 0.2;
    double len = sqrt(dx * dx + vy * vy + dz * dz) + 1e-6;
    e->vx = dx / len * 0.75;
    e->vy = vy / len * 0.75;
    e->vz = dz / len * 0.75;
    e->owner = w.id;
    e->dim = w.dim;
    e->width = e->height = 0.25f;
    e->item = potionItem(itm::SplashPotion, potionId);
    e->velDirty = true;
    playSound("entity.witch.throw", w.x, w.y, w.z, 1, 0.8f + mobs::rng().unit() * 0.4f, 5);
}

// Evoker's fangs (EvokerAttackSpellGoal): sixteen in a line towards the target, each a
// tick later; a fang bites (6) 8 ticks after it shows.
void Server::evokerFangs(Entity& ev, Entity& t) {
    double dx = t.x - ev.x, dz = t.z - ev.z, d = sqrt(dx * dx + dz * dz) + 1e-6;
    double y0 = t.y < ev.y ? t.y : ev.y;
    for (int i = 0; i < 16; i++) {
        double h = 1.25 * (i + 1);
        double fx = ev.x + dx / d * h, fz = ev.z + dz / d * h;
        int bx = (int)floor(fx), bz = (int)floor(fz);
        int by = (int)floor(y0) + 1;
        bool ok = false;   // EvokerAttackSpellGoal#createSpellEntity: down to solid ground
        for (int k = 0; k < 4; k++, by--) {
            ok = stateCollides(blockAt(bx, by - 1, bz)) && !stateCollides(blockAt(bx, by, bz));
            if (ok) break;
        }
        if (!ok) continue;
        Entity* f = spawnEntity(EK_FANGS, ent::EvokerFangs, fx, by, fz);
        if (!f) break;
        f->owner = ev.id;
        f->fuse = (int16_t)i;   // the warm-up
        f->width = 0.5f;
        f->height = 0.8f;
        f->yaw = (float)(atan2(-dx, dz) * 180.0 / M_PI);
    }
    playSound("entity.evoker.prepare_attack", ev.x, ev.y, ev.z, 1, 1, 5);
}

void Server::tickFangs(Entity& f) {
    if (--f.fuse >= 0) return;
    if (f.fuse == -1) {
        broadcastStatus(f, 4);   // up it comes
        playSound("entity.evoker_fangs.attack", f.x, f.y, f.z, 1, 1, 5);
    }
    if (f.fuse == -8) {   // the bite: the living near it, not raiders
        for (int i = 0; i < MC_MAX_PLAYERS; i++) {
            Player& p = players[i];
            if (p.inPlay() && !p.dead && p.e.dim == f.dim && fabs(p.e.x - f.x) < 0.7 && fabs(p.e.z - f.z) < 0.7 && fabs(p.e.y - f.y) < 1)
                damagePlayer(p, 6, DC_MAGIC, f.owner);
        }
        for (int i = 0; i < MC_MAX_ENTITIES; i++) {
            Entity& m = entities[i];
            if (m.kind != EK_MOB || m.removed || m.health <= 0 || m.dim != f.dim || isRaider(m.type) || m.id == f.owner) continue;
            if (fabs(m.x - f.x) < 0.5 + m.width / 2 && fabs(m.z - f.z) < 0.5 + m.width / 2 && fabs(m.y - f.y) < 1)
                damageEntity(m, 6, DC_MAGIC, f.owner);
        }
    }
    if (f.fuse < -22) removeEntity(f);
}

bool isRaiderType(uint16_t type) { return isRaider(type); }

}  // namespace mc
