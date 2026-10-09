#include "mc/server/piston.h"
#include "mc/server/server.h"
#include "mc/registry.h"
#include "mc/server/automation.h"
#include <algorithm>
#include <math.h>

namespace mc {
PistonPos PistonPos::offset(int d, int n) const {
    return {x + FACE_DX[d] * n, y + FACE_DY[d] * n, z + FACE_DZ[d] * n};
}
namespace {
bool sticky(uint16_t st) {
    return blockIdOf(st) == blk::SlimeBlock || blockIdOf(st) == blk::HoneyBlock;
}
bool sticks(uint16_t a, uint16_t b) {
    int x = blockIdOf(a), y = blockIdOf(b);
    if ((x == blk::SlimeBlock && y == blk::HoneyBlock) || (x == blk::HoneyBlock && y == blk::SlimeBlock)) return false;
    return sticky(a) || sticky(b);
}
uint16_t at(Server& s, PistonPos p) {
    if (!dimHasY(s.curDim, p.y) || !s.world.blockInBounds(p.x, p.z)) return bs::Bedrock;
    Chunk* c = s.world.load(s.curDim, p.x >> 4, p.z >> 4);
    return c && !c->readOnly ? c->get(p.x & 15, p.y, p.z & 15) : bs::Bedrock;
}
} // namespace
bool PistonPlan::pushable(Server& s, PistonPos p, int d, bool destroy, int interaction) {
    if (!dimHasY(s.curDim, p.y) || !s.world.blockInBounds(p.x, p.z)) return false;
    uint16_t st = at(s, p);
    if (stateIsAir(st)) return true;
    int id = blockIdOf(st);
    if (id == blk::Obsidian || id == blk::CryingObsidian || id == blk::RespawnAnchor) return false;
    if ((d == 0 && p.y == dimMinY(s.curDim)) || (d == 1 && p.y == dimMaxY(s.curDim))) return false;
    if (id == blk::Piston || id == blk::StickyPiston) {
        if (getBool(st, "extended")) return false;
    } else {
        if (stateUnbreakable(st)) return false;
        switch (statePushReaction(st)) {
        case PUSH_BLOCK:
            return false;
        case PUSH_DESTROY:
            return destroy;
        case PUSH_ONLY:
            return d == interaction;
        default:
            break;
        }
    }
    return !stateHasBlockEntity(st);
}
int PistonPlan::index(PistonPos p) const {
    for (int i = 0; i < moveCount; ++i)
        if (moved[i] == p) return i;
    return -1;
}
void PistonPlan::addBroken(PistonPos p) {
    for (int i = 0; i < breakCount; ++i)
        if (broken[i] == p) return;
    broken[breakCount++] = p;
}
bool PistonPlan::resolve(Server& s, PistonPos base, int face, bool extend) {
    server_ = &s;
    base_ = base;
    direction_ = extend ? face : face ^ 1;
    moveCount = breakCount = 0;
    PistonPos start = base.offset(face, extend ? 1 : 2);
    if (!pushable(s, start, direction_, false, face)) {
        if (extend && statePushReaction(at(s, start)) == PUSH_DESTROY) {
            addBroken(start);
            return true;
        }
        return false;
    }
    if (!line(start, direction_)) return false;
    for (int i = 0; i < moveCount; ++i)
        if (sticky(at(s, moved[i])) && !branches(moved[i])) return false;
    return true;
}
bool PistonPlan::line(PistonPos start, int interaction) {
    Server& s = *server_;
    uint16_t st = at(s, start);
    if (stateIsAir(st) || !pushable(s, start, direction_, false, interaction) || start == base_ || index(start) >= 0)
        return true;
    int tail = 1;
    if (moveCount + tail > 12) return false;
    while (sticky(st)) {
        PistonPos p = start.offset(direction_ ^ 1, tail);
        uint16_t next = at(s, p);
        if (stateIsAir(next) || !sticks(st, next) || !pushable(s, p, direction_, false, direction_ ^ 1) || p == base_)
            break;
        st = next;
        if (moveCount + ++tail > 12) return false;
    }
    int added = 0;
    for (int i = tail - 1; i >= 0; --i) {
        moved[moveCount++] = start.offset(direction_ ^ 1, i);
        ++added;
    }
    for (int distance = 1;; ++distance) {
        PistonPos p = start.offset(direction_, distance);
        int collision = index(p);
        if (collision >= 0) {
            // The new tail must move before the old line it ran into. This order
            // is observable through neighbour notifications after the move.
            PistonPos reordered[12];
            int n = 0;
            for (int i = 0; i < collision; ++i)
                reordered[n++] = moved[i];
            for (int i = moveCount - added; i < moveCount; ++i)
                reordered[n++] = moved[i];
            for (int i = collision; i < moveCount - added; ++i)
                reordered[n++] = moved[i];
            for (int i = 0; i < moveCount; ++i)
                moved[i] = reordered[i];
            for (int i = 0; i <= collision + added; ++i)
                if (sticky(at(s, moved[i])) && !branches(moved[i])) return false;
            return true;
        }
        st = at(s, p);
        if (stateIsAir(st)) return true;
        if (!pushable(s, p, direction_, true, direction_) || p == base_) return false;
        if (statePushReaction(st) == PUSH_DESTROY) {
            addBroken(p);
            return true;
        }
        if (moveCount == 12) return false;
        moved[moveCount++] = p;
        ++added;
    }
}
bool PistonPlan::branches(PistonPos pos) {
    uint16_t st = at(*server_, pos);
    for (int face = 0; face < 6; ++face) {
        if ((face >> 1) == (direction_ >> 1)) continue;
        PistonPos p = pos.offset(face);
        if (sticks(st, at(*server_, p)) && !line(p, face)) return false;
    }
    return true;
}
} // namespace mc

namespace mc {
namespace {
const char* const pistonFaces[] = {"down", "up", "north", "south", "west", "east"};
int pistonFacing(uint16_t st) {
    const char* f = getPropStr(st, "facing");
    for (int i = 0; i < 6; ++i)
        if (f && !strcmp(f, pistonFaces[i])) return i;
    return 2;
}
TileEntity* pistonTile(Server& s, PistonPos p) {
    Chunk* c = s.world.get(s.curDim, p.x >> 4, p.z >> 4);
    TileEntity* t = c ? c->tileAt(p.x & 15, p.y, p.z & 15) : nullptr;
    return t && t->type == TILE_PISTON ? t : nullptr;
}
void pistonSet(Server& s, PistonPos p, uint16_t st, uint8_t flags) {
    s.world.setBlock(s.curDim, p.x, p.y, p.z, st, true, flags);
}
} // namespace
bool Pistons::powered(Server& s, PistonPos p, int f) {
    auto signal = [&](PistonPos q, int d) { return s.redstone.signal(s, q.x, q.y, q.z, d) > 0; };
    for (int d = 0; d < 6; ++d)
        if (d != f && signal(p.offset(d), d)) return true;
    if (signal(p, 0)) return true;
    PistonPos above = p.offset(1);
    for (int d = 1; d < 6; ++d)
        if (signal(above.offset(d), d)) return true;
    return false;
}
void Pistons::changed(Server& s, PistonPos p, uint16_t st) {
    int f = pistonFacing(st);
    bool power = powered(s, p, f), extended = getBool(st, "extended");
    if (power && !extended) {
        PistonPlan plan;
        if (plan.resolve(s, p, f, true)) s.redstone.blockEvent(s, p.x, p.y, p.z, blockIdOf(st), 0, f);
    } else if (!power && extended) {
        int type = 1;
        TileEntity* t = pistonTile(s, p.offset(f, 2));
        if (t && t->pistonExtending && t->pistonFace == f &&
            (t->pistonPrevious == 0 || t->updated == s.worldTick() || s.handlingBlockTicks()))
            type = 2;
        s.redstone.blockEvent(s, p.x, p.y, p.z, blockIdOf(st), type, f);
    }
}
bool Pistons::moving(Server& s, PistonPos p, uint16_t st, int f, bool extend, bool source, bool sticky) {
    Chunk* c = s.world.load(s.curDim, p.x >> 4, p.z >> 4);
    if (!c || c->readOnly) return false;
    uint16_t placeholder =
        setPropStr(setPropStr(bs::MovingPiston, "facing", pistonFaces[f]), "type", sticky ? "sticky" : "normal");
    // The block event makes connected clients perform this transition locally.
    pistonSet(s, p, placeholder, source && !extend ? 20 : 68);
    TileEntity* t = c->addTile(TILE_PISTON, p.x & 15, p.y, p.z & 15);
    if (!t) {
        MC_LOGE("Piston block entity allocation failed");
        return false;
    }
    t->movedState = st;
    t->pistonFace = f;
    t->pistonExtending = extend;
    t->pistonSource = source;
    t->tickOrder = ++s.blockEntitySequence;
    c->dirty = true;
    return true;
}
bool Pistons::move(Server& s, PistonPos p, int f, bool extend, bool sticky) {
    PistonPos head = p.offset(f);
    if (!extend && blockIdOf(at(s, head)) == blk::PistonHead) pistonSet(s, head, bs::Air, 20);
    PistonPlan plan;
    if (!plan.resolve(s, p, f, extend)) return false;
    // Verify every write target before changing the structure (especially border
    // and failed-storage chunks, which must never be treated as empty space).
    int direction = extend ? f : f ^ 1;
    for (int i = 0; i < plan.moveCount; ++i) {
        PistonPos q = plan.moved[i], dest = q.offset(direction);
        if (!s.world.isWritable(s.curDim, q.x, q.z) || !s.world.isWritable(s.curDim, dest.x, dest.z)) return false;
    }
    uint16_t states[12];
    for (int i = 0; i < plan.moveCount; ++i)
        states[i] = at(s, plan.moved[i]);
    for (int i = plan.breakCount - 1; i >= 0; --i) {
        PistonPos q = plan.broken[i];
        // Drop the block's ordinary loot; structural notifications are delivered
        // after moving destinations have been installed.
        const BlockDef& b = blockOf(at(s, q));
        if (b.dropItem && b.dropMin) s.dropItem(q.x + .5, q.y + .5, q.z + .5, ItemStack::of(b.dropItem, b.dropMin));
        pistonSet(s, q, bs::Air, 18);
    }
    for (int i = plan.moveCount - 1; i >= 0; --i)
        if (!moving(s, plan.moved[i].offset(direction), states[i], f, extend, false, false)) return false;
    if (extend) {
        uint16_t hs =
            setPropStr(setPropStr(bs::PistonHead, "facing", pistonFaces[f]), "type", sticky ? "sticky" : "normal");
        if (!moving(s, head, hs, f, true, true, sticky)) return false;
    }
    // Java's remaining source-position HashMap iterates by spread hash buckets.
    // At most 12 entries means the initial capacity remains 16 throughout.
    for (unsigned bucket = 0; bucket < 16; ++bucket)
        for (int i = 0; i < plan.moveCount; ++i) {
            PistonPos q = plan.moved[i];
            uint32_t h = uint32_t(q.x) + 31u * uint32_t(q.z) + 961u * uint32_t(q.y);
            if (((h ^ (h >> 16)) & 15u) != bucket) continue;
            bool occupied = extend && q == head;
            for (int j = 0; j < plan.moveCount; ++j)
                if (q == plan.moved[j].offset(direction)) occupied = true;
            if (!occupied) pistonSet(s, q, bs::Air, 82);
        }
    for (int i = plan.breakCount - 1; i >= 0; --i) {
        PistonPos q = plan.broken[i];
        s.redstone.neighbours(s, q.x, q.y, q.z);
        s.updateNeighbors(q.x, q.y, q.z);
    }
    for (int i = plan.moveCount - 1; i >= 0; --i) {
        PistonPos q = plan.moved[i];
        s.redstone.neighbours(s, q.x, q.y, q.z);
        // a vacated source loses what stood on it (the shape update of its air)
        if (blockIdOf(at(s, q)) != blk::MovingPiston) s.updateNeighbors(q.x, q.y, q.z);
    }
    if (extend) s.redstone.neighbours(s, head.x, head.y, head.z);
    return true;
}
void Pistons::finish(Server& s, PistonPos p, bool forced) {
    TileEntity* t = pistonTile(s, p);
    if (!t || (forced && t->pistonPrevious >= 2)) return;
    uint16_t state = forced && t->pistonSource ? bs::Air : t->movedState;
    if (!forced && getBool(state, "waterlogged")) state = setBool(state, "waterlogged", false);
    Chunk* c = s.world.get(s.curDim, p.x >> 4, p.z >> 4);
    c->removeTile(p.x & 15, p.y, p.z & 15);
    if (blockIdOf(at(s, p)) != blk::MovingPiston) return;
    pistonSet(s, p, state, forced ? 3 : 67);
    s.updateNeighbors(p.x, p.y, p.z);
    if (blockIdOf(state) == blk::Piston || blockIdOf(state) == blk::StickyPiston) changed(s, p, state);
}
bool Pistons::event(Server& s, PistonPos p, uint16_t state, int type, int data) {
    if (data < 0 || data > 5 || type < 0 || type > 2) return false;
    int f = pistonFacing(state);
    bool power = powered(s, p, f), sticky = blockIdOf(state) == blk::StickyPiston;
    if (power && (type == 1 || type == 2)) {
        pistonSet(s, p, setBool(state, "extended", true), 2);
        return false;
    }
    if (!power && type == 0) return false;
    if (type == 0) {
        if (!move(s, p, f, true, sticky)) return false;
        pistonSet(s, p, setBool(state, "extended", true), 67);
        s.playSound("block.piston.extend", p.x + .5, p.y + .5, p.z + .5, .5f, .7f);
    } else if (type == 1 || type == 2) {
        PistonPos head = p.offset(f), target = p.offset(f, 2);
        finish(s, head, true);
        uint16_t base = setPropStr(sticky ? bs::StickyPiston : bs::Piston, "facing", pistonFaces[data & 7]);
        if (!moving(s, p, base, f, false, true, sticky)) return false;
        s.redstone.neighbours(s, p.x, p.y, p.z);
        bool dropped = false;
        if (sticky) {
            TileEntity* t = pistonTile(s, target);
            if (t && t->pistonExtending && t->pistonFace == f) {
                finish(s, target, true);
                dropped = true;
            }
        }
        if (!dropped) {
            uint16_t pull = at(s, target);
            if (sticky && type == 1 && !stateIsAir(pull) && PistonPlan::pushable(s, target, f ^ 1, false, f) &&
                (statePushReaction(pull) == PUSH_NORMAL || blockIdOf(pull) == blk::Piston ||
                 blockIdOf(pull) == blk::StickyPiston)) {
                move(s, p, f, false, sticky);
            } else
                pistonSet(s, head, bs::Air, 3);
        }
        s.playSound("block.piston.contract", p.x + .5, p.y + .5, p.z + .5, .5f, .65f);
    }
    return true;
}
void Pistons::tick(Server& s, PistonPos p) {
    TileEntity* t = pistonTile(s, p);
    if (!t) return;
    t->updated = s.worldTick();
    t->pistonPrevious = t->pistonProgress;
    if (t->pistonPrevious >= 2)
        finish(s, p, false);
    else {
        pushEntities(s, p, *t);
        ++t->pistonProgress;
    }
    s.world.markDirty(s.curDim, p.x >> 4, p.z >> 4);
}
} // namespace mc

namespace mc {
bool PistonBox::intersects(const PistonBox& b) const {
    for (int a = 0; a < 3; ++a)
        if (hi[a] <= b.lo[a] || lo[a] >= b.hi[a]) return false;
    return true;
}
namespace {
int axisOf(int face) {
    return face < 2 ? 1 : face < 4 ? 2 : 0;
}
PistonBox entityBox(const Entity& e) {
    return {{e.x - e.width * .5, e.y, e.z - e.width * .5}, {e.x + e.width * .5, e.y + e.height, e.z + e.width * .5}};
}
int stateBoxes(uint16_t state, PistonPos p, double dx, double dy, double dz, PistonBox* out) {
    const int8_t* shape = COLLISION_SHAPES + COLLISION_SHAPE_OFFSETS[state];
    int n = *shape++;
    for (int i = 0; i < n; ++i) {
        double shift[] = {p.x + dx, p.y + dy, p.z + dz};
        for (int a = 0; a < 3; ++a) {
            out[i].lo[a] = shape[a] / 32.0 + shift[a];
            out[i].hi[a] = shape[a + 3] / 32.0 + shift[a];
        }
        shape += 6;
    }
    return n;
}
PistonBox sweep(PistonBox box, int face, double distance) {
    int a = axisOf(face);
    if (face & 1) {
        box.lo[a] = box.hi[a];
        box.hi[a] += distance;
    } else {
        box.hi[a] = box.lo[a];
        box.lo[a] -= distance;
    }
    return box;
}
} // namespace
int Pistons::collision(Server& s, PistonPos p, PistonBox* out, int ignored) {
    uint16_t state = s.world.getBlock(s.curDim, p.x, p.y, p.z, bs::Stone);
    if (blockIdOf(state) != blk::MovingPiston) return stateBoxes(state, p, 0, 0, 0, out);
    TileEntity* t = pistonTile(s, p);
    if (!t) return 0;
    int n = 0, face = t->pistonFace, direction = t->pistonExtending ? face : face ^ 1;
    if (!t->pistonExtending && t->pistonSource)
        n = stateBoxes(setBool(t->movedState, "extended", true), p, 0, 0, 0, out);
    if (t->pistonProgress < 2 && direction == ignored) return n;
    double progress = t->pistonProgress * .5;
    uint16_t movingState = t->movedState;
    if (t->pistonSource) {
        movingState = setPropStr(bs::PistonHead, "facing", pistonFaces[face]);
        movingState =
            setPropStr(movingState, "type", blockIdOf(t->movedState) == blk::StickyPiston ? "sticky" : "normal");
        movingState = setBool(movingState, "short", t->pistonExtending != (1 - progress < .25));
    }
    double offset = t->pistonExtending ? progress - 1 : 1 - progress;
    return n +
           stateBoxes(movingState, p, offset * FACE_DX[face], offset * FACE_DY[face], offset * FACE_DZ[face], out + n);
}
void Pistons::displace(Server& s, Entity& e, int face, double distance, int ignored) {
    int a = axisOf(face), sign = (face & 1) ? 1 : -1;
    if (e.pistonMoveTick != s.worldTick()) {
        e.pistonMoveTick = s.worldTick();
        for (float& v : e.pistonMove)
            v = 0;
    }
    double total = std::max(-.51, std::min(.51, e.pistonMove[a] + distance * sign));
    double delta = total - e.pistonMove[a];
    e.pistonMove[a] = total;
    if (fabs(delta) < 1e-5) return;
    PistonBox bounds = entityBox(e), swept = bounds;
    if (delta > 0)
        swept.hi[a] += delta;
    else
        swept.lo[a] += delta;
    // Clip only along the piston's movement axis. Ignore the moving surface
    // responsible for this displacement, but still collide with stationary blocks.
    for (int x = (int)floor(swept.lo[0]) - 1; x <= (int)floor(swept.hi[0]) + 1; ++x)
        for (int y = (int)floor(swept.lo[1]) - 1; y <= (int)floor(swept.hi[1]) + 1; ++y)
            for (int z = (int)floor(swept.lo[2]) - 1; z <= (int)floor(swept.hi[2]) + 1; ++z) {
                PistonBox boxes[16];
                int n = collision(s, {x, y, z}, boxes, ignored);
                for (int i = 0; i < n; ++i) {
                    bool overlap = true;
                    for (int b = 0; b < 3; ++b)
                        if (b != a && (bounds.hi[b] <= boxes[i].lo[b] || bounds.lo[b] >= boxes[i].hi[b]))
                            overlap = false;
                    if (!overlap) continue;
                    if (delta > 0 && bounds.hi[a] <= boxes[i].lo[a] + 1e-7)
                        delta = std::min(delta, boxes[i].lo[a] - bounds.hi[a]);
                    if (delta < 0 && bounds.lo[a] >= boxes[i].hi[a] - 1e-7)
                        delta = std::max(delta, boxes[i].hi[a] - bounds.lo[a]);
                }
            }
    if (a == 0)
        e.x += delta;
    else if (a == 1)
        e.y += delta;
    else
        e.z += delta;
}
void Pistons::pushEntities(Server& s, PistonPos p, const TileEntity& t) {
    int face = t.pistonFace, direction = t.pistonExtending ? face : face ^ 1, a = axisOf(direction);
    uint16_t state = t.movedState;
    if (t.pistonSource && !t.pistonExtending)
        state = setBool(setPropStr(bs::PistonHead, "facing", pistonFaces[face]), "short", t.pistonProgress > 0);
    double offset = t.pistonExtending ? t.pistonProgress * .5 - 1 : 1 - t.pistonProgress * .5;
    PistonBox boxes[16];
    int n = stateBoxes(state, p, offset * FACE_DX[face], offset * FACE_DY[face], offset * FACE_DZ[face], boxes);
    auto push = [&](Entity& e) {
        if (e.kind == EK_NONE || e.removed || e.dim != s.curDim) return;
        if (e.kind == EK_PLAYER && e.playerSlot >= 0 && e.playerSlot < MC_MAX_PLAYERS &&
            s.players[e.playerSlot].gamemode == GM_SPECTATOR)
            return;
        PistonBox eb = entityBox(e);
        double amount = 0;
        for (int i = 0; i < n; ++i) {
            PistonBox swept = sweep(boxes[i], direction, .5);
            if (swept.intersects(eb))
                amount = std::max(amount, (direction & 1) ? swept.hi[a] - eb.lo[a] : eb.hi[a] - swept.lo[a]);
        }
        if (amount > 0) {
            if (blockIdOf(state) == blk::SlimeBlock) {
                if (e.kind == EK_PLAYER) return; // the Java client applies slime velocity itself
                double v = (direction & 1) ? 1 : -1;
                if (a == 0)
                    e.vx = v;
                else if (a == 1)
                    e.vy = v;
                else
                    e.vz = v;
                e.velDirty = true;
            }
            displace(s, e, direction, std::min(.5, amount) + .01, direction);
        }
        if (blockIdOf(state) == blk::HoneyBlock && a != 1 && e.onGround) {
            double top = 0;
            for (int i = 0; i < n; ++i)
                top = std::max(top, boxes[i].hi[1]);
            double x = p.x + offset * FACE_DX[face], z = p.z + offset * FACE_DZ[face];
            PistonBox carried{{x, top, z}, {x + 1, p.y + 1.5000001, z + 1}};
            if (e.x >= x && e.x <= x + 1 && e.z >= z && e.z <= z + 1 && carried.intersects(entityBox(e)))
                displace(s, e, direction, .5, direction);
        }
    };
    for (Entity& e : s.entities)
        push(e);
    for (Player& player : s.players)
        if (player.state == CS_PLAY) push(player.e);
}
} // namespace mc
