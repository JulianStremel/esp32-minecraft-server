// Play-state packet dispatch and the simple handlers (chat, movement, settings...).
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "mc/registry.h"
#include "mc/server/server.h"

namespace mc {

void Player::handlePlay(int id, Reader& r) {
    using namespace pkt::c2s;
    switch (id) {
        case TeleportConfirm: {
            int32_t tid = r.varint();
            if (tid == teleportId) awaitTeleport = false;
            break;
        }
        case Chat: onChat(r); break;
        case ClientCommand: onClientCommand(r); break;
        case Settings: onSettings(r); break;
        case TabComplete: onTabComplete(r); break;
        case WindowClick: onWindowClick(r); break;
        case CloseWindow: onCloseWindow(r); break;
        case UseEntity: onUseEntity(r); break;
        case KeepAlive: onKeepAlive(r); break;
        case Position: {
            double x = r.f64(), y = r.f64(), z = r.f64();
            bool g = r.boolean();
            if (r.ok()) onMove(x, y, z, true, 0, 0, false, g);
            break;
        }
        case PositionLook: {
            double x = r.f64(), y = r.f64(), z = r.f64();
            float yaw = r.f32(), pitch = r.f32();
            bool g = r.boolean();
            if (r.ok()) onMove(x, y, z, true, yaw, pitch, true, g);
            break;
        }
        case Look: {
            float yaw = r.f32(), pitch = r.f32();
            bool g = r.boolean();
            if (r.ok()) onMove(0, 0, 0, false, yaw, pitch, true, g);
            break;
        }
        case Flying: {
            bool g = r.boolean();
            if (r.ok()) onMove(0, 0, 0, false, 0, 0, false, g);
            break;
        }
        case PickItem: onPickItem(r); break;
        case Abilities: onAbilities(r); break;
        case BlockDig: onDig(r); break;
        case EntityAction: onEntityAction(r); break;
        case HeldItemSlot: onHeldItem(r); break;
        case SetCreativeSlot: onCreativeSlot(r); break;
        case UpdateSign: onUpdateSign(r); break;
        case ArmAnimation: onSwing(r); break;
        case BlockPlace: onPlace(r); break;
        case UseItem: onUseItem(r); break;
        default: break;  // custom payloads, recipe book, advancements, ...
    }
}

void Player::onKeepAlive(Reader& r) {
    int64_t id = r.i64();
    if (kaPending && id == kaId) {
        kaPending = false;
        ping = (int)(plat::millis() - kaSentMs);
    }
}

void Player::onChat(Reader& r) {
    char msg[257];
    r.string(msg, sizeof(msg));
    if (!r.ok() || !msg[0]) return;
    for (char* p = msg; *p; p++)
        if ((unsigned char)*p < 0x20 || *p == 0x7F) { kick("Illegal characters in chat"); return; }
    if (msg[0] == '/') {
        MC_LOGI("%s issued server command: %s", name, msg);
        srv->runCommand(this, msg + 1);
        return;
    }
    if (--chatTokens < 0) {
        sendSystem("You are sending messages too fast.", "red");
        if (chatTokens < -10) kick("Spamming");
        return;
    }
    char ename[40], emsg[600], json[800];
    jsonEscape(name, ename, sizeof(ename));
    jsonEscape(msg, emsg, sizeof(emsg));
    snprintf(json, sizeof(json),
             "{\"translate\":\"chat.type.text\",\"with\":[{\"text\":\"%s\",\"insertion\":\"%s\",\"clickEvent\":"
             "{\"action\":\"suggest_command\",\"value\":\"/msg %s \"}},{\"text\":\"%s\"}]}",
             ename, ename, ename, emsg);
    MC_LOGI("<%s> %s", name, msg);
    srv->broadcastChat(json, 0);
}

void Player::onClientCommand(Reader& r) {
    int action = r.varint();
    if (action == 0 && dead) srv->respawnPlayer(*this);
}

void Player::onSettings(Reader& r) {
    char locale[17];
    r.string(locale, sizeof(locale));
    int vd = r.i8();
    r.varint();       // chat mode
    r.boolean();      // chat colors
    uint8_t parts = r.u8();
    int hand = r.varint();
    if (!r.ok()) return;
    clientViewDist = vd;
    int want = vd < srv->cfg.viewDistance ? vd : srv->cfg.viewDistance;
    if (want < 2) want = 2;
    if (want != viewDist) {
        viewDist = want;
        updateView(true);
    }
    if (parts != skinParts || hand != mainHand) {
        skinParts = parts;
        mainHand = (uint8_t)hand;
        e.metaDirty = true;
    }
}

void Player::onAbilities(Reader& r) {
    int8_t flags = r.i8();
    bool wantFly = (flags & 0x02) != 0;
    if (gamemode == GM_CREATIVE || gamemode == GM_SPECTATOR) flying = wantFly;
    else if (wantFly) sendAbilities();  // not allowed: correct the client
}

void Player::onHeldItem(Reader& r) {
    int s = r.i16();
    if (s < 0 || s > 8) return;
    held = (uint8_t)s;
    usingTicks = 0;
    srv->broadcastEquipment(*this);
}

void Player::onSwing(Reader& r) {
    int hand = r.varint();
    srv->broadcastAnimation(e, hand == 1 ? 3 : 0, this);
}

void Player::onEntityAction(Reader& r) {
    r.varint();
    int action = r.varint();
    r.varint();
    switch (action) {
        case 0: e.flags |= EF_CROUCHING; e.pose = POSE_CROUCHING; e.metaDirty = true; break;
        case 1: e.flags &= ~EF_CROUCHING; e.pose = POSE_STANDING; e.metaDirty = true; break;
        case 3: e.flags |= EF_SPRINTING; e.metaDirty = true; break;
        case 4: e.flags &= ~EF_SPRINTING; e.metaDirty = true; break;
        default: break;
    }
}

void Player::onUseItem(Reader& r) {
    int hand = r.varint();
    if (dead) return;
    ItemStack& it = hand == 1 ? inv[SLOT_OFFHAND] : heldItem();
    if (it.empty()) return;
    const ItemDef& d = ITEMS[it.id];
    if (d.kind == IK_FOOD) {
        bool alwaysEdible = it.id == itm::GoldenApple || it.id == itm::EnchantedGoldenApple || it.id == itm::ChorusFruit;
        if (food < 20 || alwaysEdible || gamemode == GM_CREATIVE) {
            usingTicks = it.id == itm::DriedKelp ? 16 : 32;
            usingHand = (uint8_t)hand;
        }
    } else if (it.id == itm::Bow) {
        drawingBow = true;
        bowStart = srv->ticks;
    }
}

void Player::onMove(double x, double y, double z, bool hasPos, float yaw, float pitch, bool hasLook, bool onGround) {
    if (awaitTeleport) return;  // ignore until the client confirmed our teleport
    if (hasPos) {
        if (!isfinite(x) || !isfinite(y) || !isfinite(z) || fabs(x) > 3.0e7 || fabs(z) > 3.0e7) {
            kick("Invalid move");
            return;
        }
        double dx = x - e.x, dy = y - e.y, dz = z - e.z;
        double d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > 100.0 * 100.0 && !dead) {
            MC_LOGW("%s moved too quickly (%.1f)", name, sqrt(d2));
            teleport(e.x, e.y, e.z, e.yaw, e.pitch);
            return;
        }
        if (dead) return;
        // fall tracking
        if (!onGround && dy < 0) e.fallDistance += (float)-dy;
        e.x = x;
        e.y = y;
        e.z = z;
        // exhaustion from walking / sprinting / jumping
        double hd = sqrt(dx * dx + dz * dz);
        if (isSurvivalLike()) {
            if (e.flags & EF_SPRINTING) srv->addExhaustion(*this, (float)(hd * 0.1));
            if (dy > 0.2 && e.onGround && !onGround) srv->addExhaustion(*this, (e.flags & EF_SPRINTING) ? 0.2f : 0.05f);
        }
        positionReady = true;
        int cx = (int)floor(x) >> 4, cz = (int)floor(z) >> 4;
        if (cx != centerCx || cz != centerCz) updateView(false);
    }
    if (hasLook) {
        if (!isfinite(yaw) || !isfinite(pitch)) return;
        e.yaw = yaw;
        e.pitch = pitch;
        e.headYaw = yaw;
    }
    bool wasOnGround = e.onGround;
    e.onGround = onGround;
    if (onGround && !wasOnGround) {
        float fd = e.fallDistance;
        e.fallDistance = 0;
        if (fd > 3.0f && isSurvivalLike() && !flying) {
            // no fall damage when landing in water / on hay / slime
            int bx = (int)floor(e.x), by = (int)floor(e.y - 0.2), bz = (int)floor(e.z);
            uint16_t below = srv->blockAt(bx, by, bz);
            uint16_t feet = srv->blockAt(bx, (int)floor(e.y), bz);
            float dmg = ceilf(fd - 3.0f);
            if (blockIdOf(below) == blk::HayBlock) dmg = ceilf(dmg * 0.2f);
            if (blockIdOf(below) == blk::SlimeBlock || blockIdOf(feet) == blk::Water) dmg = 0;
            if (dmg > 0) srv->damagePlayer(*this, dmg, DC_FALL, -1);
        }
    } else if (onGround) {
        e.fallDistance = 0;
    }
}

}  // namespace mc
