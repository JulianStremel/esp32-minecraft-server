// Minecarts (AbstractMinecart with vanilla's classic movement, OldMinecartBehavior: the
// new physics of 1.21 is an experiment, off by default).
//
// - Placed from the item on a rail; the rideable minecart takes one rider (a player
//   using it, or a mob it runs into while moving), sneaking gets out.
// - On rails it follows the track (moveAlongTrack): along the rail's two exits, down
//   slopes faster, at most 0.4 blocks a tick (0.2 in water), slowing by 0.997 a tick with
//   a rider and 0.96 without. Powered rails push it 0.06 a tick and kick it off a wall;
//   unpowered ones brake it. A rider's keys push it while it is slow. Off rails it rolls
//   and falls (comeOffTrack).
// - Detector rails give a signal while a minecart is on them; activator rails throw a
//   rider out, switch hopper minecarts off and prime TNT minecarts.
// - Chest (27 slots) and hopper (5 slots) minecarts open a container window; the hopper
//   minecart takes items lying above it and from the container block above it.
// - The furnace minecart burns coal or charcoal (3600 ticks each) and pushes itself away
//   from the player who fed it.
// - The TNT minecart explodes 80 ticks after an activator rail primes it (power 4, more
//   with speed); broken while moving or hurt by fire or an explosion, it primes at once
//   with a shorter fuse.
// - Hits break a minecart into its item as boats (vehicles.cpp).
#include <math.h>
#include <string.h>
#include "mc/registry.h"
#include "mc/server/automation.h"
#include "mc/server/mob_util.h"
#include "mc/server/rails.h"
#include "mc/server/server.h"

namespace mc {

namespace {
double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

bool waterAt(Server& s, double x, double y, double z) {
    uint16_t st = s.blockAt((int)floor(x), (int)floor(y + 0.1), (int)floor(z));
    return blockIdOf(st) == blk::Water || getBool(st, "waterlogged");
}

// AbstractMinecart#getPos: the point on the rail at (x, y, z) nearest to it
bool railPos(Server& s, double x, double y, double z, double& ox, double& oy, double& oz) {
    int i = (int)floor(x), j = (int)floor(y), k = (int)floor(z);
    if (rails::isRail(s.blockAt(i, j - 1, k))) --j;
    uint16_t st = s.blockAt(i, j, k);
    if (!rails::isRail(st)) return false;
    int a[3], b[3];
    rails::exits(rails::shapeOf(st), a, b);
    double x0 = i + 0.5 + a[0] * 0.5, y0 = j + 0.0625 + a[1] * 0.5, z0 = k + 0.5 + a[2] * 0.5;
    double x1 = i + 0.5 + b[0] * 0.5, y1 = j + 0.0625 + b[1] * 0.5, z1 = k + 0.5 + b[2] * 0.5;
    double ex = x1 - x0, ey = (y1 - y0) * 2, ez = z1 - z0;
    double t;
    if (ex == 0) t = z - k;
    else if (ez == 0) t = x - i;
    else t = ((x - x0) * ex + (z - z0) * ez) * 2;
    ox = x0 + ex * t;
    oy = y0 + ey * t;
    oz = z0 + ez * t;
    if (ey < 0) oy += 1;
    else if (ey > 0) oy += 0.5;
    return true;
}

// moves the cart by (dx, dz) with block collisions, per axis (Entity#move for what a
// minecart meets: walls stop it)
void slide(Server& s, Entity& e, double dx, double dz) {
    double nx = e.x + dx;
    if (!mobs::boxCollides(s, nx, e.y + 0.01, e.z, e.width, e.height - 0.02f)) e.x = nx;
    else e.vx = 0;
    double nz = e.z + dz;
    if (!mobs::boxCollides(s, e.x, e.y + 0.01, nz, e.width, e.height - 0.02f)) e.z = nz;
    else e.vz = 0;
}

uint16_t cartTypeForItem(uint16_t item) {
    if (item == itm::Minecart) return ent::Minecart;
    if (item == itm::ChestMinecart) return ent::ChestMinecart;
    if (item == itm::HopperMinecart) return ent::HopperMinecart;
    if (item == itm::TntMinecart) return ent::TntMinecart;
    if (item == itm::FurnaceMinecart) return ent::FurnaceMinecart;
    return 0xFFFF;
}

double maxSpeed(const Entity& e, bool water) {
    if (e.type == ent::FurnaceMinecart) return (water ? 3.0 : 4.0) / 20.0;
    return (water ? 4.0 : 8.0) / 20.0;
}
}  // namespace

// ------------------------------------------------------------------ placing, using
bool Server::useMinecartItem(Player& p, int x, int y, int z) {
    ItemStack& it = p.heldItem();
    uint16_t type = cartTypeForItem(it.id);
    if (type == 0xFFFF) return false;
    uint16_t st = blockAt(x, y, z);
    if (!rails::isRail(st)) return false;
    double yy = y + 0.0625 + (rails::ascending(rails::shapeOf(st)) ? 0.5 : 0);
    Entity* c = spawnEntity(EK_MINECART, type, x + 0.5, yy, z + 0.5);
    if (!c) return false;
    c->health = 1;
    c->fuse = -1;
    if (type == ent::ChestMinecart) c->cargo.resize(27);
    if (type == ent::HopperMinecart) c->cargo.resize(5);
    int sh = rails::shapeOf(st);
    c->yaw = c->headYaw = (sh == rails::EAST_WEST || sh == rails::ASC_EAST || sh == rails::ASC_WEST) ? 0 : 90;
    if (p.gamemode != GM_CREATIVE) {
        if (--it.count == 0) it.clear();
        p.sendSlot(SLOT_HOTBAR_START + p.held);
    }
    vibration(c->x, c->y, c->z, GE_ENTITY_PLACE);
    return true;
}

void Server::interactMinecart(Player& p, Entity& c) {
    if (c.type == ent::Minecart) {
        if (p.e.flags & EF_CROUCHING) return;
        if (p.sleeping) wakeUp(p);
        closeWindow(p, true);
        mount(p.e, c);
    } else if (c.type == ent::ChestMinecart || c.type == ent::HopperMinecart) {
        openEntityContainer(p, c);
    } else if (c.type == ent::FurnaceMinecart) {
        // MinecartFurnace#interact: coal or charcoal adds 3600 ticks (to at most 32000),
        // and it pushes away from the player
        ItemStack& h = p.heldItem();
        if ((h.id == itm::Coal || h.id == itm::Charcoal) && c.fuel + 3600 <= 32000) {
            if (p.gamemode != GM_CREATIVE) {
                if (--h.count == 0) h.clear();
                p.sendSlot(SLOT_HOTBAR_START + p.held);
            }
            c.fuel = (int16_t)(c.fuel + 3600);
        }
        if (c.fuel > 0) {
            c.pushX = (float)(c.x - p.e.x);
            c.pushZ = (float)(c.z - p.e.z);
        }
        c.metaDirty = true;
    }
}

void Server::primeTntMinecart(Entity& c, int fuse) {
    if (c.type != ent::TntMinecart || c.fuse >= 0) return;
    c.fuse = (int16_t)fuse;
    broadcastStatus(c, 10);   // the client flashes it
    playSound("entity.tnt.primed", c.x, c.y, c.z, 1, 1, 6);
}

bool Server::minecartOnDetector(int x, int y, int z) {
    // DetectorRailBlock#checkPressed: minecarts within (0.2 .. 0.8) of the block
    for (int k = 0; k < MC_MAX_ENTITIES; k++) {
        const Entity& e = entities[k];
        if (e.kind != EK_MINECART || e.removed || e.dim != curDim) continue;
        double hw = e.width / 2;
        if (e.x + hw <= x + 0.2 || e.x - hw >= x + 0.8 || e.z + hw <= z + 0.2 || e.z - hw >= z + 0.8) continue;
        if (e.y + e.height <= y || e.y >= y + 0.8) continue;
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ the track
static void moveAlongTrack(Server& s, Entity& e, int bx, int by, int bz, uint16_t st, bool water) {
    e.fallDistance = 0;
    double ox = e.x, oy = e.y, oz = e.z;
    double sx = 0, sy = 0, sz = 0;
    bool had = railPos(s, ox, oy, oz, sx, sy, sz);
    double y = by;
    bool powered = false, braking = false;
    if (blockIdOf(st) == blk::PoweredRail) {
        powered = getBool(st, "powered");
        braking = !powered;
    }
    double slope = water ? 0.0078125 * 0.2 : 0.0078125;
    int shape = rails::shapeOf(st);
    switch (shape) {
        case rails::ASC_EAST: e.vx -= slope; ++y; break;
        case rails::ASC_WEST: e.vx += slope; ++y; break;
        case rails::ASC_NORTH: e.vz += slope; ++y; break;
        case rails::ASC_SOUTH: e.vz -= slope; ++y; break;
        default: break;
    }
    int a[3], b[3];
    rails::exits(shape, a, b);
    double ex = b[0] - a[0], ez = b[2] - a[2];
    double len = sqrt(ex * ex + ez * ez);
    if (e.vx * ex + e.vz * ez < 0) {
        ex = -ex;
        ez = -ez;
    }
    double h = std::min(2.0, sqrt(e.vx * e.vx + e.vz * e.vz));
    e.vx = h * ex / len;
    e.vz = h * ez / len;
    // the rider's keys push it while it is slow (OldMinecartBehavior: the move intent)
    Entity* rider = e.passengers[0] >= 0 ? s.findEntity(e.passengers[0]) : nullptr;
    if (rider && rider->kind == EK_PLAYER) {
        const Player& p = s.players[rider->playerSlot];
        double fwd = ((p.inputs & 1) ? 1 : 0) - ((p.inputs & 2) ? 1 : 0);
        double side = ((p.inputs & 4) ? 1 : 0) - ((p.inputs & 8) ? 1 : 0);
        if ((fwd != 0 || side != 0) && e.vx * e.vx + e.vz * e.vz < 0.01) {
            double yaw = p.e.yaw * M_PI / 180.0;
            double ix = side * cos(yaw) - fwd * sin(yaw), iz = fwd * cos(yaw) + side * sin(yaw);
            double il = sqrt(ix * ix + iz * iz);
            e.vx += ix / il * 0.01;
            e.vz += iz / il * 0.01;
            braking = false;
        }
    }
    if (braking) {
        double hd = sqrt(e.vx * e.vx + e.vz * e.vz);
        if (hd < 0.03) e.vx = e.vy = e.vz = 0;
        else { e.vx *= 0.5; e.vz *= 0.5; e.vy = 0; }
    }
    // onto the line between the two exits
    double x0 = bx + 0.5 + a[0] * 0.5, z0 = bz + 0.5 + a[2] * 0.5;
    double x1 = bx + 0.5 + b[0] * 0.5, z1 = bz + 0.5 + b[2] * 0.5;
    double lx = x1 - x0, lz = z1 - z0, t;
    if (lx == 0) t = e.z - bz;
    else if (lz == 0) t = e.x - bx;
    else t = ((e.x - x0) * lx + (e.z - z0) * lz) * 2;
    e.x = x0 + lx * t;
    e.z = z0 + lz * t;
    e.y = y;
    double m = e.passengers[0] >= 0 ? 0.75 : 1.0;
    double ms = maxSpeed(e, water);
    slide(s, e, clampd(m * e.vx, -ms, ms), clampd(m * e.vz, -ms, ms));
    if (a[1] != 0 && (int)floor(e.x) - bx == a[0] && (int)floor(e.z) - bz == a[2]) e.y += a[1];
    else if (b[1] != 0 && (int)floor(e.x) - bx == b[0] && (int)floor(e.z) - bz == b[2]) e.y += b[1];
    // applyNaturalSlowdown (the furnace minecart pushes itself)
    if (e.type == ent::FurnaceMinecart) {
        double p2 = (double)e.pushX * e.pushX + (double)e.pushZ * e.pushZ;
        if (p2 > 1e-7) {
            double pl = sqrt(p2);
            e.pushX = (float)(e.pushX / pl);
            e.pushZ = (float)(e.pushZ / pl);
            e.vx = e.vx * 0.8 + e.pushX;
            e.vz = e.vz * 0.8 + e.pushZ;
        } else {
            e.vx *= 0.98;
            e.vz *= 0.98;
        }
    }
    double slow = e.passengers[0] >= 0 ? 0.997 : 0.96;
    e.vx *= slow;
    e.vz *= slow;
    e.vy = 0;
    if (water) {
        e.vx *= 0.95;
        e.vz *= 0.95;
    }
    // down a slope it gains what it lost in height
    double nx, ny, nz;
    if (had && railPos(s, e.x, e.y, e.z, nx, ny, nz)) {
        double dy = (sy - ny) * 0.05;
        double hd = sqrt(e.vx * e.vx + e.vz * e.vz);
        if (hd > 0) {
            e.vx *= (hd + dy) / hd;
            e.vz *= (hd + dy) / hd;
        }
        e.y = ny;
    }
    int fx = (int)floor(e.x), fz = (int)floor(e.z);
    if (fx != bx || fz != bz) {   // onto the next block: along the way it left
        double hd = sqrt(e.vx * e.vx + e.vz * e.vz);
        e.vx = hd * (fx - bx);
        e.vz = hd * (fz - bz);
    }
    if (e.type == ent::FurnaceMinecart) {   // the push turns with the track
        double d = e.vx * e.vx + e.vz * e.vz, p2 = (double)e.pushX * e.pushX + (double)e.pushZ * e.pushZ;
        if (p2 > 1e-4 && d > 0.001) {
            double dd = sqrt(d), pp = sqrt(p2);
            e.pushX = (float)(e.vx / dd * pp);
            e.pushZ = (float)(e.vz / dd * pp);
        }
    }
    if (powered) {
        double hd = sqrt(e.vx * e.vx + e.vz * e.vz);
        if (hd > 0.01) {
            e.vx += e.vx / hd * 0.06;
            e.vz += e.vz / hd * 0.06;
        } else if (shape == rails::EAST_WEST) {   // starting off a wall
            if (stateCollides(s.blockAt(bx - 1, by, bz)) && stateFaceSturdy(s.blockAt(bx - 1, by, bz), 5)) e.vx = 0.02;
            else if (stateCollides(s.blockAt(bx + 1, by, bz)) && stateFaceSturdy(s.blockAt(bx + 1, by, bz), 4)) e.vx = -0.02;
        } else if (shape == rails::NORTH_SOUTH) {
            if (stateCollides(s.blockAt(bx, by, bz - 1)) && stateFaceSturdy(s.blockAt(bx, by, bz - 1), 3)) e.vz = 0.02;
            else if (stateCollides(s.blockAt(bx, by, bz + 1)) && stateFaceSturdy(s.blockAt(bx, by, bz + 1), 2)) e.vz = -0.02;
        }
    }
}

static void comeOffTrack(Server& s, Entity& e, bool water) {
    double ms = maxSpeed(e, water);
    e.vx = clampd(e.vx, -ms, ms);
    e.vz = clampd(e.vz, -ms, ms);
    if (e.onGround) {
        e.vx *= 0.5;
        e.vy *= 0.5;
        e.vz *= 0.5;
    }
    mobs::moveEntity(s, e);
    if (!e.onGround) {
        e.vx *= 0.95;
        e.vy *= 0.95;
        e.vz *= 0.95;
    }
}

// ------------------------------------------------------------------ tick
void Server::tickMinecart(Entity& e) {
    if (e.removed) return;
    if (e.hurtTicks > 0 && --e.hurtTicks == 0) e.metaDirty = true;
    if (e.vehicleDamage > 0) e.vehicleDamage = e.vehicleDamage > 1 ? e.vehicleDamage - 1 : 0;
    if (e.y < dimVoidY(e.dim)) {
        breakVehicle(e, false);
        return;
    }
    checkRiders(e);
    // the TNT minecart's fuse
    if (e.type == ent::TntMinecart && e.fuse >= 0 && --e.fuse <= 0) {
        double speed = std::min(sqrt(e.vx * e.vx + e.vz * e.vz), 5.0);
        float power = (float)(4.0 + mobs::rng().unit() * 1.5 * speed);
        double x = e.x, y = e.y, z = e.z;
        breakVehicle(e, false);
        explode(x, y, z, power, -1, false);
        return;
    }
    if (e.type == ent::FurnaceMinecart && e.fuel > 0 && --e.fuel == 0) {
        e.pushX = e.pushZ = 0;
        e.metaDirty = true;
    }
    bool water = waterAt(*this, e.x, e.y, e.z);
    e.vy -= water ? 0.005 : 0.04;
    double px = e.x, pz = e.z;
    int bx = (int)floor(e.x), by = (int)floor(e.y), bz = (int)floor(e.z);
    if (rails::isRail(blockAt(bx, by - 1, bz))) --by;
    uint16_t st = blockAt(bx, by, bz);
    e.onRails = rails::isRail(st);
    if (e.onRails) {
        moveAlongTrack(*this, e, bx, by, bz, st, water);
        uint16_t id = blockIdOf(st);
        if (id == blk::ActivatorRail) {   // activateMinecart
            bool on = getBool(st, "powered");
            if (e.type == ent::Minecart && on) {
                for (int i = 0; i < 2; i++) {
                    Entity* r = e.passengers[i] >= 0 ? findEntity(e.passengers[i]) : nullptr;
                    if (r) dismount(*r);
                }
                if (e.hurtTicks == 0) {
                    e.hurtDir = (int8_t)-e.hurtDir;
                    e.hurtTicks = 10;
                    e.vehicleDamage = 50;
                    e.metaDirty = true;
                }
            } else if (e.type == ent::HopperMinecart) {
                e.hopperOn = !on;
            } else if (e.type == ent::TntMinecart && on) {
                primeTntMinecart(e, 80);
            }
        } else if (id == blk::DetectorRail && !getBool(st, "powered")) {
            setBlock(bx, by, bz, setBool(st, "powered", true));
            redstone.neighbours(*this, bx, by - 1, bz);
            scheduleTick(bx, by, bz, 20);
        }
    } else {
        comeOffTrack(*this, e, water);
    }
    // facing: along the way it moves, turned round when it reverses
    double dx = px - e.x, dz = pz - e.z;
    if (dx * dx + dz * dz > 0.001) {
        float yaw = (float)(atan2(dz, dx) * 180.0 / M_PI);
        if (e.inReverse) yaw += 180;
        float d = fmodf(yaw - e.yaw + 540.0f, 360.0f) - 180.0f;
        if (d < -170 || d >= 170) {
            yaw += 180;
            e.inReverse = !e.inReverse;
        }
        e.yaw = e.headYaw = fmodf(yaw + 360.0f, 360.0f);
    }
    // riders sit in it; a player's view follows
    for (int i = 0; i < 2; i++) {
        Entity* r = e.passengers[i] >= 0 ? findEntity(e.passengers[i]) : nullptr;
        if (!r) continue;
        r->x = e.x;
        r->y = e.y;
        r->z = e.z;
        r->fallDistance = 0;
        if (r->kind == EK_PLAYER) {
            Player& p = players[r->playerSlot];
            p.lastX = e.x; p.lastY = e.y; p.lastZ = e.z;
            int cx = (int)floor(e.x) >> 4, cz = (int)floor(e.z) >> 4;
            if (cx != p.centerCx || cz != p.centerCz) p.updateView(false);
        } else {
            r->vx = r->vy = r->vz = 0;
        }
    }
    // a moving rideable minecart picks up the mobs it runs into
    if (e.type == ent::Minecart && e.passengers[0] < 0 && e.vx * e.vx + e.vz * e.vz > 0.01) {
        for (int k = 0; k < MC_MAX_ENTITIES; k++) {
            Entity& m = entities[k];
            if (m.kind != EK_MOB || m.removed || !m.alive() || m.dim != e.dim || m.vehicle >= 0 || m.mountCooldown > 0) continue;
            if (m.type == ent::Ghast || m.type == ent::EnderDragon) continue;
            double r = (e.width + m.width) / 2 + 0.2;
            if (fabs(m.x - e.x) > r || fabs(m.z - e.z) > r || m.y > e.y + e.height || m.y + m.height < e.y) continue;
            mount(m, e);
            break;
        }
    }
    // the hopper minecart takes what lies above it and what the container above holds
    if (e.type == ent::HopperMinecart && e.hopperOn && e.cargo.size() == 5) {
        auto take = [&](ItemStack& from) -> bool {
            for (ItemStack& slot : e.cargo) {
                if (slot.empty()) {
                    slot = from;
                    slot.count = 1;
                } else if (slot.sameItem(from) && slot.count < maxStack(slot.id)) {
                    slot.count++;
                } else continue;
                if (--from.count == 0) from.clear();
                return true;
            }
            return false;
        };
        bool moved = false;
        PistonPos above{(int)floor(e.x), (int)floor(e.y + 1), (int)floor(e.z)};
        TileEntity* t = Automation::container(*this, above);
        if (t) {
            uint16_t aid = blockIdOf(blockAt(above.x, above.y, above.z));
            bool furnace = aid == blk::Furnace || aid == blk::BlastFurnace || aid == blk::Smoker;
            for (int i = furnace ? 2 : 0; i < t->slotCount() && !moved; i++)
                if (!t->items[i].empty()) moved = take(t->items[i]);
            if (moved) containerChanged(above.x, above.y, above.z);
        }
        for (int k = 0; k < MC_MAX_ENTITIES && !moved; k++) {
            Entity& it = entities[k];
            if (&it == &e) continue;
            if (it.kind != EK_ITEM || it.removed || it.dim != e.dim || it.item.empty()) continue;
            if (fabs(it.x - e.x) > 0.75 || fabs(it.z - e.z) > 0.75 || it.y < e.y || it.y > e.y + 1.7) continue;
            if (take(it.item)) {
                moved = true;
                if (it.item.empty()) removeEntity(it);
                else it.metaDirty = true;
            }
        }
        if (moved) entityContainerChanged(e);
    }
}

}  // namespace mc
