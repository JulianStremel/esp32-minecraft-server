// Vibrations (1.19+): game events with a frequency travel to sculk sensors in range,
// which then give a redstone signal for a while (vanilla's VibrationSystem, simplified:
// one vibration on its way per sensor, the first that reaches it).
#include <math.h>
#include <string.h>
#include "mc/registry.h"
#include "mc/server/redstone.h"
#include "mc/server/server.h"

namespace mc {

namespace {

bool calibrated(uint16_t id) { return id == blk::CalibratedSculkSensor; }

// #occludes_vibration_signals: wool and wool carpets
bool occludes(uint16_t state) {
    const char* n = blockOf(state).name;
    size_t l = strlen(n);
    return (l > 5 && !strcmp(n + l - 5, "_wool")) || (l > 7 && !strcmp(n + l - 7, "_carpet") && strcmp(n, "moss_carpet") &&
                                                      strcmp(n, "pale_moss_carpet"));
}

// Any occluding block on the way (sampled every quarter block, the ends excluded).
bool blocked(Server& s, double ax, double ay, double az, double bx, double by, double bz) {
    double dx = bx - ax, dy = by - ay, dz = bz - az, len = sqrt(dx * dx + dy * dy + dz * dz);
    int steps = (int)(len * 4);
    int lx = INT32_MIN, ly = 0, lz = 0;
    for (int i = 1; i < steps; i++) {
        double t = (double)i / steps;
        int x = (int)floor(ax + dx * t), y = (int)floor(ay + dy * t), z = (int)floor(az + dz * t);
        if (x == lx && y == ly && z == lz) continue;
        lx = x; ly = y; lz = z;
        if (occludes(s.blockAt(x, y, z))) return true;
    }
    return false;
}

const char* const H[] = {"north", "south", "west", "east"};
int horizontal(uint16_t state) {
    const char* f = getPropStr(state, "facing");
    for (int i = 0; i < 4; i++)
        if (f && !strcmp(f, H[i])) return i + 2;
    return 2;
}

}  // namespace

bool Redstone::sculkSensor(uint16_t id) { return id == blk::SculkSensor || id == blk::CalibratedSculkSensor; }

// A calibrated sensor listens only to the frequency its input side is powered with.
int Redstone::sculkInput(Server& s, int x, int y, int z, uint16_t state) {
    if (!calibrated(blockIdOf(state))) return 0;
    int f = horizontal(state);
    return signal(s, x + FACE_DX[f], y, z + FACE_DZ[f], f);
}

void Server::vibration(double x, double y, double z, int frequency) {
    if (frequency <= 0 || frequency > 15) return;
    int cx0 = ((int)floor(x) - 16) >> 4, cx1 = ((int)floor(x) + 16) >> 4;
    int cz0 = ((int)floor(z) - 16) >> 4, cz1 = ((int)floor(z) + 16) >> 4;
    for (int cx = cx0; cx <= cx1; cx++)
        for (int cz = cz0; cz <= cz1; cz++) {
            Chunk* c = world.peek(curDim, cx, cz);
            if (!c || !c->sculkSensors()) continue;
            for (TileEntity* t = c->tiles(); t; t = t->next) {
                if (t->type != TILE_SCULK || t->pendingFrequency) continue;
                int sx = cx * 16 + t->lx, sy = t->y, sz = cz * 16 + t->lz;
                uint16_t st = c->get(t->lx, t->y, t->lz);
                if (!Redstone::sculkSensor(blockIdOf(st)) || strcmp(getPropStr(st, "sculk_sensor_phase"), "inactive")) continue;
                int range = calibrated(blockIdOf(st)) ? 16 : 8;
                double dx = sx + .5 - x, dy = sy + .5 - y, dz = sz + .5 - z, d = sqrt(dx * dx + dy * dy + dz * dz);
                if (d > range) continue;
                int input = redstone.sculkInput(*this, sx, sy, sz, st);
                if (input > 0 && input != frequency) continue;
                if (blocked(*this, x, y, z, sx + .5, sy + .5, sz + .5)) continue;
                int strength = 15 - (int)floor(15.0 / range * d);
                t->pendingFrequency = (uint8_t)frequency;
                t->pendingStrength = (uint8_t)(strength < 1 ? 1 : strength);
                scheduleTick(sx, sy, sz, d < 1 ? 1 : (int)floor(d));   // the vibration travels a block per tick
            }
        }
}

// The sensor's scheduled tick: a vibration arrives, the active phase ends, the cooldown ends.
void Redstone::sculkTick(Server& s, int x, int y, int z, uint16_t st) {
    Chunk* c = s.world.get(s.curDim, x >> 4, z >> 4);
    TileEntity* t = c ? c->tileAt(x & 15, y, z & 15) : nullptr;
    const char* phase = getPropStr(st, "sculk_sensor_phase");
    bool cal = calibrated(blockIdOf(st));
    if (!strcmp(phase, "inactive")) {
        if (!t || t->type != TILE_SCULK || !t->pendingFrequency) return;
        t->frequency = t->pendingFrequency;
        int power = t->pendingStrength;
        t->pendingFrequency = 0;
        c->dirty = true;
        output(s, x, y, z, setProp(setPropStr(st, "sculk_sensor_phase", "active"), "power", power));
        neighbours(s, x, y - 1, z);   // strong power into the block below
        analogChanged(s, x, y, z);
        s.playSound(cal ? "block.calibrated_sculk_sensor.clicking" : "block.sculk_sensor.clicking", x + .5, y + .5, z + .5, 1,
                    1, 4);
        s.scheduleTick(x, y, z, cal ? 10 : 30);
    } else if (!strcmp(phase, "active")) {
        output(s, x, y, z, setProp(setPropStr(st, "sculk_sensor_phase", "cooldown"), "power", 0));
        neighbours(s, x, y - 1, z);
        s.playSound(cal ? "block.calibrated_sculk_sensor.clicking_stop" : "block.sculk_sensor.clicking_stop", x + .5, y + .5,
                    z + .5, 1, 1, 4);
        s.scheduleTick(x, y, z, 10);
    } else {
        output(s, x, y, z, setPropStr(st, "sculk_sensor_phase", "inactive"));
    }
}

}  // namespace mc
