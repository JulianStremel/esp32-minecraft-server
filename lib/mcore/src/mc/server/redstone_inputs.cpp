#include "mc/server/redstone.h"
#include "mc/server/server.h"
#include "mc/server/piston.h"
#include "mc/registry.h"
#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace mc {
namespace {
bool stoneButton(uint16_t state) {
    int id = blockIdOf(state);
    return id == blk::StoneButton || id == blk::PolishedBlackstoneButton;
}
bool ignoresTriggers(Server& s, const Entity& e) {
    return e.removed || e.kind == EK_NONE || e.dim != s.curDim ||
           (e.kind == EK_PLAYER && e.playerSlot >= 0 && e.playerSlot < MC_MAX_PLAYERS &&
            s.players[e.playerSlot].gamemode == GM_SPECTATOR);
}
bool intersects(const Entity& e, const PistonBox& box) {
    PistonBox eb{{e.x - e.width * .5, e.y, e.z - e.width * .5},
                 {e.x + e.width * .5, e.y + e.height, e.z + e.width * .5}};
    return eb.intersects(box);
}
template <class F> int countEntities(Server& s, const PistonBox& box, F accepts) {
    int count = 0;
    for (const Entity& e : s.entities)
        if (!ignoresTriggers(s, e) && accepts(e) && intersects(e, box)) ++count;
    for (const Player& p : s.players)
        if (p.inPlay() && !ignoresTriggers(s, p.e) && accepts(p.e) && intersects(p.e, box)) ++count;
    return count;
}
PistonBox buttonBox(uint16_t state, int x, int y, int z) {
    const char* facing = getPropStr(state, "facing");
    int f = !strcmp(facing, "north") ? 2 : !strcmp(facing, "south") ? 3 : !strcmp(facing, "west") ? 4 : 5;
    const char* attach = getPropStr(state, "face");
    double depth = getBool(state, "powered") ? 1.0 / 16 : 2.0 / 16;
    PistonBox b{{5.0 / 16, 6.0 / 16, 5.0 / 16}, {11.0 / 16, 10.0 / 16, 11.0 / 16}};
    if (!strcmp(attach, "wall")) {
        int axis = f < 4 ? 2 : 0;
        b.lo[axis] = f & 1 ? 0 : 1 - depth;
        b.hi[axis] = f & 1 ? depth : 1;
    } else {
        bool floor = !strcmp(attach, "floor");
        b.lo[1] = floor ? 0 : 1 - depth;
        b.hi[1] = floor ? depth : 1;
        int narrow = f < 4 ? 2 : 0;
        b.lo[narrow] = 6.0 / 16;
        b.hi[narrow] = 10.0 / 16;
    }
    for (int a = 0; a < 3; ++a) {
        double shift = a == 0 ? x : a == 1 ? y : z;
        b.lo[a] += shift;
        b.hi[a] += shift;
    }
    return b;
}
} // namespace
void Redstone::pressurePlate(Server& s, int x, int y, int z, uint16_t state) {
    int id = blockIdOf(state);
    bool weighted = id == blk::LightWeightedPressurePlate || id == blk::HeavyWeightedPressurePlate;
    bool livingOnly = id == blk::StonePressurePlate || id == blk::PolishedBlackstonePressurePlate;
    int count = countEntities(s, {{x + .125, y + 0.0, z + .125}, {x + .875, y + .25, z + .875}},
                              [&](const Entity& e) { return !livingOnly || e.kind == EK_PLAYER || e.kind == EK_MOB; });
    int max = id == blk::HeavyWeightedPressurePlate ? 150 : 15;
    int power = weighted ? (std::min(max, count) * 15 + max - 1) / max : count ? 15 : 0;
    int previous = weighted ? getProp(state, "power") : getBool(state, "powered") ? 15 : 0;
    if (power != previous) {
        uint16_t next = weighted ? setProp(state, "power", power) : setBool(state, "powered", power > 0);
        s.world.setBlock(s.curDim, x, y, z, next, true, 2);
        neighbours(s, x, y, z);
        neighbours(s, x, y - 1, z);
        if ((power > 0) != (previous > 0)) {
            const char* material = weighted ? "metal" : livingOnly ? "stone" : "wooden";
            char sound[80];
            snprintf(sound, sizeof(sound), "block.%s_pressure_plate.click_%s", material, power ? "on" : "off");
            s.playSound(sound, x + .5, y + .1, z + .5, .3f,
                        weighted     ? (power ? .9f : .75f)
                        : livingOnly ? (power ? .6f : .5f)
                                     : (power ? .8f : .7f),
                        4);
        }
    }
    if (power) s.scheduleTick(x, y, z, weighted ? 10 : 20);
}
void Redstone::button(Server& s, int x, int y, int z, uint16_t state) {
    bool arrow = !stoneButton(state) &&
                 countEntities(s, buttonBox(state, x, y, z), [](const Entity& e) { return e.kind == EK_ARROW; }) > 0;
    if (arrow != getBool(state, "powered")) {
        output(s, x, y, z, setBool(state, "powered", arrow));
        switchOutputChanged(s, x, y, z, state);
        char sound[80];
        snprintf(sound, sizeof(sound), "block.%s_button.click_%s", stoneButton(state) ? "stone" : "wooden",
                 arrow ? "on" : "off");
        s.playSound(sound, x + .5, y + .5, z + .5, .3f, arrow ? .6f : .5f, 4);
    }
    if (arrow) s.scheduleTick(x, y, z, 30);
}
void Redstone::entityInside(Server& s, const Entity& e) {
    if (ignoresTriggers(s, e)) return;
    double half = e.width * .5;
    for (int x = (int)floor(e.x - half + .001); x <= (int)floor(e.x + half - .001); ++x)
        for (int y = (int)floor(e.y + .001); y <= (int)floor(e.y + e.height - .001); ++y)
            for (int z = (int)floor(e.z - half + .001); z <= (int)floor(e.z + half - .001); ++z) {
                uint16_t state = s.blockAt(x, y, z);
                const char* name = blockOf(state).name;
                if (strstr(name, "pressure_plate") && !getBool(state, "powered") && getProp(state, "power") <= 0)
                    pressurePlate(s, x, y, z, state);
                else if (e.kind == EK_ARROW && strstr(name, "_button") && !stoneButton(state) &&
                         !getBool(state, "powered"))
                    button(s, x, y, z, state);
                else if (blockIdOf(state) == blk::Tripwire && !getBool(state, "powered"))
                    tripwire(s, x, y, z, state);
            }
}
void Redstone::targetHit(Server& s, int x, int y, int z, int face, double hx, double hy, double hz, bool arrow) {
    uint16_t state = s.blockAt(x, y, z);
    if (blockIdOf(state) != blk::Target || face < 0 || face > 5 ||
        s.timers.pending(TimerKey::block(x, y, z, blk::Target, s.curDim)))
        return;
    double dx = fabs(hx - floor(hx) - .5), dy = fabs(hy - floor(hy) - .5), dz = fabs(hz - floor(hz) - .5);
    double offset = face < 2 ? std::max(dx, dz) : face < 4 ? std::max(dx, dy) : std::max(dy, dz);
    int power = std::max(1, (int)ceil(15 * std::max(0.0, std::min(1.0, 1 - offset * 2))));
    output(s, x, y, z, setProp(state, "power", power));
    s.scheduleTick(x, y, z, arrow ? 20 : 8);
}
} // namespace mc

namespace mc {
namespace {
const char* const tripFaces[] = {"down", "up", "north", "south", "west", "east"};
int tripFacing(uint16_t st) {
    const char* f = getPropStr(st, "facing");
    for (int i = 2; i < 6; ++i)
        if (f && !strcmp(f, tripFaces[i])) return i;
    return 2;
}
} // namespace
void Redstone::tripwire(Server& s, int x, int y, int z, uint16_t state) {
    bool attached = getBool(state, "attached");
    PistonBox bounds{{x + 0.0, y + (attached ? 1.0 / 16 : 0), z + 0.0},
                     {x + 1.0, y + (attached ? 2.5 / 16 : .5), z + 1.0}};
    bool powered = countEntities(s, bounds, [](const Entity&) { return true; }) > 0;
    if (powered != getBool(state, "powered")) {
        state = setBool(state, "powered", powered);
        output(s, x, y, z, state);
        tripwireChanged(s, x, y, z, state);
    }
    if (powered) s.scheduleTick(x, y, z, 10);
}
void Redstone::tripwireChanged(Server& s, int x, int y, int z, uint16_t state) {
    // Java deliberately searches only south and west; the contacted hook scans
    // back over the complete line, including the other endpoint.
    for (int f : {3, 4})
        for (int distance = 1; distance < 42; ++distance) {
            int px = x + FACE_DX[f] * distance, pz = z + FACE_DZ[f] * distance;
            uint16_t st = s.blockAt(px, y, pz);
            if (blockIdOf(st) == blk::TripwireHook) {
                if (tripFacing(st) == (f ^ 1)) tripwireHook(s, px, y, pz, st, false, distance, state);
                break;
            }
            if (blockIdOf(st) != blk::Tripwire) break;
        }
}
void Redstone::tripwireHook(Server& s, int x, int y, int z, uint16_t state, bool removed, int changedDistance,
                            uint16_t changedState) {
    int f = tripFacing(state), distance = 0;
    bool oldAttached = getBool(state, "attached"), oldPowered = getBool(state, "powered");
    bool attached = !removed, powered = false;
    uint16_t line[42] = {};
    for (int i = 1; i < 42; ++i) {
        uint16_t st = s.blockAt(x + FACE_DX[f] * i, y, z + FACE_DZ[f] * i);
        if (blockIdOf(st) == blk::TripwireHook) {
            if (tripFacing(st) == (f ^ 1)) distance = i;
            break;
        }
        if (blockIdOf(st) == blk::Tripwire || i == changedDistance) {
            if (i == changedDistance) st = changedState;
            bool armed = !getBool(st, "disarmed");
            powered |= armed && getBool(st, "powered");
            line[i] = st;
            if (i == changedDistance) {
                s.scheduleTick(x, y, z, 10);
                attached &= armed;
            }
        } else
            attached = false;
    }
    attached &= distance > 1;
    powered &= attached;
    auto notify = [&](int px, int pz, int facing) {
        neighbours(s, px, y, pz);
        neighbours(s, px - FACE_DX[facing], y, pz - FACE_DZ[facing]);
    };
    auto sound = [&](int px, int pz) {
        const char* event = nullptr;
        float pitch = 1;
        if (powered && !oldPowered) {
            event = "click_on";
            pitch = .6f;
        } else if (!powered && oldPowered) {
            event = "click_off";
            pitch = .5f;
        } else if (attached && !oldAttached) {
            event = "attach";
            pitch = .7f;
        } else if (!attached && oldAttached) {
            event = "detach";
            pitch = 1.2f;
        }
        if (event) {
            char name[64];
            snprintf(name, sizeof(name), "block.tripwire.%s", event);
            s.playSound(name, px + .5, y + .5, pz + .5, .4f, pitch, 4);
        }
    };
    uint16_t hook = setBool(setBool(bs::TripwireHook, "attached", attached), "powered", powered);
    if (distance > 0) {
        int px = x + FACE_DX[f] * distance, pz = z + FACE_DZ[f] * distance;
        output(s, px, y, pz, setPropStr(hook, "facing", tripFaces[f ^ 1]));
        notify(px, pz, f ^ 1);
        sound(px, pz);
    }
    sound(x, z);
    if (!removed) {
        output(s, x, y, z, setPropStr(hook, "facing", tripFaces[f]));
        notify(x, z, f);
    } else if (oldPowered)
        notify(x, z, f);
    if (oldAttached != attached)
        for (int i = 1; i < distance; ++i)
            if (line[i]) {
                int px = x + FACE_DX[f] * i, pz = z + FACE_DZ[f] * i;
                if (!stateIsAir(s.blockAt(px, y, pz))) output(s, px, y, pz, setBool(line[i], "attached", attached));
            }
}
} // namespace mc
