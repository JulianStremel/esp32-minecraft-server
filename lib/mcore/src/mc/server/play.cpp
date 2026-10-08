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
        case ChatMessage: onChat(r); break;
        case ChatCommand:
        case ChatCommandSigned: onChatCommand(r); break;
        case ClientCommand: onClientCommand(r); break;
        case Settings: onSettings(r); break;
        case TabComplete: onTabComplete(r); break;
        case EditBook: onEditBook(r); break;
        case EnchantItem: onWindowButton(r); break;
        case WindowClick: onWindowClick(r); break;
        case CloseWindow: onCloseWindow(r); break;
        case UseEntity: onUseEntity(r); break;
        case KeepAlive: onKeepAlive(r); break;
        case Position: {
            double x = r.f64(), y = r.f64(), z = r.f64();
            uint8_t f = r.u8();
            if (r.ok()) onMove(x, y, z, true, 0, 0, false, f & 1);
            break;
        }
        case PositionLook: {
            double x = r.f64(), y = r.f64(), z = r.f64();
            float yaw = r.f32(), pitch = r.f32();
            uint8_t f = r.u8();
            if (r.ok()) onMove(x, y, z, true, yaw, pitch, true, f & 1);
            break;
        }
        case Look: {
            float yaw = r.f32(), pitch = r.f32();
            uint8_t f = r.u8();
            if (r.ok()) onMove(0, 0, 0, false, yaw, pitch, true, f & 1);
            break;
        }
        case Flying: {
            uint8_t f = r.u8();
            if (r.ok()) onMove(0, 0, 0, false, 0, 0, false, f & 1);
            break;
        }
        case PickItemFromBlock: onPickItem(r); break;
        case SetSlotState: onSlotState(r); break;
        case Abilities: onAbilities(r); break;
        case BlockDig: onDig(r); break;
        case EntityAction: onEntityAction(r); break;
        case PlayerInput: onPlayerInput(r); break;
        case HeldItemSlot: onHeldItem(r); break;
        case SetCreativeSlot: onCreativeSlot(r); break;
        case UpdateSign: onUpdateSign(r); break;
        case ArmAnimation: onSwing(r); break;
        case BlockPlace: onPlace(r); break;
        case UseItem: onUseItem(r); break;
        default: break;  // custom payloads, recipe book, chunk batch acks, tick end, ...
    }
    // block actions carry a sequence number: acknowledging it makes the client accept the
    // server's blocks in place of its predictions (the changes were sent before this)
    if (lastSequence >= 0) {
        Packet pk(pkt::s2c::AcknowledgePlayerDigging);
        pk.w.varint(lastSequence);
        conn.send(pk);
        lastSequence = -1;
    }
}

void Player::onKeepAlive(Reader& r) {
    int64_t id = r.i64();
    if (kaPending && id == kaId) {
        kaPending = false;
        ping = (int)(plat::millis() - kaSentMs);
    }
}

// Chat is unsigned here (secure chat is not enforced): only the text is used.
void Player::onChat(Reader& r) {
    char msg[257];
    r.string(msg, sizeof(msg));
    if (!r.ok() || !msg[0]) return;
    chatLine(msg);
}

void Player::onChatCommand(Reader& r) {
    char cmd[257];
    cmd[0] = '/';
    r.string(cmd + 1, sizeof(cmd) - 1);
    if (!r.ok() || !cmd[1]) return;
    chatLine(cmd);
}

void Player::chatLine(const char* msg) {
    for (const char* p = msg; *p; p++)
        if ((unsigned char)*p < 0x20 || *p == 0x7F) { kick("Illegal characters in chat"); return; }
    if (msg[0] != '/' && srv->menuChat(*this, msg)) return;   // a seed for the operator menu
    if (msg[0] == '/') {
        MC_LOGI("%s issued server command: %s", name, msg);
        srv->runCommand(this, msg + 1);
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
    // vanilla's limit: bursts of 10 messages, one per second on average; operators are exempt
    chatSpam += 20;
    if (chatSpam > 200 && !op) kick("Kicked for spamming");
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
    r.boolean();      // text filtering
    r.boolean();      // listed in the server's player list
    r.varint();       // particles
    if (!r.ok()) return;
    clientViewDist = vd;
    if (state != CS_PLAY) {   // configuration: remembered for the join
        skinParts = parts;
        mainHand = (uint8_t)hand;
        return;
    }
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
        case 1: e.flags |= EF_SPRINTING; e.metaDirty = true; break;
        case 2: e.flags &= ~EF_SPRINTING; e.metaDirty = true; break;
        default: break;
    }
}

// The movement keys (1.21.2+); sneaking is the shift key.
void Player::onPlayerInput(Reader& r) {
    uint8_t keys = r.u8();
    if (!r.ok()) return;
    bool sneak = keys & 0x20;
    if (sneak != ((inputs & 0x20) != 0)) {
        if (sneak) { e.flags |= EF_CROUCHING; e.pose = POSE_CROUCHING; }
        else { e.flags &= ~EF_CROUCHING; e.pose = POSE_STANDING; }
        e.metaDirty = true;
    }
    inputs = keys;
}

void Player::onUseItem(Reader& r) {
    int hand = r.varint();
    lastSequence = r.varint();
    r.f32();   // yaw
    r.f32();   // pitch
    if (!r.ok() || dead) return;
    ItemStack& it = hand == 1 ? inv[SLOT_OFFHAND] : heldItem();
    if (it.empty()) return;
    const ItemDef& d = ITEMS[it.id];
    if (d.kind == IK_FOOD) {
        bool alwaysEdible = it.id == itm::GoldenApple || it.id == itm::EnchantedGoldenApple || it.id == itm::ChorusFruit;
        if (food < 20 || alwaysEdible || gamemode == GM_CREATIVE) {
            usingTicks = it.id == itm::DriedKelp ? 16 : 32;
            usingHand = (uint8_t)hand;
        }
    } else if (it.id == itm::WrittenBook) {
        Packet packet(pkt::s2c::OpenBook); packet.w.varint(hand); conn.send(packet);
    } else if (it.id == itm::Bow) {
        drawingBow = true;
        bowStart = srv->ticks;
    }
}

// Blocks at the player's feet or waist that cancel the fall so far.
static bool fallStops(Server& s, const Entity& e) {
    int bx = (int)floor(e.x), bz = (int)floor(e.z);
    for (int k = 0; k < 2; k++) {
        uint16_t st = s.blockAt(bx, (int)floor(e.y + (k ? 0.9 : 0.0)), bz);
        uint16_t id = blockIdOf(st);
        if (id == blk::Water || id == blk::BubbleColumn || id == blk::Kelp || id == blk::KelpPlant ||
            id == blk::Seagrass || id == blk::TallSeagrass || getProp(st, "waterlogged") == 1)
            return true;
        if (id == blk::Ladder || id == blk::Vine || id == blk::Scaffolding || id == blk::Cobweb ||
            id == blk::WeepingVines || id == blk::WeepingVinesPlant || id == blk::TwistingVines ||
            id == blk::TwistingVinesPlant)
            return true;
    }
    return false;
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
        // as vanilla: water, climbing and cobwebs end a fall (a fall into water used to
        // keep counting while swimming and hurt on stepping out onto land)
        if (fallStops(*srv, e) || flying || !isSurvivalLike()) e.fallDistance = 0;   // flying, creative: no fall
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
