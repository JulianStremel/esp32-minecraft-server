// Placing a structure template into a chunk: vanilla's StructureTemplate#placeInWorld and
// #processBlockInfos with the processors a pool element brings (structure blocks and, for
// legacy elements, air left out; jigsaws replaced by their final state; rules, rot and
// protected blocks with vanilla's random number per position, Mth.getSeed; gravity for
// terrain-matching pieces), the blocks mirrored and rotated, waterlogged where they go
// into water, containers given their loot and the template's mobs kept with the chunk.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc/platform.h"
#include "mc/world/vanilla/structure_build.h"
#include "mc/world/vanilla/templates.h"

namespace mc {
namespace vanilla {
namespace sb {

namespace {

// the names resolved once: states, block ids of the strings and of the sets
struct Resolved {
    bool done = false;
    uint16_t* state = nullptr;   // TD_STATES
    int16_t* block = nullptr;    // TD_STRINGS as block ids (-1 none)
    uint16_t jigsaw = 0, structureBlock = 0, structureVoid = 0, air = 0, water = 0, chest = 0, barrel = 0, trapped = 0;
};
Resolved R;

uint16_t parseState(const char* spec) {
    char name[64];
    const char* br = strchr(spec, '[');
    size_t n = br ? (size_t)(br - spec) : strlen(spec);
    if (n >= sizeof(name)) n = sizeof(name) - 1;
    memcpy(name, spec, n);
    name[n] = 0;
    int b = findBlock(name);
    if (b < 0) return 0;
    uint16_t st = BLOCKS[b].defState;
    if (br) {
        char props[160];
        snprintf(props, sizeof(props), "%s", br + 1);
        char* save = nullptr;
        for (char* kv = strtok_r(props, ",]", &save); kv; kv = strtok_r(nullptr, ",]", &save)) {
            char* eq = strchr(kv, '=');
            if (!eq) continue;
            *eq = 0;
            st = setPropStr(st, kv, eq + 1);
        }
    }
    return st;
}

}  // namespace

void templatesInit() {
    if (R.done) return;
    R.state = (uint16_t*)malloc(sizeof(uint16_t) * TD_NUM_STATES);
    R.block = (int16_t*)malloc(sizeof(int16_t) * TD_NUM_STRINGS);
    if (!R.state || !R.block) return;
    for (int i = 0; i < TD_NUM_STATES; i++) R.state[i] = parseState(TD_STATES[i]);
    for (int i = 0; i < TD_NUM_STRINGS; i++) R.block[i] = (int16_t)findBlock(TD_STRINGS[i]);
    R.jigsaw = (uint16_t)findBlock("jigsaw");
    R.structureBlock = (uint16_t)findBlock("structure_block");
    R.structureVoid = (uint16_t)findBlock("structure_void");
    R.air = (uint16_t)findBlock("air");
    R.water = (uint16_t)findBlock("water");
    R.chest = (uint16_t)findBlock("chest");
    R.trapped = (uint16_t)findBlock("trapped_chest");
    R.barrel = (uint16_t)findBlock("barrel");
    R.done = true;
}

uint16_t templateState(int i) { return R.done && i >= 0 && i < TD_NUM_STATES ? R.state[i] : 0; }

// ------------------------------------------------------------------ transforms
namespace {

// StructureTemplate#transform: mirror (1 left-right: z, 2 front-back: x), then rotation
// (1 clockwise 90, 2 180, 3 counter-clockwise 90) around the pivot
inline void transformXZ(int x, int z, int mirror, int rot, int& ox, int& oz, int l = 0, int i1 = 0) {
    if (mirror == 1) z = -z;
    else if (mirror == 2) x = -x;
    switch (rot & 3) {
        case 1: ox = l + i1 - z; oz = i1 - l + x; break;
        case 2: ox = l + l - x; oz = i1 + i1 - z; break;
        case 3: ox = l - i1 + z; oz = l + i1 - x; break;
        default: ox = x; oz = z; break;
    }
}

const char* const DIRS[4] = {"north", "east", "south", "west"};
int dirIndex(const char* s) {
    for (int i = 0; i < 4; i++)
        if (!strcmp(s, DIRS[i])) return i;
    return -1;
}
int mapDir(int d, int mirror, int rot) {
    if (mirror == 1 && (d == 0 || d == 2)) d ^= 2;
    if (mirror == 2 && (d == 1 || d == 3)) d ^= 2;
    return (d + rot) & 3;
}

}  // namespace

// BlockState#mirror then #rotate: facing, axis, the sides, rails, stairs' and doors'
// handedness, chests' halves, the 16-step rotation of signs, banners and heads
uint16_t transformState(uint16_t st, int mirror, int rot) {
    if ((!mirror && !(rot & 3)) || st == 0) return st;
    uint16_t out = st, b = blockIdOf(st);
    if (propIndexOf(b, "facing") >= 0) {
        int d = dirIndex(getPropStr(st, "facing"));
        if (d >= 0) out = setPropStr(out, "facing", DIRS[mapDir(d, mirror, rot)]);
    }
    if ((rot & 1) && propIndexOf(b, "axis") >= 0) {
        const char* a = getPropStr(st, "axis");
        if (!strcmp(a, "x")) out = setPropStr(out, "axis", "z");
        else if (!strcmp(a, "z")) out = setPropStr(out, "axis", "x");
    }
    if (propIndexOf(b, "north") >= 0 && propIndexOf(b, "east") >= 0 && propIndexOf(b, "south") >= 0 && propIndexOf(b, "west") >= 0) {
        const char* v[4];
        for (int d = 0; d < 4; d++) v[d] = getPropStr(st, DIRS[d]);
        for (int d = 0; d < 4; d++) out = setPropStr(out, DIRS[mapDir(d, mirror, rot)], v[d]);
    }
    if (propIndexOf(b, "shape") >= 0) {
        const char* s = getPropStr(st, "shape");
        char buf[32];
        snprintf(buf, sizeof(buf), "%s", s);
        if (!strncmp(buf, "ascending_", 10)) {   // rails going up
            int d = dirIndex(buf + 10);
            if (d >= 0) {
                char o[32];
                snprintf(o, sizeof(o), "ascending_%s", DIRS[mapDir(d, mirror, rot)]);
                out = setPropStr(out, "shape", o);
            }
        } else if (char* us = strchr(buf, '_')) {   // straight and curved rails
            *us = 0;
            int a = dirIndex(buf), c = dirIndex(us + 1);
            if (a >= 0 && c >= 0) {
                a = mapDir(a, mirror, rot);
                c = mapDir(c, mirror, rot);
                char o[32];
                snprintf(o, sizeof(o), "%s_%s", DIRS[a], DIRS[c]);
                uint16_t t = setPropStr(out, "shape", o);
                if (strcmp(getPropStr(t, "shape"), o) != 0) {
                    snprintf(o, sizeof(o), "%s_%s", DIRS[c], DIRS[a]);
                    t = setPropStr(out, "shape", o);
                }
                out = t;
            } else if (mirror) {   // stairs: inner_left <-> inner_right
                static const char* const SWAP[][2] = {{"inner_left", "inner_right"}, {"outer_left", "outer_right"}};
                for (const auto& sw : SWAP) {
                    if (!strcmp(s, sw[0])) out = setPropStr(out, "shape", sw[1]);
                    else if (!strcmp(s, sw[1])) out = setPropStr(out, "shape", sw[0]);
                }
            }
        }
    }
    if (mirror) {
        if (propIndexOf(b, "hinge") >= 0) out = setPropStr(out, "hinge", !strcmp(getPropStr(st, "hinge"), "left") ? "right" : "left");
        if (propIndexOf(b, "type") >= 0) {
            const char* t = getPropStr(st, "type");
            if (!strcmp(t, "left")) out = setPropStr(out, "type", "right");
            else if (!strcmp(t, "right")) out = setPropStr(out, "type", "left");
        }
    }
    int ri = propIndexOf(b, "rotation");
    if (ri >= 0) {
        int r = getProp(st, "rotation");
        if (mirror) {   // Mirror#mirror(rotation, 16)
            int j = r > 8 ? r - 16 : r;
            r = mirror == 2 ? (16 - j) % 16 : (8 - j + 16) % 16;
        }
        r = (r + 4 * (rot & 3)) & 15;
        out = setProp(out, "rotation", r);
    }
    return out;
}

void templateTransform(int x, int z, int mirror, int rot, int pivotX, int pivotZ, int& ox, int& oz) {
    transformXZ(x, z, mirror, rot, ox, oz, pivotX, pivotZ);
}

SBox templateBox(int tmpl, int x, int y, int z, int rot, int mirror, int pivotX, int pivotZ) {
    const TdTemplate& t = TD_TEMPLATES[tmpl];
    int ax, az, bx, bz;
    transformXZ(0, 0, mirror, rot, ax, az, pivotX, pivotZ);
    transformXZ(t.sx - 1, t.sz - 1, mirror, rot, bx, bz, pivotX, pivotZ);
    return {x + (ax < bx ? ax : bx), y, z + (az < bz ? az : bz), x + (ax > bx ? ax : bx), y + t.sy - 1, z + (az > bz ? az : bz)};
}

// ------------------------------------------------------------------ processors
namespace {

}  // namespace

// Mth.getSeed: the random number vanilla's processors start from at a position
int64_t positionSeed(int x, int y, int z) {
    int64_t i = (int64_t)(int32_t)((uint32_t)x * 3129871u) ^ (int64_t)z * 116129781LL ^ (int64_t)y;
    i = (int64_t)((uint64_t)i * (uint64_t)i * 42317861ULL + (uint64_t)i * 11ULL);
    return i >> 16;
}

namespace {

bool inSet(int set1, uint16_t blockId) {   // set index + 1
    if (set1 <= 0) return false;
    const TdSet& s = TD_SETS[set1 - 1];
    for (int i = 0; i < s.count; i++)
        if (R.block[TD_SET_NAMES[s.first + i]] == (int16_t)blockId) return true;
    return false;
}

// RuleTest#test: always true, the block, the state, the block at random, the tag, the state at random
bool ruleTest(uint8_t type, uint16_t arg, float prob, uint16_t st, JavaRandom& r) {
    switch (type) {
        case 0: return true;
        case 1: return blockIdOf(st) == (uint16_t)R.block[arg];
        case 2: return st == R.state[arg];
        case 3: return blockIdOf(st) == (uint16_t)R.block[arg] && r.nextFloat() < prob;
        case 4: return inSet(arg + 1, blockIdOf(st));
        case 5: return st == R.state[arg] && r.nextFloat() < prob;
        default: return false;
    }
}

}  // namespace

// The blocks of one template placed into the chunk (whatever of it lies in the clip box).
int placeTemplate(Chunk& c, const SBox& clip, const TemplatePlacement& t, int64_t seed) {
    if (!R.done || t.tmpl < 0 || t.tmpl >= TD_NUM_TEMPLATES) return 0;
    const TdTemplate& tp = TD_TEMPLATES[t.tmpl];
    SBox box = templateBox(t.tmpl, t.x, t.y, t.z, t.rot, t.mirror, t.pivotX, t.pivotZ);
    // a terrain-matching piece follows the ground: its blocks may leave the box in y
    SBox xz = clip;
    if (box.x1 < xz.x0 || box.x0 > xz.x1 || box.z1 < xz.z0 || box.z0 > xz.z1) return 0;
    if (!t.gravity && (box.y1 < xz.y0 || box.y0 > xz.y1)) return 0;
    // WORLD_SURFACE_WG before the piece: the first non-air from the top, per column
    int16_t surf[256];
    if (t.gravity)
        for (int col = 0; col < 256; col++) {
            int lx = col & 15, lz = col >> 4, y = c.maxY();
            while (y >= c.minY() && stateIsAir(c.get(lx, y, lz))) y--;
            surf[col] = (int16_t)(y + 1);
        }
    const TdProcList& pl = TD_PROC_LISTS[t.processors];
    int placed = 0;
    const uint8_t* p = TD_BLOB + tp.data;
    const int total = tp.sx * tp.sy * tp.sz;
    auto varint = [&]() {
        uint32_t v = 0;
        for (int sh = 0;; sh += 7) {
            uint8_t b = *p++;
            v |= (uint32_t)(b & 0x7F) << sh;
            if (!(b & 0x80)) return v;
        }
    };
    // StructurePlaceSettings#getRandomPalette: one at random for the template's position
    int palette = 0;
    if (tp.palettes > 1) {
        JavaRandom pr(positionSeed(t.x, t.y, t.z));
        palette = pr.nextInt(tp.palettes);
    }
    int idx = 0;
    while (idx < total) {
        uint32_t run = varint(), val = varint();
        if (!val) {
            idx += (int)run;
            continue;
        }
        uint16_t raw = R.state[TD_PALETTE[tp.palFirst + palette * tp.palCount + val - 1]];
        for (uint32_t k = 0; k < run; k++, idx++) {
            int lx = idx % tp.sx, lz = (idx / tp.sx) % tp.sz, ly = idx / (tp.sx * tp.sz);
            int ox, oz;
            transformXZ(lx, lz, t.mirror, t.rot, ox, oz, t.pivotX, t.pivotZ);
            int X = t.x + ox, Y = t.y + ly, Z = t.z + oz;
            if (X < clip.x0 || X > clip.x1 || Z < clip.z0 || Z > clip.z1) continue;
            uint16_t st = raw, id = blockIdOf(st);
            // BlockIgnoreProcessor: structure blocks; legacy elements leave out air
            if (id == R.structureBlock || (t.legacy && id == R.air)) continue;
            // JigsawReplacementProcessor
            if (id == R.jigsaw && !t.keepJigsaws) {
                st = 0;
                for (int j = 0; j < tp.jigCount; j++) {
                    const TdJigsaw& jg = TD_JIGSAWS[tp.jigFirst + j];
                    if (jg.x == lx && jg.y == ly && jg.z == lz) st = R.state[jg.finalState];
                }
                if (blockIdOf(st) == R.structureVoid) continue;
            }
            {
                bool drop = false;
                uint16_t world = c.hasY(Y) ? c.get(X & 15, Y, Z & 15) : 0;
                if (t.integrity < 1.0f) {   // the piece's own BlockRotProcessor
                    JavaRandom r(positionSeed(X, Y, Z));
                    if (!(r.nextFloat() <= t.integrity)) continue;
                }
                for (int pi = 0; pi < pl.count && !drop; pi++) {
                    const TdProcessor& pr = TD_PROCESSORS[pl.first + pi];
                    if (pr.type == 0) {   // RuleProcessor: the first rule that holds
                        JavaRandom r(positionSeed(X, Y, Z));
                        for (int ri = 0; ri < pr.rules; ri++) {
                            const TdRule& ru = TD_RULES[pr.firstRule + ri];
                            if (!ruleTest(ru.inType, ru.inArg, ru.inProb, st, r)) continue;
                            if (!ruleTest(ru.locType, ru.locArg, ru.locProb, world, r)) continue;
                            if (ru.posType == 1) {   // axis_aligned_linear_pos
                                int d = ru.posAxis == 0 ? abs(X - t.x) : ru.posAxis == 2 ? abs(Z - t.z) : abs(Y - t.y);
                                float f = ru.posMaxDist > ru.posMinDist ? (float)(d - ru.posMinDist) / (float)(ru.posMaxDist - ru.posMinDist) : 0;
                                if (f < 0) f = 0;
                                if (f > 1) f = 1;
                                if (r.nextFloat() > ru.posMinChance + f * (ru.posMaxChance - ru.posMinChance)) continue;
                            }
                            st = R.state[ru.output];
                            break;
                        }
                    } else if (pr.type == 1) {   // BlockRotProcessor
                        JavaRandom r(positionSeed(X, Y, Z));
                        if (!(pr.set && !inSet(pr.set, blockIdOf(st))) && !(r.nextFloat() <= pr.integrity)) drop = true;
                    } else if (pr.type == 2) {   // ProtectedBlockProcessor
                        if (inSet(pr.set, blockIdOf(world))) drop = true;
                    }
                }
                if (!drop && t.process) st = t.process(t.processCtx, st, world, X, Y, Z, drop);
                if (drop) continue;
                if (t.gravity) {   // GravityProcessor(WORLD_SURFACE_WG, -1)
                    Y = surf[(Z & 15) * 16 + (X & 15)] - 1 + ly;
                    world = c.hasY(Y) ? c.get(X & 15, Y, Z & 15) : 0;
                }
                if (Y < clip.y0 || Y > clip.y1 || !c.hasY(Y)) continue;
                uint16_t out = transformState(st, t.mirror, t.rot);
                // into water: waterlogged where the block can be (LiquidSettings.APPLY_WATERLOGGING)
                if (!t.noWaterlogging && blockIdOf(world) == R.water && getProp(world, "level") == 0 &&
                    propIndexOf(blockIdOf(out), "waterlogged") >= 0)
                    out = setBool(out, "waterlogged", true);
                c.set(X & 15, Y, Z & 15, out);
                placed++;
                uint16_t oid = blockIdOf(out);
                if (oid == R.chest || oid == R.trapped || oid == R.barrel) {
                    for (int ci = 0; ci < tp.conCount; ci++) {
                        const TdContainer& ct = TD_CONTAINERS[tp.conFirst + ci];
                        if (ct.x != lx || ct.y != ly || ct.z != lz) continue;
                        TileEntity* te = c.addTile(oid == R.barrel ? TILE_BARREL : TILE_CHEST, X & 15, Y, Z & 15);
                        fillLoot(te, TD_STRINGS[ct.loot], seed ^ positionSeed(X, Y, Z));
                    }
                }
            }
        }
    }
    // the template's mobs (villagers, golems, cats, ...) wait with the chunk for a player
    if (!t.ignoreEntities)
        for (int ei = 0; ei < tp.entCount; ei++) {
            const TdEntity& te = TD_ENTITIES[tp.entFirst + ei];
            int ox, oz;
            transformXZ(te.x, te.z, t.mirror, t.rot, ox, oz, t.pivotX, t.pivotZ);
            int X = t.x + ox, Y = t.y + te.y, Z = t.z + oz;
            if (t.gravity) Y = surf[(Z & 15) * 16 + (X & 15)] - 1 + te.y;
            if (X < clip.x0 || X > clip.x1 || Z < clip.z0 || Z > clip.z1 || !c.hasY(Y)) continue;
            int type = findEntityType(TD_STRINGS[te.type]);
            if (type < 0 || !isMobType(type)) continue;
            SavedEntity e;
            e.kind = 3;   // a mob (EK_MOB)
            e.type = (uint16_t)type;
            e.x = X + 0.5;
            e.y = Y;
            e.z = Z + 0.5;
            e.health = 20;
            c.addEntity(e);
        }
    // the data markers (StructureTemplatePiece#handleDataMarker)
    if (t.marker)
        for (int mi = 0; mi < tp.markCount; mi++) {
            const TdMarker& mk = TD_MARKERS[tp.markFirst + mi];
            int ox, oz;
            transformXZ(mk.x, mk.z, t.mirror, t.rot, ox, oz, t.pivotX, t.pivotZ);
            int X = t.x + ox, Y = t.y + mk.y, Z = t.z + oz;
            if (X < clip.x0 || X > clip.x1 || Z < clip.z0 || Z > clip.z1 || !c.hasY(Y)) continue;
            t.marker(t.markerCtx, c, clip, TD_STRINGS[mk.name], X, Y, Z);
        }
    return placed;
}

}  // namespace sb
}  // namespace vanilla
}  // namespace mc
