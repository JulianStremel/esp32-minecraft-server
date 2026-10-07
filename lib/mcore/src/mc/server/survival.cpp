// Player health, hunger, environmental damage, death and respawn, experience.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "mc/registry.h"
#include "mc/server/server.h"

namespace mc {

static int armorPoints(const Player& p) {
    int a = 0;
    for (int i = SLOT_ARMOR_START; i < SLOT_ARMOR_START + 4; i++)
        if (!p.inv[i].empty()) a += ITEMS[p.inv[i].id].armor;
    return a;
}

static bool armorApplies(uint8_t cause) {
    return cause == DC_ATTACK || cause == DC_ARROW || cause == DC_EXPLOSION || cause == DC_CACTUS || cause == DC_GENERIC;
}

void Server::damagePlayer(Player& p, float amount, uint8_t cause, int32_t attacker) {
    if (!p.inPlay() || p.dead || amount <= 0) return;
    if ((p.gamemode == GM_CREATIVE || p.gamemode == GM_SPECTATOR) && cause != DC_VOID && cause != DC_KILL) return;
    if (cfg.difficulty == 0 && cause == DC_ATTACK && playerByEntity(attacker) == nullptr) return;
    // damage immunity: only the part above the last hit applies
    float& last = p.e.damage;
    if (p.e.invuln > 0 && cause != DC_KILL && cause != DC_VOID) {
        if (amount <= last) return;
        float extra = amount - last;
        last = amount;
        amount = extra;
    } else {
        last = amount;
        p.e.invuln = 10;
    }
    if (armorApplies(cause)) {
        int armor = armorPoints(p);
        if (armor > 0) {
            float red = fmaxf(armor / 5.0f, armor - amount / 2.0f);
            if (red > 20) red = 20;
            amount *= 1 - red / 25.0f;
            // wear the armour
            int wear = (int)(last / 4);
            if (wear < 1) wear = 1;
            for (int i = SLOT_ARMOR_START; i < SLOT_ARMOR_START + 4; i++) {
                ItemStack& a = p.inv[i];
                if (a.empty() || !ITEMS[a.id].durability) continue;
                a.damage = (uint16_t)(a.damage + wear);
                if (a.damage >= ITEMS[a.id].durability) {
                    a.clear();
                    playSound("entity.item.break", p.e.x, p.e.y, p.e.z, 1, 1, 7);
                }
                p.sendSlot(i);
            }
            broadcastEquipment(p);
        }
    }
    p.e.health -= amount;
    p.healthDirty = true;
    p.e.lastAttacker = attacker;
    broadcastStatus(p.e, 2);
    addExhaustion(p, 0.1f);
    if (p.e.health <= 0) killPlayer(p, cause, attacker);
    else p.sendHealth();
}

static const char* entityName(uint16_t type) {
    switch (type) {
        case ent::Zombie: return "Zombie";
        case ent::Skeleton: return "Skeleton";
        case ent::Creeper: return "Creeper";
        case ent::Spider: return "Spider";
        default: return "a mob";
    }
}

void Server::killPlayer(Player& p, uint8_t cause, int32_t attacker) {
    if (p.dead) return;
    p.dead = true;
    p.e.health = 0;
    p.e.deathTicks = 0;
    p.usingTicks = 0;
    closeWindow(p, true);
    char killer[40] = "";
    Player* kp = playerByEntity(attacker);
    Entity* ke = kp ? nullptr : findEntity(attacker);
    if (kp) snprintf(killer, sizeof(killer), "%s", kp->name);
    else if (ke) snprintf(killer, sizeof(killer), "%s", entityName(ke->type));
    char msg[160];
    switch (cause) {
        case DC_FALL: snprintf(msg, sizeof(msg), "%s fell from a high place", p.name); break;
        case DC_VOID: snprintf(msg, sizeof(msg), "%s fell out of the world", p.name); break;
        case DC_DROWN: snprintf(msg, sizeof(msg), "%s drowned", p.name); break;
        case DC_LAVA: snprintf(msg, sizeof(msg), "%s tried to swim in lava", p.name); break;
        case DC_FIRE: snprintf(msg, sizeof(msg), "%s burned to death", p.name); break;
        case DC_STARVE: snprintf(msg, sizeof(msg), "%s starved to death", p.name); break;
        case DC_CACTUS: snprintf(msg, sizeof(msg), "%s was pricked to death", p.name); break;
        case DC_SUFFOCATE: snprintf(msg, sizeof(msg), "%s suffocated in a wall", p.name); break;
        case DC_KILL: snprintf(msg, sizeof(msg), "%s was killed", p.name); break;
        case DC_ARROW: snprintf(msg, sizeof(msg), "%s was shot by %s", p.name, killer[0] ? killer : "an arrow"); break;
        case DC_EXPLOSION:
            if (killer[0]) snprintf(msg, sizeof(msg), "%s was blown up by %s", p.name, killer);
            else snprintf(msg, sizeof(msg), "%s blew up", p.name);
            break;
        default:
            if (killer[0]) snprintf(msg, sizeof(msg), "%s was slain by %s", p.name, killer);
            else snprintf(msg, sizeof(msg), "%s died", p.name);
            break;
    }
    p.sendHealth();
    {
        char json[300];
        textJson(json, sizeof(json), msg, nullptr);
        Packet pk(pkt::s2c::CombatEvent);
        pk.w.varint(2);
        pk.w.varint(p.e.id);
        pk.w.i32(kp ? kp->e.id : (ke ? ke->id : -1));
        pk.w.string(json);
        p.conn.send(pk);
    }
    broadcastStatus(p.e, 3);
    broadcastSystem(msg, nullptr);
    if (p.isSurvivalLike()) {
        for (int i = 0; i < INV_SIZE; i++) {
            if (i == SLOT_CRAFT_RESULT || p.inv[i].empty()) continue;
            dropItem(p.e.x, p.e.y + 1, p.e.z, p.inv[i]);
            p.inv[i].clear();
        }
        if (!p.cursor.empty()) { dropItem(p.e.x, p.e.y + 1, p.e.z, p.cursor); p.cursor.clear(); }
        p.xpLevel = 0;
        p.xpProgress = 0;
        p.xpTotal = 0;
        p.sendInventory();
    }
    if (kp && kp != &p) giveXp(*kp, 7);
}

void Server::respawnPlayer(Player& p) {
    if (!p.dead) return;
    p.dead = false;
    p.e.health = 20;
    p.e.fireTicks = 0;
    p.e.air = 300;
    p.e.fallDistance = 0;
    p.e.invuln = 60;
    p.e.flags &= (uint8_t)~(EF_CROUCHING | EF_SPRINTING | EF_ON_FIRE);
    p.e.pose = POSE_STANDING;
    p.food = 20;
    p.saturation = 5;
    p.exhaustion = 0;
    int sx = p.hasSpawn ? p.spawnX : meta.spawnX, sz = p.hasSpawn ? p.spawnZ : meta.spawnZ;
    Chunk* c = world.load(sx >> 4, sz >> 4);
    int sy = c->height(sx & 15, sz & 15);
    if (p.hasSpawn) {
        // bed spawn: stand next to the bed if it still exists
        uint16_t b = blockAt(p.spawnX, p.spawnY, p.spawnZ);
        if (!(BLOCKS[blockIdOf(b)].name && strstr(BLOCKS[blockIdOf(b)].name, "_bed"))) {
            p.hasSpawn = false;
            p.sendSystem("You have no home bed or charged respawn anchor, or it was obstructed", nullptr);
            sx = meta.spawnX; sz = meta.spawnZ;
            c = world.load(sx >> 4, sz >> 4);
            sy = c->height(sx & 15, sz & 15);
        } else {
            sy = p.spawnY + 1;
        }
    }
    if (sy < 1) sy = meta.spawnY;
    {
        Packet pk(pkt::s2c::Respawn);
        pk.w.bytes(DIMENSION_NBT, DIMENSION_NBT_LEN);
        pk.w.string("minecraft:overworld");
        pk.w.i64((int64_t)mix64(meta.seed));
        pk.w.u8(p.gamemode);
        pk.w.u8(p.gamemode);
        pk.w.boolean(false);
        pk.w.boolean(meta.worldType == WORLD_FLAT);
        pk.w.boolean(false);
        p.conn.send(pk);
    }
    // the client dropped all chunks and entities
    p.resetView();
    forgetEntities(p);
    p.sendAbilities();
    p.teleport(sx + 0.5, sy, sz + 0.5, p.e.yaw, 0);
    p.sendHealth();
    p.sendXp();
    p.sendInventory();
    {
        Packet pk(pkt::s2c::HeldItemSlot);
        pk.w.i8((int8_t)p.held);
        p.conn.send(pk);
    }
    {
        Packet pk(pkt::s2c::SpawnPosition);
        pk.w.u64(packPos(meta.spawnX, meta.spawnY, meta.spawnZ));
        p.conn.send(pk);
    }
    p.sendTime();
    p.e.sx = p.e.x; p.e.sy = p.e.y; p.e.sz = p.e.z;
    p.e.sinceTeleport = 400;  // force a teleport packet to everybody
    p.e.metaDirty = true;
}

void Server::addExhaustion(Player& p, float amount) {
    if (!p.isSurvivalLike()) return;
    p.exhaustion += amount;
}

void Server::heal(Player& p, float amount) {
    if (p.dead || p.e.health >= 20) return;
    p.e.health += amount;
    if (p.e.health > 20) p.e.health = 20;
    p.healthDirty = true;
}

static int xpForLevel(int level) {
    if (level <= 15) return 2 * level + 7;
    if (level <= 30) return 5 * level - 38;
    return 9 * level - 158;
}

void Server::giveXp(Player& p, int points) {
    if (points <= 0) return;
    p.xpTotal += points;
    float prog = p.xpProgress + (float)points / xpForLevel(p.xpLevel);
    int oldLevel = p.xpLevel;
    while (prog >= 1.0f) {
        prog = (prog - 1.0f) * xpForLevel(p.xpLevel) / xpForLevel(p.xpLevel + 1);
        p.xpLevel++;
    }
    p.xpProgress = prog;
    p.sendXp();
    if (p.xpLevel > oldLevel && p.xpLevel % 5 == 0)
        playSound("entity.player.levelup", p.e.x, p.e.y, p.e.z, 0.75f, 1, 7);
    else
        playSound("entity.experience_orb.pickup", p.e.x, p.e.y, p.e.z, 0.1f, 1, 7);
}

void Server::finishUsingItem(Player& p) {
    ItemStack& it = p.usingHand == 1 ? p.inv[SLOT_OFFHAND] : p.heldItem();
    if (it.empty() || ITEMS[it.id].kind != IK_FOOD) return;
    const ItemDef& d = ITEMS[it.id];
    p.food += d.food;
    if (p.food > 20) p.food = 20;
    p.saturation += d.saturation10 / 10.0f;
    if (p.saturation > p.food) p.saturation = (float)p.food;
    if (it.id == itm::GoldenApple) heal(p, 4);
    if (it.id == itm::EnchantedGoldenApple) heal(p, 16);
    p.healthDirty = true;
    uint16_t container = 0;
    if (it.id == itm::MushroomStew || it.id == itm::RabbitStew || it.id == itm::BeetrootSoup || it.id == itm::SuspiciousStew)
        container = itm::Bowl;
    if (it.id == itm::HoneyBottle) container = itm::GlassBottle;
    if (p.gamemode != GM_CREATIVE) {
        if (--it.count == 0) it.clear();
        p.sendSlot(p.usingHand == 1 ? SLOT_OFFHAND : SLOT_HOTBAR_START + p.held);
        if (container) giveItem(p, ItemStack::of(container));
    }
    Packet pk(pkt::s2c::EntityStatus);
    pk.w.i32(p.e.id);
    pk.w.i8(9);
    p.conn.send(pk);
    playSound("entity.player.burp", p.e.x, p.e.y, p.e.z, 0.5f, 1, 7);
}

static bool touches(Server& s, const Player& p, uint16_t blockId, double grow) {
    double hw = 0.3 + grow;
    int x0 = (int)floor(p.e.x - hw), x1 = (int)floor(p.e.x + hw);
    int y0 = (int)floor(p.e.y), y1 = (int)floor(p.e.y + 1.79);
    int z0 = (int)floor(p.e.z - hw), z1 = (int)floor(p.e.z + hw);
    for (int x = x0; x <= x1; x++)
        for (int y = y0; y <= y1; y++)
            for (int z = z0; z <= z1; z++)
                if (blockIdOf(s.blockAt(x, y, z)) == blockId) return true;
    return false;
}

void Server::tickSurvival(Player& p) {
    if (p.e.invuln > 0) p.e.invuln--;
    if (p.dead) {
        p.e.deathTicks++;
        return;
    }
    if (!p.positionReady) return;
    bool survival = p.isSurvivalLike();
    // eating
    if (p.usingTicks > 0 && --p.usingTicks == 0) finishUsingItem(p);

    // breathing
    int bx = (int)floor(p.e.x), bz = (int)floor(p.e.z);
    uint16_t eyes = blockAt(bx, (int)floor(p.e.y + 1.62), bz);
    int oldAir = p.e.air;
    if (blockIdOf(eyes) == blk::Water && survival) {
        if (--p.e.air <= -20) {
            p.e.air = 0;
            damagePlayer(p, 2, DC_DROWN, -1);
        }
    } else if (p.e.air < 300) {
        p.e.air = p.e.air + 4 > 300 ? 300 : p.e.air + 4;
    }
    if (p.e.air / 15 != oldAir / 15) p.e.metaDirty = true;

    if (survival) {
        // lava and fire
        bool lava = touches(*this, p, blk::Lava, -0.05);
        bool water = touches(*this, p, blk::Water, -0.05);
        if (lava) {
            if (ticks % 10 == 0) damagePlayer(p, 4, DC_LAVA, -1);
            if (p.e.fireTicks <= 0) p.e.metaDirty = true;
            p.e.fireTicks = 300;
        } else if (touches(*this, p, blk::Fire, -0.05)) {
            if (ticks % 10 == 0) damagePlayer(p, 1, DC_FIRE, -1);
            if (p.e.fireTicks < 160) { p.e.fireTicks = 160; p.e.metaDirty = true; }
        }
        if (p.e.fireTicks > 0) {
            if (water) { p.e.fireTicks = 0; p.e.metaDirty = true; }
            else {
                if (p.e.fireTicks % 20 == 0 && !lava) damagePlayer(p, 1, DC_FIRE, -1);
                if (--p.e.fireTicks == 0) p.e.metaDirty = true;
            }
        }
        if (ticks % 10 == 0 && touches(*this, p, blk::Cactus, 0.02)) damagePlayer(p, 1, DC_CACTUS, -1);
        if (ticks % 10 == 0) {
            uint16_t head = blockAt(bx, (int)floor(p.e.y + 1.62), bz);
            if (stateOpaque(head) && stateCollides(head)) damagePlayer(p, 1, DC_SUFFOCATE, -1);
        }
    }
    if (p.e.y < -64 && ticks % 10 == 0) damagePlayer(p, 4, DC_VOID, -1);
    if (p.dead) return;

    // hunger (vanilla 1.16 FoodData rules)
    if (survival) {
        if (p.exhaustion > 4) {
            p.exhaustion -= 4;
            if (p.saturation > 0) p.saturation = p.saturation - 1 > 0 ? p.saturation - 1 : 0;
            else if (cfg.difficulty > 0 && p.food > 0) p.food--;
            p.healthDirty = true;
        }
        if (p.saturation > 0 && p.food >= 20 && p.e.health < 20) {
            if (++p.foodTimer >= 10) {
                float s = p.saturation < 6 ? p.saturation : 6;
                heal(p, s / 6.0f);
                p.exhaustion += s;
                p.foodTimer = 0;
            }
        } else if (p.food >= 18 && p.e.health < 20) {
            if (++p.foodTimer >= 80) {
                heal(p, 1);
                p.exhaustion += 6;
                p.foodTimer = 0;
            }
        } else if (p.food <= 0) {
            if (++p.foodTimer >= 80) {
                if (p.e.health > 10 || cfg.difficulty == 3 || (p.e.health > 1 && cfg.difficulty == 2))
                    damagePlayer(p, 1, DC_STARVE, -1);
                p.foodTimer = 0;
            }
        } else {
            p.foodTimer = 0;
        }
        if (cfg.difficulty == 0 && ticks % 20 == 0) {
            heal(p, 1);
            if (p.food < 20) { p.food++; p.healthDirty = true; }
        }
    }
    if (p.healthDirty && !p.dead) p.sendHealth();
}

}  // namespace mc
