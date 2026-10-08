#include "mc/server/redstone.h"
#include "mc/server/books.h"
#include "mc/server/server.h"
#include "mc/server/piston.h"
#include "mc/server/automation.h"
#include "mc/registry.h"
#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace mc {
namespace {
const int order[] = {4, 5, 0, 1, 2, 3}; // Level.updateNeighborsAt
const char* const faces[] = {"down", "up", "north", "south", "west", "east"};
int facing(uint16_t st) {
    const char* p = getPropStr(st, "facing");
    for (int f = 0; f < 6; ++f)
        if (!strcmp(p, faces[f])) return f;
    return 2;
}
bool suffix(const char* n, const char* end) {
    size_t a = strlen(n), b = strlen(end);
    return a >= b && !strcmp(n + a - b, end);
}
// the eight copper bulbs (oxidation stages, waxed or not)
bool copperBulb(uint16_t st) { return suffix(blockOf(st).name, "copper_bulb"); }
// a mob head standing on a note block: it plays the mob's sound (1.20)
const char* headInstrument(uint16_t above) {
    switch (blockIdOf(above)) {
        case blk::ZombieHead: return "zombie";
        case blk::SkeletonSkull: return "skeleton";
        case blk::CreeperHead: return "creeper";
        case blk::DragonHead: return "dragon";
        case blk::WitherSkeletonSkull: return "wither_skeleton";
        case blk::PiglinHead: return "piglin";
        case blk::PlayerHead: return "custom_head";
        default: return nullptr;
    }
}
bool diode(uint16_t st) {
    return blockIdOf(st) == blk::Repeater || blockIdOf(st) == blk::Comparator;
}
bool torch(uint16_t st) {
    return blockIdOf(st) == blk::RedstoneTorch || blockIdOf(st) == blk::RedstoneWallTorch;
}
bool conductor(uint16_t st) {
    return stateConductsRedstone(st);
}
int attached(uint16_t st) { // direction from component toward its supporting block
    const char* f = getPropStr(st, "face");
    return !strcmp(f, "floor") ? 0 : !strcmp(f, "ceiling") ? 1 : facing(st) ^ 1;
}
bool source(uint16_t st) {
    return stateSignalSource(st);
}
int dust(uint16_t st) {
    return blockIdOf(st) == blk::RedstoneWire ? getProp(st, "power") : 0;
}
TileEntity* tile(Server& s, int x, int y, int z) {
    Chunk* c = s.world.get(s.curDim, x >> 4, z >> 4);
    return c ? c->tileAt(x & 15, y, z & 15) : nullptr;
}
int comparatorOutput(Server& s, int x, int y, int z) {
    TileEntity* t = tile(s, x, y, z);
    return t && t->type == TILE_COMPARATOR ? t->signal : 0;
}
} // namespace

Redstone::~Redstone() {
    plat::bigFree(work_);
    plat::bigFree(burns_);
    plat::bigFree(events_);
}
void Redstone::fail(Server&) {
    if (!failed_) {
        ++failures;
        MC_LOGE("Redstone stopped: update memory/cycle limit exceeded");
    }
    failed_ = true;
}
void Redstone::push(Server& s, uint8_t dim, int x, int y, int z, uint8_t kind) {
    if (failed_ || y < 0 || y >= WORLD_HEIGHT || !s.world.isResident(dim, x >> 4, z >> 4)) return;
    if (size_ == capacity_) {
        int cap = capacity_ ? capacity_ * 2 : 128;
        if (cap > 65536) {
            fail(s);
            return;
        }
        Update* p = (Update*)plat::bigAlloc(sizeof(Update) * cap);
        if (!p) {
            fail(s);
            return;
        }
        if (size_) memcpy(p, work_, size_ * sizeof(Update));
        plat::bigFree(work_);
        work_ = p;
        capacity_ = cap;
    }
    work_[size_++] = {x, z, (int16_t)y, dim, kind};
    highWater = std::max(highWater, (uint32_t)size_);
}
void Redstone::drain(Server& s) {
    if (draining_ || failed_) return;
    draining_ = true;
    uint32_t steps = 0;
    while (size_ && !failed_) {
        if (++steps > 1000000) {
            fail(s);
            break;
        }
        Update u = work_[--size_];
        Server::InDim in(s, u.dim);
        ++updates;
        if (u.kind == 1) {
            uint16_t st = s.blockAt(u.x, u.y, u.z);
            if (blockIdOf(st) == blk::Observer && !getBool(st, "powered")) s.scheduleTick(u.x, u.y, u.z, 2);
        } else
            neighbour(s, u.x, u.y, u.z);
    }
    draining_ = false;
}
void Redstone::neighbours(Server& s, int x, int y, int z) {
    for (int i = 5; i >= 0; --i) {
        int f = order[i];
        push(s, s.curDim, x + FACE_DX[f], y + FACE_DY[f], z + FACE_DZ[f]);
    }
    drain(s);
}
void Redstone::switchOutputChanged(Server& s, int x, int y, int z, uint16_t state) {
    // block_activate / block_deactivate (levers, buttons, plates)
    bool on = getBool(state, "powered") || getProp(state, "power") > 0;
    s.vibration(x + .5, y + .5, z + .5, on ? GE_OPEN : GE_CLOSE);
    neighbours(s, x, y, z);
    int f = attached(state);
    neighbours(s, x + FACE_DX[f], y + FACE_DY[f], z + FACE_DZ[f]);
}
void Redstone::changed(Server& s, uint8_t dim, int x, int y, int z, uint16_t old, uint16_t st, uint8_t flags) {
    Server::InDim in(s, dim);
    if (blockIdOf(old) != blockIdOf(st)) {
        Chunk* c = s.world.get(dim, x >> 4, z >> 4);
        TileEntity* t = c ? c->tileAt(x & 15, y, z & 15) : nullptr;
        if (t && t->type == TILE_LECTERN && getBool(old, "has_book") && !t->items[0].empty()) {
            int face = facing(old);
            s.dropItem(x + .5 + FACE_DX[face] * .25, y + 1, z + .5 + FACE_DZ[face] * .25, t->items[0]);
        }
        if (t && (t->type == TILE_COMPARATOR || t->type == TILE_PISTON || t->type == TILE_DAYLIGHT ||
                  t->type == TILE_LECTERN || t->type == TILE_SCULK || Automation::tileType(blockIdOf(old))))
            c->removeTile(x & 15, y, z & 15);
        if (c && blockIdOf(st) == blk::Comparator) c->addTile(TILE_COMPARATOR, x & 15, y, z & 15);
        uint8_t type = blockIdOf(st) == blk::DaylightDetector ? TILE_DAYLIGHT
                       : blockIdOf(st) == blk::Lectern        ? TILE_LECTERN
                       : sculkSensor(blockIdOf(st))           ? TILE_SCULK
                                                              : Automation::tileType(blockIdOf(st));
        if (c && type) {
            TileEntity* created = c->addTile(type, x & 15, y, z & 15);
            if (created) created->tickOrder = ++s.blockEntitySequence;
        }
        if (!(flags & 64) && blockIdOf(old) == blk::Tripwire)
            tripwireChanged(s, x, y, z, setBool(old, "powered", true));
        if (!(flags & 64) && blockIdOf(old) == blk::TripwireHook) tripwireHook(s, x, y, z, old, true);
        if (blockIdOf(st) == blk::Tripwire) tripwireChanged(s, x, y, z, st);
        if (blockIdOf(st) == blk::TripwireHook) tripwireHook(s, x, y, z, st);
    }
    auto around = [&](int cx, int cy, int cz) {
        if (!(flags & 1)) return;
        for (int j = 5; j >= 0; --j) {
            int g = order[j];
            push(s, dim, cx + FACE_DX[g], cy + FACE_DY[g], cz + FACE_DZ[g]);
        }
    };
    if (blockIdOf(old) == blk::RedstoneWire || blockIdOf(st) == blk::RedstoneWire) {
        // 1.16.5 RedStoneWireBlock uses a seven-position HashSet. Java HashMap
        // spreads Vec3i.hashCode into 16 buckets and retains insertion order within
        // each bucket here (no resize or treeification with seven entries).
        struct Position {
            int x, y, z;
            unsigned bucket;
        } p[7];
        for (int i = 0; i < 7; ++i) {
            int px = x + (i ? FACE_DX[i - 1] : 0);
            int py = y + (i ? FACE_DY[i - 1] : 0);
            int pz = z + (i ? FACE_DZ[i - 1] : 0);
            uint32_t h = (uint32_t)px + 31u * (uint32_t)pz + 961u * (uint32_t)py;
            p[i] = {px, py, pz, (h ^ (h >> 16)) & 15u};
        }
        // Reverse bucket/insertion order for the LIFO work list.
        for (int bucket = 15; bucket >= 0; --bucket)
            for (int i = 6; i >= 0; --i)
                if (p[i].bucket == (unsigned)bucket) around(p[i].x, p[i].y, p[i].z);
    } else if (torch(old) || torch(st)) {
        for (int f = 5; f >= 0; --f)
            around(x + FACE_DX[f], y + FACE_DY[f], z + FACE_DZ[f]);
    } else if (diode(old) || diode(st) || blockIdOf(old) == blk::Observer || blockIdOf(st) == blk::Observer) {
        uint16_t component = diode(st) || blockIdOf(st) == blk::Observer ? st : old;
        int f = facing(component) ^ 1;
        around(x + FACE_DX[f], y + FACE_DY[f], z + FACE_DZ[f]);
    } else if (blockIdOf(old) != blockIdOf(st) && !(flags & 64) && getBool(old, "powered") &&
               (blockIdOf(old) == blk::Lever || suffix(blockOf(old).name, "_button"))) {
        // Removing a powered control notifies its support. A raw state write
        // (including /setblock) does not execute the control's use callback.
        int f = attached(old);
        around(x + FACE_DX[f], y + FACE_DY[f], z + FACE_DZ[f]);
    } else if (blockIdOf(old) == blk::Lectern && blockIdOf(st) != blk::Lectern && getBool(old, "powered")) {
        around(x, y - 1, z);
    } else if (strstr(blockOf(old).name, "pressure_plate") || strstr(blockOf(st).name, "pressure_plate")) {
        around(x, y - 1, z);
    }
    for (int i = 5; i >= 0; --i) {
        int f = order[i];
        int nx = x + FACE_DX[f], ny = y + FACE_DY[f], nz = z + FACE_DZ[f];
        uint16_t n = s.blockAt(nx, ny, nz);
        if (!(flags & 16) && blockIdOf(n) == blk::Observer && facing(n) == (f ^ 1)) push(s, dim, nx, ny, nz, 1);
        if (flags & 1) push(s, dim, nx, ny, nz);
    }
    if (flags & 1) {
        // Lamp power is chosen during player placement. A raw state assignment
        // keeps the requested state until a real neighbouring block notifies it.
        if (blockIdOf(st) != blk::RedstoneLamp) push(s, dim, x, y, z);
        analogChanged(s, x, y, z);
    }
    drain(s);
}

int Redstone::directSignal(Server& s, int x, int y, int z, int d, bool wires) {
    uint16_t st = s.blockAt(x, y, z), id = blockIdOf(st);
    if (id == blk::RedstoneWire) {
        if (!wires || d == 0) return 0;
        return d == 1 || strcmp(getPropStr(wireShape(s, x, y, z, st), faces[d ^ 1]), "none") ? dust(st) : 0;
    }
    if (torch(st)) return d == 0 && getBool(st, "lit") ? 15 : 0;
    if (diode(st) || id == blk::Observer) {
        if (!getBool(st, "powered") || facing(st) != d) return 0;
        return id == blk::Comparator ? comparatorOutput(s, x, y, z) : 15;
    }
    if (id == blk::Lever || suffix(blockOf(st).name, "_button"))
        return getBool(st, "powered") && d == (attached(st) ^ 1) ? 15 : 0;
    if (strstr(blockOf(st).name, "pressure_plate"))
        return d == 1 ? (getProp(st, "power") >= 0 ? getProp(st, "power") : getBool(st, "powered") ? 15 : 0) : 0;
    if (id == blk::Lectern) return d == 1 && getBool(st, "powered") ? 15 : 0;
    if (id == blk::TripwireHook) return d == facing(st) && getBool(st, "powered") ? 15 : 0;
    if (id == blk::LightningRod) return d == facing(st) && getBool(st, "powered") ? 15 : 0;
    if (sculkSensor(id)) return d == 1 ? getProp(st, "power") : 0;   // strong only into the block below
    if (id == blk::TrappedChest) {
        TileEntity* t = tile(s, x, y, z);
        return d == 1 && t ? std::min(15, (int)t->signal) : 0;
    }
    return 0;
}
int Redstone::signal(Server& s, int x, int y, int z, int d, bool wires) {
    uint16_t st = s.blockAt(x, y, z), id = blockIdOf(st);
    int v = 0;
    if (id == blk::RedstoneBlock)
        v = 15;
    else if (torch(st)) {
        int excluded = id == blk::RedstoneTorch ? 1 : facing(st);
        v = getBool(st, "lit") && d != excluded ? 15 : 0;
    } else if (id == blk::Lever || suffix(blockOf(st).name, "_button") || id == blk::TripwireHook || id == blk::Lectern ||
               id == blk::LightningRod)
        v = getBool(st, "powered") ? 15 : 0;
    else if (id == blk::DaylightDetector || id == blk::Target)
        v = getProp(st, "power");
    else if (sculkSensor(id)) {   // a calibrated one gives nothing back into its input side
        bool input = id == blk::CalibratedSculkSensor && d == (facing(st) ^ 1);
        v = input ? 0 : getProp(st, "power");
    }
    else if (id == blk::TrappedChest) {
        TileEntity* t = tile(s, x, y, z);
        v = t ? std::min(15, (int)t->signal) : 0;
    } else if (strstr(blockOf(st).name, "pressure_plate"))
        v = getProp(st, "power") >= 0 ? getProp(st, "power") : getBool(st, "powered") ? 15 : 0;
    else
        v = directSignal(s, x, y, z, d, wires);
    if (conductor(st)) {
        for (int f = 0; f < 6 && v < 15; ++f)
            v = std::max(v, directSignal(s, x + FACE_DX[f], y + FACE_DY[f], z + FACE_DZ[f], f, wires));
    }
    return v;
}
int Redstone::bestSignal(Server& s, int x, int y, int z, bool wires) {
    int v = 0;
    for (int f = 0; f < 6 && v < 15; ++f)
        v = std::max(v, signal(s, x + FACE_DX[f], y + FACE_DY[f], z + FACE_DZ[f], f, wires));
    return v;
}
uint16_t Redstone::wireShape(Server& s, int x, int y, int z, uint16_t st) {
    bool connected[6] = {};
    const char* shape[6] = {};
    bool dot = true;
    for (int f = 2; f < 6; ++f)
        dot &= !strcmp(getPropStr(st, faces[f]), "none");
    for (int f = 2; f < 6; ++f) {
        int nx = x + FACE_DX[f], nz = z + FACE_DZ[f];
        uint16_t n = s.blockAt(nx, y, nz);
        bool link = source(n);
        if (blockIdOf(n) == blk::Repeater) link = (facing(n) / 2 == f / 2);
        if (blockIdOf(n) == blk::Observer) link = facing(n) == f;
        shape[f] = link ? "side" : "none";
        if (!conductor(s.blockAt(x, y + 1, z)) && (stateFaceSturdy(n, 1) || blockIdOf(n) == blk::Hopper) &&
            blockIdOf(s.blockAt(nx, y + 1, nz)) == blk::RedstoneWire)
            shape[f] = stateFaceSturdy(n, f ^ 1) ? "up" : "side";
        else if (!link && !conductor(n) && blockIdOf(s.blockAt(nx, y - 1, nz)) == blk::RedstoneWire)
            shape[f] = "side";
        connected[f] = strcmp(shape[f], "none") != 0;
    }
    bool ns = connected[2] || connected[3], ew = connected[4] || connected[5];
    if (!(dot && !ns && !ew)) {
        if (!ns) {
            if (!connected[4]) shape[4] = "side";
            if (!connected[5]) shape[5] = "side";
        }
        if (!ew) {
            if (!connected[2]) shape[2] = "side";
            if (!connected[3]) shape[3] = "side";
        }
    }
    for (int f = 2; f < 6; ++f)
        st = setPropStr(st, faces[f], shape[f]);
    return st;
}
int Redstone::sideInput(Server& s, int x, int y, int z, uint16_t st, bool diodesOnly) {
    int f = facing(st), v = 0;
    int a = f < 4 ? 4 : 2;
    for (int d = a; d < a + 2; ++d) {
        int nx = x + FACE_DX[d], nz = z + FACE_DZ[d];
        uint16_t n = s.blockAt(nx, y, nz);
        if (diodesOnly && !diode(n)) continue;
        if (!source(n)) continue;
        int p = blockIdOf(n) == blk::RedstoneBlock  ? 15
                : blockIdOf(n) == blk::RedstoneWire ? dust(n)
                                                    : directSignal(s, nx, y, nz, d);
        v = std::max(v, p);
    }
    return v;
}
int Redstone::analog(Server& s, int x, int y, int z) {
    uint16_t st = s.blockAt(x, y, z), id = blockIdOf(st);
    if (id == blk::Lectern) {
        TileEntity* t = tile(s, x, y, z);
        return getBool(st, "has_book") && t && t->type == TILE_LECTERN ? Books::comparator(*t) : 0;
    }
    if (id == blk::Cake) return (7 - getProp(st, "bites")) * 2;
    // 1.17+: an empty cauldron, and filled ones with their own blocks (level 1..3 is index 0..2)
    if (id == blk::Cauldron) return 0;
    if (id == blk::WaterCauldron || id == blk::PowderSnowCauldron) return getProp(st, "level") + 1;
    if (id == blk::LavaCauldron) return 3;
    if (id == blk::Composter) return getProp(st, "level");
    if (id == blk::EndPortalFrame) return getBool(st, "eye") ? 15 : 0;
    if (id == blk::RespawnAnchor) return getProp(st, "charges") * 15 / 4;
    if (id == blk::Beehive || id == blk::BeeNest) return getProp(st, "honey_level");
    if (copperBulb(st)) return getBool(st, "lit") ? 15 : 0;
    if (sculkSensor(id)) {   // the frequency of the last vibration
        TileEntity* t = tile(s, x, y, z);
        return t && t->type == TILE_SCULK ? t->frequency : 0;
    }
    if (id == blk::ChiseledBookshelf) {   // the slot last put in or taken from, 1..6
        TileEntity* t = tile(s, x, y, z);
        return t && t->type == TILE_BOOKSHELF ? t->lastSlot + 1 : 0;
    }
    if (id == blk::Crafter) {   // slots that hold an item or are disabled
        TileEntity* t = tile(s, x, y, z);
        int n = 0;
        if (t && t->type == TILE_CRAFTER)
            for (int i = 0; i < 9; i++) n += !t->items[i].empty() || (t->disabledSlots & (1u << i));
        return n;
    }
    if (!Automation::tileType(id)) return -1;
    TileEntity* t = tile(s, x, y, z);
    if (!t) return 0;
    TileEntity* partner = nullptr;
    if (id == blk::Chest || id == blk::TrappedChest) {
        if (conductor(s.blockAt(x, y + 1, z))) return 0;
        const char* type = getPropStr(st, "type");
        if (strcmp(type, "single")) {
            int f = facing(st);
            int clockwise = f == 2 ? 5 : f == 5 ? 3 : f == 3 ? 4 : 2;
            int d = !strcmp(type, "left") ? clockwise : clockwise ^ 1;
            int px = x + FACE_DX[d], pz = z + FACE_DZ[d];
            uint16_t other = s.blockAt(px, y, pz);
            if (blockIdOf(other) == id && facing(other) == f && strcmp(getPropStr(other, "type"), type) &&
                strcmp(getPropStr(other, "type"), "single")) {
                if (conductor(s.blockAt(px, y + 1, pz))) return 0;
                partner = tile(s, px, y, pz);
            }
        }
    }
    float fullness = 0;
    int occupied = 0, count = t->slotCount() + (partner ? partner->slotCount() : 0);
    for (TileEntity* inv : {t, partner})
        if (inv) {
            for (int i = 0; i < inv->slotCount(); ++i)
                if (!inv->items[i].empty()) {
                    fullness += (float)inv->items[i].count / maxStack(inv->items[i].id);
                    ++occupied;
                }
        }
    return (int)(fullness / count * 14) + (occupied ? 1 : 0);
}
void Redstone::analogChanged(Server& s, int x, int y, int z) {
    for (int f = 2; f < 6; ++f) {
        int nx = x + FACE_DX[f], nz = z + FACE_DZ[f];
        uint16_t st = s.blockAt(nx, y, nz);
        if (conductor(st)) {
            nx += FACE_DX[f];
            nz += FACE_DZ[f];
            st = s.blockAt(nx, y, nz);
        }
        if (blockIdOf(st) == blk::Comparator) push(s, s.curDim, nx, y, nz);
    }
    drain(s);
}

int Redstone::input(Server& s, int x, int y, int z, uint16_t st) {
    int f = facing(st), nx = x + FACE_DX[f], nz = z + FACE_DZ[f];
    uint16_t n = s.blockAt(nx, y, nz);
    int v = std::max(signal(s, nx, y, nz, f), dust(n));
    if (blockIdOf(st) == blk::Comparator) {
        int a = analog(s, nx, y, nz);
        if (a >= 0)
            v = a;
        else if (v < 15 && conductor(n)) {
            a = analog(s, nx + FACE_DX[f], y, nz + FACE_DZ[f]);
            if (a >= 0) v = a;
        }
    }
    return v;
}
void Redstone::output(Server& s, int x, int y, int z, uint16_t st) {
    // Updates are retained on the explicit stack, rather than recursing on the
    // ESP32 task stack. Callbacks must not depend on work after a nested write.
    s.world.setBlock(s.curDim, x, y, z, st);
}
void Redstone::neighbour(Server& s, int x, int y, int z) {
    uint16_t pistonState = s.blockAt(x, y, z);
    if (blockIdOf(pistonState) == blk::Piston || blockIdOf(pistonState) == blk::StickyPiston) {
        Pistons::changed(s, {x, y, z}, pistonState);
        return;
    }
    uint16_t st = s.blockAt(x, y, z), id = blockIdOf(st);
    if (id == blk::Tnt) {
        if (bestSignal(s, x, y, z) > 0) s.primeTnt(x, y, z);
    } else if (id == blk::Dropper || id == blk::Dispenser) {
        bool power = bestSignal(s, x, y, z) > 0 || bestSignal(s, x, y + 1, z) > 0;
        if (power && !getBool(st, "triggered")) {
            s.scheduleTick(x, y, z, 4);
            s.world.setBlock(s.curDim, x, y, z, setBool(st, "triggered", true), true, 4);
        } else if (!power && getBool(st, "triggered"))
            s.world.setBlock(s.curDim, x, y, z, setBool(st, "triggered", false), true, 4);
    } else if (id == blk::Tripwire) {
        uint16_t shaped = st;
        for (int f = 2; f < 6; ++f) {
            uint16_t n = s.blockAt(x + FACE_DX[f], y, z + FACE_DZ[f]);
            shaped =
                setBool(shaped, faces[f],
                        blockIdOf(n) == blk::Tripwire || (blockIdOf(n) == blk::TripwireHook && facing(n) == (f ^ 1)));
        }
        if (shaped != st) output(s, x, y, z, shaped);
    } else if (id == blk::RedstoneWire) {
        uint16_t shape = wireShape(s, x, y, z, st);
        int power = bestSignal(s, x, y, z, false), near = 0;
        for (int f = 2; f < 6 && power < 15; ++f) {
            int nx = x + FACE_DX[f], nz = z + FACE_DZ[f];
            uint16_t n = s.blockAt(nx, y, nz);
            near = std::max(near, dust(n));
            if (conductor(n) && !conductor(s.blockAt(x, y + 1, z)))
                near = std::max(near, dust(s.blockAt(nx, y + 1, nz)));
            else if (!conductor(n))
                near = std::max(near, dust(s.blockAt(nx, y - 1, nz)));
        }
        output(s, x, y, z, setProp(shape, "power", std::max(power, near - 1)));
    } else if (torch(st)) {
        int f = id == blk::RedstoneTorch ? 0 : facing(st) ^ 1;
        bool powered = signal(s, x + FACE_DX[f], y + FACE_DY[f], z + FACE_DZ[f], f) > 0;
        if (getBool(st, "lit") == powered && !s.willTickThisTick(x, y, z, id)) s.scheduleTick(x, y, z, 2);
    } else if (diode(st)) {
        if (s.willTickThisTick(x, y, z, id)) return;
        int f = facing(st), out = f ^ 1;
        uint16_t front = s.blockAt(x + FACE_DX[out], y, z + FACE_DZ[out]);
        bool priority = diode(front) && facing(front) != out;
        if (id == blk::Repeater) {
            bool locked = sideInput(s, x, y, z, st, true) > 0;
            if (locked != getBool(st, "locked")) output(s, x, y, z, setBool(st, "locked", locked));
            if (!locked && getBool(st, "powered") != (input(s, x, y, z, st) > 0))
                s.scheduleTick(x, y, z, (getProp(st, "delay") + 1) * 2,
                               priority                 ? -3
                               : getBool(st, "powered") ? -2
                                                        : -1);
        } else {
            int in = input(s, x, y, z, st), side = sideInput(s, x, y, z, st, false);
            bool subtract = !strcmp(getPropStr(st, "mode"), "subtract");
            int v = subtract ? std::max(in - side, 0) : in;
            bool powered = in > 0 && (in > side || (in == side && !subtract));
            if (v != comparatorOutput(s, x, y, z) || powered != getBool(st, "powered"))
                s.scheduleTick(x, y, z, 2, priority ? -1 : 0);
        }
    } else if (copperBulb(st)) {
        // toggles on every rising edge, at once (CopperBulbBlock#checkAndFlip)
        bool powered = bestSignal(s, x, y, z) > 0;
        if (powered != getBool(st, "powered")) {
            uint16_t next = st;
            if (powered) {
                next = setBool(next, "lit", !getBool(st, "lit"));
                s.playSound(getBool(next, "lit") ? "block.copper_bulb.turn_on" : "block.copper_bulb.turn_off", x + .5, y + .5,
                            z + .5, 1, 1, 4);
            }
            output(s, x, y, z, setBool(next, "powered", powered));
            if (getBool(next, "lit") != getBool(st, "lit")) analogChanged(s, x, y, z);
        }
    } else if (id == blk::Crafter) {
        // crafts 4 ticks after a rising edge (CrafterBlock#neighborChanged)
        bool powered = bestSignal(s, x, y, z) > 0;
        if (powered && !getBool(st, "triggered")) {
            TileEntity* t = Automation::container(s, {x, y, z});
            if (t && t->type == TILE_CRAFTER) t->craftPending = true;
            s.scheduleTick(x, y, z, 4);
            s.world.setBlock(s.curDim, x, y, z, setBool(st, "triggered", true), true, 2);
        } else if (!powered && getBool(st, "triggered")) {
            s.world.setBlock(s.curDim, x, y, z, setBool(setBool(st, "triggered", false), "crafting", false), true, 2);
        }
    } else if (id == blk::NoteBlock) {
        const char* head = headInstrument(s.blockAt(x, y + 1, z));
        st = head ? setPropStr(st, "instrument", head) : setProp(st, "instrument", stateNoteInstrument(s.blockAt(x, y - 1, z)));
        bool powered = bestSignal(s, x, y, z) > 0;
        if (powered && !getBool(st, "powered")) playNote(s, x, y, z);
        output(s, x, y, z, setBool(st, "powered", powered));
    } else if (id == blk::RedstoneLamp) {
        bool powered = bestSignal(s, x, y, z) > 0;
        if (powered && !getBool(st, "lit"))
            s.world.setBlock(s.curDim, x, y, z, setBool(st, "lit", true), true, 2);
        else if (!powered && getBool(st, "lit"))
            s.scheduleTick(x, y, z, 4);
    } else {
        const char* n = blockOf(st).name;
        bool door = suffix(n, "_door"), gate = suffix(n, "_fence_gate"), trap = suffix(n, "_trapdoor");
        if (!door && !gate && !trap && id != blk::Hopper) return;
        bool powered = bestSignal(s, x, y, z) > 0;
        int oy = door ? (!strcmp(getPropStr(st, "half"), "lower") ? y + 1 : y - 1) : y;
        if (door) powered |= bestSignal(s, x, oy, z) > 0;
        if (id == blk::Hopper)
            output(s, x, y, z, setBool(st, "enabled", !powered));
        else if (powered != getBool(st, "powered")) {
            output(s, x, y, z, setBool(setBool(st, "powered", powered), "open", powered));
            if (door) {
                uint16_t other = s.blockAt(x, oy, z);
                if (blockIdOf(other) == id)
                    output(s, x, oy, z, setBool(setBool(other, "powered", powered), "open", powered));
            }
        }
    }
}
bool Redstone::burnedOut(Server& s, int x, int y, int z, bool add) {
    uint32_t now = s.worldTick();
    int n = 0, matches = 0;
    for (int i = 0; i < burnCount_; ++i) {
        Burn b = burns_[i];
        if (now - b.tick > 60) continue;
        burns_[n++] = b;
        if (b.dim == s.curDim && b.x == x && b.y == y && b.z == z) ++matches;
    }
    burnCount_ = n;
    if (add) {
        if (n == burnCapacity_) {
            int cap = burnCapacity_ ? burnCapacity_ * 2 : 32;
            Burn* p = (Burn*)plat::bigAlloc(cap * sizeof(Burn));
            if (!p) {
                fail(s);
                return true;
            }
            if (n) memcpy(p, burns_, n * sizeof(Burn));
            plat::bigFree(burns_);
            burns_ = p;
            burnCapacity_ = cap;
        }
        burns_[burnCount_++] = {x, z, now, (int16_t)y, s.curDim};
        ++matches;
    }
    return matches >= 8;
}
bool Redstone::tick(Server& s, const TimerEvent& ev) {
    int x = ev.key.x, y = ev.key.y, z = ev.key.z;
    uint16_t st = s.blockAt(x, y, z), id = blockIdOf(st);
    if (id == blk::Dropper || id == blk::Dispenser) {
        Automation::dispense(s, {x, y, z});
    } else if (id == blk::Lectern) {
        s.setBlock(x, y, z, setBool(st, "powered", false));
        neighbours(s, x, y - 1, z);
    } else if (id == blk::TripwireHook) {
        tripwireHook(s, x, y, z, st);
    } else if (id == blk::Tripwire) {
        if (getBool(st, "powered")) tripwire(s, x, y, z, st);
    } else if (strstr(blockOf(st).name, "pressure_plate")) {
        pressurePlate(s, x, y, z, st);
    } else if (suffix(blockOf(st).name, "_button")) {
        button(s, x, y, z, st);
    } else if (id == blk::Target) {
        if (getProp(st, "power")) output(s, x, y, z, setProp(st, "power", 0));
    } else if (id == blk::LightningRod) {
        if (getBool(st, "powered")) output(s, x, y, z, setBool(st, "powered", false));
    } else if (sculkSensor(id)) {
        sculkTick(s, x, y, z, st);
    } else if (id == blk::Crafter) {
        TileEntity* t = tile(s, x, y, z);
        if (t && t->type == TILE_CRAFTER && t->craftPending) {
            t->craftPending = false;
            Automation::craft(s, {x, y, z});
        } else if (getBool(st, "crafting")) {
            s.world.setBlock(s.curDim, x, y, z, setBool(st, "crafting", false), true, 2);
        }
    } else if (id == blk::Repeater) {
        if (sideInput(s, x, y, z, st, true) > 0) return true;
        bool powered = getBool(st, "powered"), in = input(s, x, y, z, st) > 0;
        if (powered && !in)
            output(s, x, y, z, setBool(st, "powered", false));
        else if (!powered) {
            output(s, x, y, z, setBool(st, "powered", true));
            if (!in) s.scheduleTick(x, y, z, (getProp(st, "delay") + 1) * 2, -2);
        }
    } else if (id == blk::Comparator) {
        int in = input(s, x, y, z, st), side = sideInput(s, x, y, z, st, false);
        bool subtract = !strcmp(getPropStr(st, "mode"), "subtract");
        int value = subtract ? std::max(in - side, 0) : in;
        bool powered = in > 0 && (in > side || (in == side && !subtract));
        TileEntity* t = tile(s, x, y, z);
        if (!t || t->type != TILE_COMPARATOR) {
            // Worlds written before redstone support can contain a comparator
            // state without a block entity. Materialize its output lazily.
            Chunk* c = s.world.get(s.curDim, x >> 4, z >> 4);
            t = c ? c->addTile(TILE_COMPARATOR, x & 15, y, z & 15) : nullptr;
            if (!t) {
                fail(s);
                return true;
            }
        }
        int old = t->signal;
        t->signal = (uint8_t)value;
        s.world.markDirty(s.curDim, x >> 4, z >> 4);
        if (old != value || !subtract) {
            output(s, x, y, z, setBool(st, "powered", powered));
            neighbours(s, x, y, z);
        }
    } else if (id == blk::Observer) {
        bool powered = getBool(st, "powered");
        output(s, x, y, z, setBool(st, "powered", !powered));
        if (!powered) s.scheduleTick(x, y, z, 2);
    } else if (torch(st)) {
        int f = id == blk::RedstoneTorch ? 0 : facing(st) ^ 1;
        bool powered = signal(s, x + FACE_DX[f], y + FACE_DY[f], z + FACE_DZ[f], f) > 0;
        if (getBool(st, "lit") && powered) {
            output(s, x, y, z, setBool(st, "lit", false));
            if (burnedOut(s, x, y, z, true)) s.scheduleTick(x, y, z, 160);
        } else if (!getBool(st, "lit") && !powered && !burnedOut(s, x, y, z, false))
            output(s, x, y, z, setBool(st, "lit", true));
    } else if (id == blk::RedstoneLamp) {
        if (getBool(st, "lit") && bestSignal(s, x, y, z) == 0)
            s.world.setBlock(s.curDim, x, y, z, setBool(st, "lit", false), true, 2);
    } else
        return false;
    return true;
}

void Redstone::blockEvent(Server& s, int x, int y, int z, uint16_t block, uint8_t type, uint8_t data) {
    if (failed_ || y < 0 || y >= WORLD_HEIGHT || !s.world.isResident(s.curDim, x >> 4, z >> 4)) return;
    for (int i = eventHead_; i < eventCount_; ++i) {
        const BlockEvent& e = events_[i];
        if (e.dim == s.curDim && e.x == x && e.y == y && e.z == z && e.block == block && e.type == type &&
            e.data == data)
            return;
    }
    if (eventCount_ == eventCapacity_) {
        if (eventHead_) {
            memmove(events_, events_ + eventHead_, (eventCount_ - eventHead_) * sizeof(BlockEvent));
            eventCount_ -= eventHead_;
            eventHead_ = 0;
        } else {
            int cap = eventCapacity_ ? eventCapacity_ * 2 : 32;
            if (cap > 65536) {
                fail(s);
                return;
            }
            BlockEvent* p = (BlockEvent*)plat::bigAlloc(sizeof(BlockEvent) * cap);
            if (!p) {
                fail(s);
                return;
            }
            if (eventCount_) memcpy(p, events_, eventCount_ * sizeof(BlockEvent));
            plat::bigFree(events_);
            events_ = p;
            eventCapacity_ = cap;
        }
    }
    events_[eventCount_++] = {x, z, (int16_t)y, block, s.curDim, type, data};
}
void Redstone::playNote(Server& s, int x, int y, int z) {
    uint16_t above = s.blockAt(x, y + 1, z);
    if (stateIsAir(above) || headInstrument(above)) {
        blockEvent(s, x, y, z, blk::NoteBlock, 0, 0);
        s.vibration(x + .5, y + .5, z + .5, GE_OPEN);   // note_block_play
    }
}
bool Redstone::pinsChunk(uint8_t dim, int cx, int cz) const {
    for (int i = eventHead_; i < eventCount_; ++i) {
        const BlockEvent& e = events_[i];
        if (e.dim == dim && (e.x >> 4) == cx && (e.z >> 4) == cz) return true;
    }
    return false;
}
void Redstone::runBlockEvents(Server& s) {
    uint32_t processed = 0;
    while (eventHead_ < eventCount_ && !failed_) {
        if (++processed > 65536) {
            fail(s);
            break;
        }
        // Remove before invoking the handler: it can enqueue the same event again.
        BlockEvent e = events_[eventHead_++];
        Server::InDim in(s, e.dim);
        uint16_t st = s.blockAt(e.x, e.y, e.z);
        if (blockIdOf(st) != e.block) continue;
        bool handled = false;
        if (e.block == blk::NoteBlock) {
            char sound[64];
            snprintf(sound, sizeof(sound), "block.note_block.%s", getPropStr(st, "instrument"));
            s.playSound(sound, e.x + 0.5, e.y + 0.5, e.z + 0.5, 3.0f, powf(2.0f, (getProp(st, "note") - 12) / 12.0f),
                        2);
            handled = true;
        } else if (e.block == blk::Piston || e.block == blk::StickyPiston) {
            handled = Pistons::event(s, {e.x, e.y, e.z}, st, e.type, e.data);
        }
        if (!handled) continue;
        ++blockEventsExecuted;
        Packet pk(pkt::s2c::BlockAction);
        pk.w.u64(packPos(e.x, e.y, e.z));
        pk.w.u8(e.type);
        pk.w.u8(e.data);
        pk.w.varint(e.block);
        s.broadcastNear(pk, e.x >> 4, e.z >> 4);
    }
    if (eventHead_ == eventCount_) eventHead_ = eventCount_ = 0;
}
} // namespace mc
