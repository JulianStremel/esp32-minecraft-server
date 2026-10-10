// Vehicles: boats, chest boats and rafts (AbstractBoat, VehicleEntity).
//
// - A boat item is used on water or a block in reach (BoatItem#use: a ray from the eyes
//   that stops at fluids too); the boat faces the way the player looks.
// - Using a boat gets in (two seats; one on a chest boat, whose other is the chest);
//   sneaking while using a chest boat opens its 27 slots. Sneaking gets out, to a free
//   spot beside the boat. SetPassengers tells every client that sees the boat.
// - A boat a player steers is moved by that player's client (VehicleMove), as in vanilla;
//   the server checks the move (not too far, not into blocks), moves the riders with it
//   and corrects the client otherwise. Other boats run vanilla's floatBoat here:
//   buoyancy in water, gravity, the drag of water, air and land (ice is slippery).
// - Mobs that bump into a boat with a free seat get in (not players, not mobs as wide as
//   the boat). They stay until the boat breaks.
// - Hits add 10x their damage; above 40 the boat breaks into its item (and a chest boat's
//   contents); a creative player breaks it at once. Below the world it is gone.
#include <math.h>
#include <string.h>
#include "mc/registry.h"
#include "mc/server/mob_util.h"
#include "mc/server/server.h"

namespace mc {

namespace {
struct BoatKind { uint16_t item, type; bool chest; };
BoatKind g_boats[24];
int g_numBoats = -1;

// boat items and entity types share their names (oak_boat, oak_chest_boat, bamboo_raft, ...)
void loadBoatKinds() {
    if (g_numBoats >= 0) return;
    g_numBoats = 0;
    for (int i = 0; i < NUM_ITEMS && g_numBoats < 24; i++) {
        const char* n = ITEMS[i].name;
        size_t l = strlen(n);
        bool boat = (l > 5 && !strcmp(n + l - 5, "_boat")) || (l > 5 && !strcmp(n + l - 5, "_raft"));
        if (!boat) continue;
        int t = findEntityType(n);
        if (t < 0) continue;
        g_boats[g_numBoats++] = {(uint16_t)i, (uint16_t)t, strstr(n, "_chest_") != nullptr};
    }
}
const BoatKind* boatByItem(uint16_t item) {
    loadBoatKinds();
    for (int i = 0; i < g_numBoats; i++)
        if (g_boats[i].item == item) return &g_boats[i];
    return nullptr;
}
const BoatKind* boatByType(uint16_t type) {
    loadBoatKinds();
    for (int i = 0; i < g_numBoats; i++)
        if (g_boats[i].type == type) return &g_boats[i];
    return nullptr;
}

// The surface of the water in a block (vanilla's fluid height: a source 8/9 of the block,
// flowing water amount/9, a full block under more water); -1 without water.
double waterTop(Server& s, int x, int y, int z) {
    uint16_t st = s.blockAt(x, y, z);
    bool water = blockIdOf(st) == blk::Water;
    if (!water && !getBool(st, "waterlogged")) return -1;
    uint16_t above = s.blockAt(x, y + 1, z);
    if (blockIdOf(above) == blk::Water || getBool(above, "waterlogged")) return y + 1.0;
    int level = water ? getProp(st, "level") : 0;
    int amount = level >= 8 ? 8 : 8 - level;
    return y + amount / 9.0;
}

double blockFriction(uint16_t st) {
    uint16_t id = blockIdOf(st);
    if (id == blk::Ice || id == blk::PackedIce || id == blk::FrostedIce) return 0.98;
    if (id == blk::BlueIce) return 0.989;
    if (id == blk::SlimeBlock) return 0.8;
    return 0.6;
}
}  // namespace

Entity* Server::vehicleOf(const Entity& rider) {
    if (rider.vehicle < 0) return nullptr;
    Entity* v = findEntity(rider.vehicle);
    return v && (v->kind == EK_BOAT || v->kind == EK_MINECART) ? v : nullptr;
}

// boats two seats (a chest boat one: the chest takes the other), a rideable minecart one
int Server::seatsOf(const Entity& v) {
    if (v.kind == EK_MINECART) return v.type == ent::Minecart ? 1 : 0;
    const BoatKind* k = boatByType(v.type);
    return k && k->chest ? 1 : 2;
}

// the item a vehicle breaks into (minecarts and boats share their names with it)
uint16_t Server::vehicleItem(const Entity& v) {
    if (v.kind == EK_BOAT) {
        const BoatKind* k = boatByType(v.type);
        return k ? k->item : 0;
    }
    int it = v.type < NUM_ENTITY_TYPES ? findItem(ENTITY_TYPES[v.type].name) : -1;
    return it > 0 ? (uint16_t)it : (uint16_t)itm::Minecart;
}

// ------------------------------------------------------------------ placing
bool Server::useBoatItem(Player& p, int hand) {
    ItemStack& it = hand == 1 ? p.inv[SLOT_OFFHAND] : p.heldItem();
    const BoatKind* kind = boatByItem(it.id);
    if (!kind) return false;
    // the ray from the eyes, stopping at water or the first block (5 blocks)
    double yaw = p.e.yaw * M_PI / 180.0, pitch = p.e.pitch * M_PI / 180.0;
    double dx = -sin(yaw) * cos(pitch), dy = -sin(pitch), dz = cos(yaw) * cos(pitch);
    double ex = p.e.x, ey = p.e.y + ((p.e.flags & EF_CROUCHING) ? 1.27 : 1.62), ez = p.e.z;
    bool found = false;
    double hx = 0, hy = 0, hz = 0;
    double prevY = ey;
    for (double t = 0; t <= 5.0 && !found; t += 0.05) {
        double x = ex + dx * t, y = ey + dy * t, z = ez + dz * t;
        int bx = (int)floor(x), by = (int)floor(y), bz = (int)floor(z);
        double wt = waterTop(*this, bx, by, bz);
        if (wt >= 0 && y <= wt) {   // on the water's surface
            hx = x; hy = wt; hz = z;
            found = true;
            break;
        }
        uint16_t st = blockAt(bx, by, bz);
        if (stateCollides(st)) {
            double top = by + collisionTop32(st) / 32.0;
            if (y > top) { prevY = y; continue; }   // above a low block's shape
            if (prevY < top - 0.01) return false;   // a wall: no room for a boat there
            hx = x; hy = top; hz = z;
            found = true;
            break;
        }
        prevY = y;
    }
    if (!found) return false;
    const EntityTypeDef& def = ENTITY_TYPES[kind->type];
    if (mobs::boxCollides(*this, hx, hy + 0.01, hz, def.width, def.height)) return false;
    Entity* b = spawnEntity(EK_BOAT, kind->type, hx, hy, hz);
    if (!b) return false;
    b->yaw = b->headYaw = p.e.yaw;
    b->health = 1;
    if (kind->chest) b->cargo.resize(27);
    if (p.gamemode != GM_CREATIVE) {
        if (--it.count == 0) it.clear();
        p.sendSlot(hand == 1 ? SLOT_OFFHAND : SLOT_HOTBAR_START + p.held);
    }
    vibration(hx, hy, hz, GE_ENTITY_PLACE);
    return true;
}

// ------------------------------------------------------------------ riding
void Server::sendPassengers(Entity& v, Player* only) {
    Packet pk(pkt::s2c::SetPassengers);
    pk.w.varint(v.id);
    int n = (v.passengers[0] >= 0) + (v.passengers[1] >= 0);
    pk.w.varint(n);
    for (int i = 0; i < 2; i++)
        if (v.passengers[i] >= 0) pk.w.varint(v.passengers[i]);
    if (only) {
        only->conn.send(pk);
        return;
    }
    int idx = (int)(&v - entities);
    if (idx < 0 || idx >= MC_MAX_ENTITIES) return;
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& pl = players[i];
        if (pl.inPlay() && ((pl.knownEntities[idx >> 3] >> (idx & 7)) & 1)) pl.conn.send(pk);
    }
}

bool Server::mount(Entity& rider, Entity& v) {
    if ((v.kind != EK_BOAT && v.kind != EK_MINECART) || v.removed || rider.vehicle >= 0 || &rider == &v) return false;
    int seat = -1;
    for (int i = 0; i < seatsOf(v); i++)
        if (v.passengers[i] < 0) { seat = i; break; }
    if (seat < 0) return false;
    v.passengers[seat] = rider.id;
    rider.vehicle = v.id;
    rider.vx = rider.vy = rider.vz = 0;
    rider.fallDistance = 0;
    rider.x = v.x;
    rider.y = v.y;
    rider.z = v.z;
    sendPassengers(v);
    return true;
}

void Server::dismount(Entity& rider, bool placeBeside) {
    if (rider.vehicle < 0) return;
    Entity* v = findEntity(rider.vehicle);
    rider.vehicle = -1;
    rider.mountCooldown = 60;
    if (!v) return;
    InDim in(*this, v->dim);
    for (int i = 0; i < 2; i++)
        if (v->passengers[i] == rider.id) v->passengers[i] = -1;
    if (v->passengers[0] < 0 && v->passengers[1] >= 0) {   // the other one moves up
        v->passengers[0] = v->passengers[1];
        v->passengers[1] = -1;
    }
    sendPassengers(*v);
    if (!placeBeside) return;
    // a free spot beside the boat, on solid ground or water (DismountHelper), else on top
    double x = v->x, y = v->y + 0.6, z = v->z;
    double yaw = v->yaw * M_PI / 180.0;
    double fx = -sin(yaw), fz = cos(yaw);   // the boat's front
    const double sides[4][2] = {{fz, -fx}, {-fz, fx}, {fx, fz}, {-fx, -fz}};   // left, right, front, back
    for (const auto& d : sides) {
        double cx = v->x + d[0] * 1.2, cz = v->z + d[1] * 1.2;
        for (int up = 0; up <= 1; up++) {
            double cy = floor(v->y) + up;
            if (mobs::boxCollides(*this, cx, cy + 0.01, cz, 0.6f, 1.8f)) continue;
            bool ground = mobs::boxCollides(*this, cx, cy - 0.3, cz, 0.6f, 0.3f) ||
                          waterTop(*this, (int)floor(cx), (int)floor(cy), (int)floor(cz)) >= 0 ||
                          waterTop(*this, (int)floor(cx), (int)floor(cy) - 1, (int)floor(cz)) >= 0;
            if (!ground) continue;
            x = cx; y = cy; z = cz;
            goto found;
        }
    }
found:
    if (rider.kind == EK_PLAYER) {
        Player& p = players[rider.playerSlot];
        p.teleport(x, y, z, p.e.yaw, p.e.pitch);
    } else {
        rider.x = x;
        rider.y = y;
        rider.z = z;
    }
}

void Server::interactVehicle(Player& p, Entity& v) {
    if (p.dead || p.gamemode == GM_SPECTATOR) return;
    if (v.kind == EK_MINECART) {
        interactMinecart(p, v);
        return;
    }
    if (v.kind != EK_BOAT) return;
    const BoatKind* k = boatByType(v.type);
    if (k && k->chest && (p.e.flags & EF_CROUCHING)) {
        openEntityContainer(p, v);
        return;
    }
    if (p.e.flags & EF_CROUCHING) return;
    double dx = p.e.x - v.x, dz = p.e.z - v.z;
    if (dx * dx + dz * dz > 36) return;
    if (p.sleeping) wakeUp(p);
    closeWindow(p, true);
    mount(p.e, v);
}

// The rider's client moves the boat it steers. Checked as vanilla's
// handleMoveVehicle: a move much longer than the speed allows, or into blocks, is
// undone (the client gets the boat's position back).
void Player::onVehicleMove(Reader& r) {
    double x = r.f64(), y = r.f64(), z = r.f64();
    float yaw = r.f32(), pitch = r.f32();
    bool onGround = r.boolean();
    if (!r.ok() || dead) return;
    Entity* v = srv->vehicleOf(e);
    if (!v || v->kind != EK_BOAT || v->passengers[0] != e.id) return;   // only a boat, by the one in front
    if (!isfinite(x) || !isfinite(y) || !isfinite(z) || !isfinite(yaw) || fabs(x) > 3.0e7 || fabs(z) > 3.0e7) {
        kick("Invalid move");
        return;
    }
    double dx = x - v->x, dy = y - v->y, dz = z - v->z;
    double moved = dx * dx + dy * dy + dz * dz;
    double speed = v->vx * v->vx + v->vy * v->vy + v->vz * v->vz;
    // (a chunk the server does not hold right now: the client has it and the server loads
    // it shortly; until then the move is not checked against it rather than refused, as
    // a missing chunk counts as solid)
    bool resident = true;
    for (int k = 0; k < 4; k++) {
        double ox = x + ((k & 1) ? 1 : -1) * v->width / 2, oz = z + ((k & 2) ? 1 : -1) * v->width / 2;
        resident &= srv->world.isResident(v->dim, (int)floor(ox) >> 4, (int)floor(oz) >> 4);
    }
    bool bad = moved - speed > 100.0 ||   // vanilla: more than 10 blocks off
               !srv->world.blockInBounds((int)floor(x), (int)floor(z)) ||
               (resident && mobs::boxCollides(*srv, x, y + 0.0625, z, v->width - 0.1f, v->height - 0.125f));
    if (bad) {
        Packet pk(pkt::s2c::VehicleMove);
        pk.w.f64(v->x);
        pk.w.f64(v->y);
        pk.w.f64(v->z);
        pk.w.f32(v->yaw);
        pk.w.f32(v->pitch);
        conn.send(pk);
        return;
    }
    v->vx = dx;
    v->vy = dy;
    v->vz = dz;
    v->x = x;
    v->y = y;
    v->z = z;
    v->yaw = v->headYaw = yaw;
    v->pitch = pitch;
    v->onGround = onGround;
    // the riders go with it; the player's view follows
    for (int i = 0; i < 2; i++) {
        Entity* rider = v->passengers[i] >= 0 ? srv->findEntity(v->passengers[i]) : nullptr;
        if (!rider) continue;
        rider->x = x;
        rider->y = y;
        rider->z = z;
        rider->fallDistance = 0;
    }
    lastX = x; lastY = y; lastZ = z;
    positionReady = true;
    int cx = (int)floor(x) >> 4, cz = (int)floor(z) >> 4;
    if (cx != centerCx || cz != centerCz) updateView(false);
}

// The paddles the rider moves (shown on the boat to everyone).
void Player::onSteerBoat(Reader& r) {
    bool left = r.boolean(), right = r.boolean();
    if (!r.ok()) return;
    Entity* v = srv->vehicleOf(e);
    if (!v || v->kind != EK_BOAT || v->passengers[0] != e.id) return;
    if (v->paddleLeft != left || v->paddleRight != right) {
        v->paddleLeft = left;
        v->paddleRight = right;
        v->metaDirty = true;
    }
}

// ------------------------------------------------------------------ damage
void Server::hitVehicle(Player& p, Entity& b, float damage) {
    if (b.removed) return;
    b.hurtDir = (int8_t)-b.hurtDir;
    b.hurtTicks = 10;
    b.vehicleDamage += damage * 10;
    b.metaDirty = true;
    vibration(b.x, b.y, b.z, GE_ENTITY_DAMAGE);
    if (p.gamemode == GM_CREATIVE) breakVehicle(b, false);
    else if (b.vehicleDamage > 40) {
        // a TNT minecart broken while moving primes instead (TntMinecart#destroy)
        if (b.kind == EK_MINECART && b.type == ent::TntMinecart && b.vx * b.vx + b.vz * b.vz >= 0.01)
            primeTntMinecart(b, (int)(plat::random32() % 20 + plat::random32() % 20));
        else breakVehicle(b, true);
    }
}

void Server::breakVehicle(Entity& b, bool drop) {
    if (b.removed) return;
    InDim in(*this, b.dim);
    for (int i = 0; i < 2; i++) {
        Entity* r = b.passengers[i] >= 0 ? findEntity(b.passengers[i]) : nullptr;
        if (r) dismount(*r, false);
        else b.passengers[i] = -1;
    }
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {   // nobody looks into it any more
        Player& p = players[i];
        if (p.inPlay() && p.winKind == WK_ENTITY_CONTAINER && p.winEntity == b.id) closeWindow(p, true);
    }
    for (ItemStack& st : b.cargo)
        if (!st.empty()) dropItem(b.x, b.y + 0.3, b.z, st);
    b.cargo.clear();
    uint16_t item = vehicleItem(b);
    if (drop && item) dropItem(b.x, b.y + 0.3, b.z, ItemStack::of(item));
    removeEntity(b);
}

// ------------------------------------------------------------------ tick
void Server::checkRiders(Entity& v) {
    for (int i = 0; i < 2; i++) {
        if (v.passengers[i] < 0) continue;
        Entity* r = findEntity(v.passengers[i]);
        if (!r || r->vehicle != v.id || r->dim != v.dim || (r->kind != EK_PLAYER && !r->alive())) {
            if (r && r->vehicle == v.id) r->vehicle = -1;
            v.passengers[i] = -1;
            if (i == 0 && v.passengers[1] >= 0) { v.passengers[0] = v.passengers[1]; v.passengers[1] = -1; }
            sendPassengers(v);
        }
    }
}

void Server::tickBoat(Entity& b) {
    if (b.removed) return;
    if (b.hurtTicks > 0 && --b.hurtTicks == 0) b.metaDirty = true;
    if (b.vehicleDamage > 0) b.vehicleDamage = b.vehicleDamage > 1 ? b.vehicleDamage - 1 : 0;
    if (b.y < dimVoidY(b.dim)) {
        breakVehicle(b, false);
        return;
    }
    checkRiders(b);
    Entity* front = b.passengers[0] >= 0 ? findEntity(b.passengers[0]) : nullptr;
    bool steered = front && front->kind == EK_PLAYER;
    if (!steered) {
        if (b.paddleLeft || b.paddleRight) {
            b.paddleLeft = b.paddleRight = false;
            b.metaDirty = true;
        }
        // where it is (AbstractBoat#getStatus): under water, in water, on land, in the air
        double hw = b.width / 2;
        int x0 = (int)floor(b.x - hw), x1 = (int)floor(b.x + hw);
        int z0 = (int)floor(b.z - hw), z1 = (int)floor(b.z + hw);
        double level = -1e9;
        bool inWater = false, under = false;
        for (int bx = x0; bx <= x1; bx++)
            for (int bz = z0; bz <= z1; bz++) {
                for (int by = (int)floor(b.y); by <= (int)floor(b.y + 0.001); by++) {
                    double wt = waterTop(*this, bx, by, bz);
                    if (wt < 0) continue;
                    if (wt > level) level = wt;
                    if (b.y < wt) inWater = true;
                }
                double top = waterTop(*this, bx, (int)floor(b.y + b.height), bz);
                if (top >= b.y + b.height) under = true;
            }
        double friction = 0;
        int frictionBlocks = 0;
        if (!inWater && !under)
            for (int bx = x0; bx <= x1; bx++)
                for (int bz = z0; bz <= z1; bz++) {
                    uint16_t st = blockAt(bx, (int)floor(b.y - 0.001), bz);
                    if (!stateCollides(st)) continue;
                    friction += blockFriction(st);
                    frictionBlocks++;
                }
        // AbstractBoat#floatBoat
        double gravity = -0.04, buoy = 0, inv = 0.05;
        if (under) { buoy = 0.01; inv = 0.45; }
        else if (inWater) { buoy = (level - b.y) / b.height; inv = 0.9; }
        else if (frictionBlocks) inv = friction / frictionBlocks;
        else inv = 0.9;   // in the air
        b.vx *= inv;
        b.vz *= inv;
        b.vy += gravity;
        if (buoy > 0) b.vy = (b.vy + buoy * 0.06153846) * 0.75;
        mobs::moveEntity(*this, b);
        if (frictionBlocks || b.onGround) b.vy = b.vy > 0 ? b.vy : 0;
    }
    // the riders sit in it (the clients place them; this keeps their range and chunk)
    for (int i = 0; i < 2; i++) {
        Entity* r = b.passengers[i] >= 0 ? findEntity(b.passengers[i]) : nullptr;
        if (!r || r->kind == EK_PLAYER) continue;
        r->x = b.x;
        r->y = b.y;
        r->z = b.z;
        r->vx = r->vy = r->vz = 0;
        r->yaw = r->headYaw = b.yaw;
    }
    // mobs that bump into it get in (AbstractBoat#push)
    if (b.passengers[seatsOf(b) - 1] < 0 && !steered) {
        for (int k = 0; k < MC_MAX_ENTITIES; k++) {
            Entity& m = entities[k];
            if (m.kind != EK_MOB || m.removed || !m.alive() || m.dim != b.dim || m.vehicle >= 0 || m.mountCooldown > 0) continue;
            if (m.width >= b.width || m.type == ent::Ghast || m.type == ent::EnderDragon) continue;
            double r = (b.width + m.width) / 2 + 0.2;
            if (fabs(m.x - b.x) > r || fabs(m.z - b.z) > r || m.y > b.y + b.height || m.y + m.height < b.y) continue;
            mount(m, b);
            break;
        }
    }
}

}  // namespace mc
