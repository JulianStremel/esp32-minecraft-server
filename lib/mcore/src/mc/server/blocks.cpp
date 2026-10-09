// Block interaction: digging, placing, using blocks, neighbour updates, fluids
// and random ticks (crop growth etc.).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc/nbt.h"
#include "mc/registry.h"
#include "mc/server/chunk_codec.h"
#include "mc/server/server.h"
#include "mc/server/books.h"
#include "mc/world/noise.h"

namespace mc {

static Rng s_brng(0xB10C);

static inline bool endsWith(const char* s, const char* suf) {
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcmp(s + a - b, suf);
}
static inline bool nameHas(uint16_t blockId, const char* part) { return strstr(BLOCKS[blockId].name, part) != nullptr; }
static inline const char* bname(uint16_t state) { return blockOf(state).name; }

static const char* const HFACING[4] = {"south", "west", "north", "east"};  // by yaw quadrant
static const char* const FACE_NAME[6] = {"down", "up", "north", "south", "west", "east"};
static int oppositeFace(int f) { return f ^ 1; }
// Clockwise neighbour direction of a horizontal facing (north -> east -> south -> west).
static void clockwiseVec(int face, int& dx, int& dz) {
    switch (face) {
        case 2: dx = 1; dz = 0; break;    // north -> east
        case 5: dx = 0; dz = 1; break;    // east -> south
        case 3: dx = -1; dz = 0; break;   // south -> west
        default: dx = 0; dz = -1; break;  // west -> north
    }
}

static int yawQuadrant(float yaw) {
    int q = (int)floorf(yaw / 90.0f + 0.5f);
    return q & 3;
}
static const char* playerFacing(const Player& p) { return HFACING[yawQuadrant(p.e.yaw)]; }
static const char* playerFacingOpposite(const Player& p) { return HFACING[(yawQuadrant(p.e.yaw) + 2) & 3]; }
static int faceIndexOf(const char* dir) {
    for (int i = 0; i < 6; i++)
        if (!strcmp(FACE_NAME[i], dir)) return i;
    return 2;
}
// nearest looking direction including up/down
static const char* lookDirection(const Player& p) {
    if (p.e.pitch > 45) return "down";
    if (p.e.pitch < -45) return "up";
    return playerFacing(p);
}

static bool isReplaceable(uint16_t s) {
    if (stateIsAir(s)) return true;
    const BlockDef& b = blockOf(s);
    if (blockIdOf(s) == blk::Snow) return getProp(s, "layers") == 0;  // a single layer
    return (b.flags & BF_REPLACEABLE) != 0;
}

static bool isFluidSource(uint16_t s, uint16_t fluid) { return blockIdOf(s) == fluid && getProp(s, "level") == 0; }

// ====================================================================== digging
float Server::digTicks(Player& p, uint16_t state) {
    const BlockDef& b = blockOf(state);
    if (b.hardness < 0 || !(b.flags & BF_DIGGABLE)) return 1e9f;
    if (b.hardness == 0) return 0;
    ItemStack& h = p.heldItem();
    float speed = 1.0f;
    bool canHarvest = !(b.flags & BF_TOOL_REQUIRED);
    if (!h.empty()) {
        const ItemDef& d = ITEMS[h.id];
        static const float TIER_SPEED[6] = {2, 4, 6, 8, 9, 12};
        static const int TIER_LEVEL[6] = {1, 2, 3, 4, 4, 1};
        bool matches = (d.kind == IK_PICKAXE && b.toolClass == TC_PICKAXE) || (d.kind == IK_AXE && b.toolClass == TC_AXE) ||
                       (d.kind == IK_SHOVEL && b.toolClass == TC_SHOVEL) || (d.kind == IK_HOE && b.toolClass == TC_HOE);
        if (matches) {
            speed = TIER_SPEED[d.tier];
            if (b.flags & BF_TOOL_REQUIRED) canHarvest = TIER_LEVEL[d.tier] >= b.minTier;
        }
        if (d.kind == IK_SHEARS && b.toolClass == TC_SHEARS) { speed = blockIdOf(state) == blk::Cobweb ? 15 : 5; canHarvest = true; }
        if (d.kind == IK_SWORD && blockIdOf(state) == blk::Cobweb) { speed = 15; canHarvest = true; }
        if (d.kind == IK_SWORD && b.toolClass == TC_SHEARS) speed = 1.5f;
    }
    float dmg = speed / b.hardness / (canHarvest ? 30.0f : 100.0f);
    if (dmg >= 1) return 0;
    return ceilf(1.0f / dmg);
}

static bool canHarvest(Player& p, uint16_t state) {
    const BlockDef& b = blockOf(state);
    if (!(b.flags & BF_TOOL_REQUIRED)) return true;
    ItemStack& h = p.heldItem();
    if (h.empty()) return false;
    const ItemDef& d = ITEMS[h.id];
    static const int TIER_LEVEL[6] = {1, 2, 3, 4, 4, 1};
    if (b.toolClass == TC_PICKAXE && d.kind == IK_PICKAXE) return TIER_LEVEL[d.tier] >= b.minTier;
    if (b.toolClass == TC_SHEARS && (d.kind == IK_SHEARS || d.kind == IK_SWORD)) return true;
    if (b.toolClass == TC_AXE && d.kind == IK_AXE) return true;
    if (b.toolClass == TC_SHOVEL && d.kind == IK_SHOVEL) return true;
    return false;
}

// The client predicts digging; the server's block (and the acknowledged sequence number,
// see handlePlay) settles it. Refused digs get the block back.
static void ackDig(Player& p, int x, int y, int z, int status, bool ok) {
    (void)status;
    if (ok) return;   // the change was broadcast
    Packet pk(pkt::s2c::BlockChange);
    pk.w.u64(packPos(x, y, z));
    pk.w.varint(p.srv->blockAt(x, y, z));
    p.conn.send(pk);
}

static void breakAnimation(Server& s, Player& p, int stage) {
    Packet pk(pkt::s2c::BlockBreakAnimation);
    pk.w.varint(p.e.id);
    pk.w.u64(packPos(p.digX, p.digY, p.digZ));
    pk.w.i8((int8_t)stage);
    s.broadcastNear(pk, p.digX >> 4, p.digZ >> 4, &p);
}

void Player::onDig(Reader& r) {
    int status = r.varint();
    int x, y, z;
    unpackPos(r.u64(), x, y, z);
    r.i8();
    lastSequence = r.varint();
    if (!r.ok()) return;
    Server& s = *srv;
    switch (status) {
        case 0:
        case 2: {
            if (dead || gamemode == GM_SPECTATOR || gamemode == GM_ADVENTURE) { ackDig(*this, x, y, z, status, false); return; }
            double dx = x + 0.5 - e.x, dy = y + 0.5 - (e.y + 1.62), dz = z + 0.5 - e.z;
            uint16_t st = s.blockAt(x, y, z);
            if (dx * dx + dy * dy + dz * dz > 7.5 * 7.5 || !s.world.blockInBounds(x, z) || stateIsAir(st) ||
                stateIsFluid(st)) {
                ackDig(*this, x, y, z, status, false);
                return;
            }
            if (gamemode == GM_CREATIVE) {
                ItemStack& h = heldItem();
                if (!h.empty() && ITEMS[h.id].kind == IK_SWORD) { ackDig(*this, x, y, z, status, false); return; }
                s.breakBlock(x, y, z, this, false);
                ackDig(*this, x, y, z, status, true);
                return;
            }
            float need = s.digTicks(*this, st);
            if (status == 0) {
                if (blockIdOf(st) == blk::NoteBlock) s.redstone.playNote(s, x, y, z);
                if (need >= 1e8f) { ackDig(*this, x, y, z, status, false); return; }
                if (need <= 0) {
                    s.breakBlock(x, y, z, this, canHarvest(*this, st));
                    ackDig(*this, x, y, z, status, true);
                    return;
                }
                digging = true;
                digX = x; digY = y; digZ = z;
                digStart = s.ticks;
                digStage = -1;
                // the block as it is: a client that wrongly predicted the break (it would
                // keep a hole mobs walk over) gets it back with the acknowledgement
                ackDig(*this, x, y, z, status, false);
                return;
            }
            // finished: allow for latency and client-side bonuses we do not model
            bool ok = digging && digX == x && digY == y && digZ == z && (float)(s.ticks - digStart) >= need * 0.6f - 4;
            if (!ok && need <= 0) ok = true;
            if (digging) breakAnimation(s, *this, -1);
            digging = false;
            if (!ok) {
                ackDig(*this, x, y, z, status, false);
                return;
            }
            bool harvest = canHarvest(*this, st);
            s.breakBlock(x, y, z, this, harvest);
            ackDig(*this, x, y, z, status, true);
            ItemStack& h = heldItem();
            if (!h.empty() && ITEMS[h.id].durability && blockOf(st).hardness > 0) {
                uint8_t k = ITEMS[h.id].kind;
                s.damageHeldItem(*this, (k == IK_SWORD) ? 2 : 1);
            }
            s.addExhaustion(*this, 0.005f);
            return;
        }
        case 1:
            if (digging) breakAnimation(s, *this, -1);
            digging = false;
            ackDig(*this, x, y, z, status, false);   // aborted: nothing broke
            return;
        case 3:
        case 4: {
            ItemStack& h = heldItem();
            if (h.empty() || dead) return;
            ItemStack drop = h;
            if (status == 4) drop.count = 1;
            s.throwItem(*this, drop);
            h.count = (uint8_t)(h.count - drop.count);
            if (!h.count) h.clear();
            sendSlot(SLOT_HOTBAR_START + held);
            s.broadcastEquipment(*this);
            return;
        }
        case 5: {
            usingTicks = 0;
            if (drawingBow) {
                drawingBow = false;
                ItemStack& h = heldItem();
                if (h.id != itm::Bow) return;
                // find arrows
                int arrowSlot = -1;
                for (int i = SLOT_MAIN_START; i < SLOT_OFFHAND + 1 && arrowSlot < 0; i++)
                    if (inv[i].id == itm::Arrow) arrowSlot = i;
                if (arrowSlot < 0 && gamemode != GM_CREATIVE) return;
                float c = (s.ticks - bowStart) / 20.0f;
                float power = (c * c + c * 2) / 3;
                if (power < 0.1f) return;
                if (power > 1) power = 1;
                double yaw = e.yaw * M_PI / 180.0, pitch = e.pitch * M_PI / 180.0;
                Entity* a = s.spawnEntity(EK_ARROW, ent::Arrow, e.x, e.y + 1.52, e.z);
                if (!a) return;
                double sp = power * 3.0;
                a->vx = -sin(yaw) * cos(pitch) * sp;
                a->vy = -sin(pitch) * sp;
                a->vz = cos(yaw) * cos(pitch) * sp;
                a->owner = e.id;
                a->damage = power >= 1 ? 2.5f : 2.0f;
                a->yaw = e.yaw;
                a->pitch = e.pitch;
                a->velDirty = true;
                s.playSound("entity.arrow.shoot", e.x, e.y, e.z, 1, 1.0f / (0.8f + power * 0.5f), 7);
                if (gamemode != GM_CREATIVE) {
                    if (--inv[arrowSlot].count == 0) inv[arrowSlot].clear();
                    sendSlot(arrowSlot);
                    s.damageHeldItem(*this, 1);
                }
            }
            return;
        }
        case 6: {
            ItemStack t = inv[SLOT_OFFHAND];
            inv[SLOT_OFFHAND] = heldItem();
            heldItem() = t;
            sendSlot(SLOT_OFFHAND);
            sendSlot(SLOT_HOTBAR_START + held);
            s.broadcastEquipment(*this);
            return;
        }
        default: return;
    }
}

// ====================================================================== breaking
static void dropStack(Server& s, int x, int y, int z, uint16_t item, int count) {
    if (item && count > 0) s.dropItem(x + 0.5, y + 0.3, z + 0.5, ItemStack::of(item, count));
}

static uint16_t saplingFor(uint16_t leavesId) {
    switch (leavesId) {
        case blk::OakLeaves: return itm::OakSapling;
        case blk::SpruceLeaves: return itm::SpruceSapling;
        case blk::BirchLeaves: return itm::BirchSapling;
        case blk::JungleLeaves: return itm::JungleSapling;
        case blk::AcaciaLeaves: return itm::AcaciaSapling;
        case blk::DarkOakLeaves: return itm::DarkOakSapling;
        default: return 0;
    }
}

static void dropsFor(Server& s, Player* by, int x, int y, int z, uint16_t st) {
    uint16_t id = blockIdOf(st);
    const BlockDef& b = BLOCKS[id];
    int age = getProp(st, "age");
    switch (id) {
        case blk::Gravel:
            dropStack(s, x, y, z, s_brng.range(10) == 0 ? itm::Flint : itm::Gravel, 1);
            return;
        case blk::ShortGrass: case blk::TallGrass: case blk::Fern: case blk::LargeFern:
            if (s_brng.range(8) == 0) dropStack(s, x, y, z, itm::WheatSeeds, 1);
            return;
        case blk::Wheat:
            if (age == 7) { dropStack(s, x, y, z, itm::Wheat, 1); dropStack(s, x, y, z, itm::WheatSeeds, s_brng.range(4)); }
            else dropStack(s, x, y, z, itm::WheatSeeds, 1);
            return;
        case blk::Carrots: dropStack(s, x, y, z, itm::Carrot, age == 7 ? 1 + s_brng.range(4) : 1); return;
        case blk::Potatoes:
            dropStack(s, x, y, z, itm::Potato, age == 7 ? 1 + s_brng.range(4) : 1);
            if (age == 7 && s_brng.range(50) == 0) dropStack(s, x, y, z, itm::PoisonousPotato, 1);
            return;
        case blk::Beetroots:
            if (age == 3) { dropStack(s, x, y, z, itm::Beetroot, 1); dropStack(s, x, y, z, itm::BeetrootSeeds, 1 + s_brng.range(3)); }
            else dropStack(s, x, y, z, itm::BeetrootSeeds, 1);
            return;
        case blk::Snow: dropStack(s, x, y, z, itm::Snowball, getProp(st, "layers") + 1); return;
        default: break;
    }
    if (strstr(b.name, "_leaves")) {
        bool shears = by && by->heldItem().id == itm::Shears;
        if (shears) { dropStack(s, x, y, z, b.item, 1); return; }
        if (s_brng.range(20) == 0) dropStack(s, x, y, z, saplingFor(id), 1);
        if (s_brng.range(50) == 0) dropStack(s, x, y, z, itm::Stick, 1 + s_brng.range(2));
        if ((id == blk::OakLeaves || id == blk::DarkOakLeaves) && s_brng.range(200) == 0) dropStack(s, x, y, z, itm::Apple, 1);
        return;
    }
    if (strstr(b.name, "_slab") && getProp(st, "type") >= 0) {
        const char* t = getPropStr(st, "type");
        dropStack(s, x, y, z, b.item, t && !strcmp(t, "double") ? 2 : 1);
        return;
    }
    if (strstr(b.name, "_door") || strstr(b.name, "_bed") || id == blk::Sunflower || id == blk::Lilac ||
        id == blk::RoseBush || id == blk::Peony) {
        const char* half = getPropStr(st, "half");
        const char* part = getPropStr(st, "part");
        if ((half && !strcmp(half, "upper")) || (part && !strcmp(part, "head"))) return;  // dropped by the other half
    }
    int n = b.dropMin + (b.dropMax > b.dropMin ? s_brng.range(b.dropMax - b.dropMin + 1) : 0);
    dropStack(s, x, y, z, b.dropItem, n);
    // experience from ores
    if (by) {
        int xp = 0;
        if (id == blk::CoalOre) xp = s_brng.range(3);
        else if (id == blk::DiamondOre || id == blk::EmeraldOre) xp = 3 + s_brng.range(5);
        else if (id == blk::LapisOre || id == blk::NetherQuartzOre) xp = 2 + s_brng.range(4);
        else if (id == blk::RedstoneOre) xp = 1 + s_brng.range(5);
        if (xp) s.giveXp(*by, xp);
    }
}

void Server::breakBlock(int x, int y, int z, Player* by, bool drops) {
    uint16_t st = blockAt(x, y, z);
    if (stateIsAir(st) || !dimHasY(curDim, y)) return;
    vibration(x + .5, y + .5, z + .5, GE_BLOCK_DESTROY);
    uint16_t id = blockIdOf(st);
    if (id == blk::Tnt && getBool(st,"unstable") && by && by->gamemode != GM_CREATIVE) {
        primeTnt(x,y,z,by->e.id);
        return;
    }
    if (id == blk::Tripwire && by && by->heldItem().id == itm::Shears) {
        st = setBool(st, "disarmed", true);
        world.setBlock(curDim,x,y,z,st,true,4);
    }
    if (id == blk::Bedrock && (!by || by->gamemode != GM_CREATIVE)) return;
    // container contents
    Chunk* c = world.get(curDim, x >> 4, z >> 4);
    if (c) {
        TileEntity* t = c->tileAt(x & 15, y, z & 15);
        if (t) {
            for (int i = 0; i < t->slotCount(); i++)
                if (!t->items[i].empty()) dropItem(x + 0.5, y + 0.5, z + 0.5, t->items[i]);
            c->removeTile(x & 15, y, z & 15);
            c->dirty = true;
        }
    }
    if (drops) dropsFor(*this, by, x, y, z, st);
    // remaining fluid: waterlogged blocks leave water behind, ice melts
    uint16_t replacement = 0;
    if (getProp(st, "waterlogged") == 0) replacement = bs::Water;
    if (id == blk::Ice && by && by->isSurvivalLike()) {
        uint16_t below = blockAt(x, y - 1, z);
        if (stateCollides(below) || stateIsFluid(below)) replacement = bs::Water;
    }
    {
        Packet pk(pkt::s2c::WorldEvent);
        pk.w.i32(2001);
        pk.w.u64(packPos(x, y, z));
        pk.w.i32(st);
        pk.w.boolean(false);
        broadcastNear(pk, x >> 4, z >> 4, by);
    }
    setBlock(x, y, z, replacement);
    // the other half of two-block structures
    const char* half = getPropStr(st, "half");
    if (half && (!strcmp(half, "upper") || !strcmp(half, "lower")) && !strstr(BLOCKS[id].name, "stairs") &&
        !strstr(BLOCKS[id].name, "trapdoor")) {
        int oy = !strcmp(half, "upper") ? y - 1 : y + 1;
        if (blockIdOf(blockAt(x, oy, z)) == id) {
            if (!strcmp(half, "upper") && drops) dropsFor(*this, by, x, oy, z, blockAt(x, oy, z));
            setBlock(x, oy, z, 0);
        }
    }
    // an extended piston and its head go together (PistonHeadBlock#playerWillDestroy):
    // the base drops itself, the head nothing
    if (((id == blk::Piston || id == blk::StickyPiston) && getBool(st, "extended")) || id == blk::PistonHead) {
        int f = faceIndexOf(getPropStr(st, "facing"));
        int sgn = id == blk::PistonHead ? -1 : 1;
        int ox = x + FACE_DX[f] * sgn, oy = y + FACE_DY[f] * sgn, oz = z + FACE_DZ[f] * sgn;
        uint16_t other = blockAt(ox, oy, oz);
        uint16_t oid = blockIdOf(other);
        bool pair = id == blk::PistonHead ? (oid == blk::Piston || oid == blk::StickyPiston) && getBool(other, "extended")
                                          : oid == blk::PistonHead;
        if (pair && faceIndexOf(getPropStr(other, "facing")) == f) {
            if (id == blk::PistonHead && drops) dropsFor(*this, by, ox, oy, oz, other);
            setBlock(ox, oy, oz, 0);
        }
    }
    const char* part = getPropStr(st, "part");
    if (part && strstr(BLOCKS[id].name, "_bed")) {
        int f = faceIndexOf(getPropStr(st, "facing"));
        int sgn = !strcmp(part, "foot") ? 1 : -1;
        int ox = x + FACE_DX[f] * sgn, oz = z + FACE_DZ[f] * sgn;
        if (blockIdOf(blockAt(ox, y, oz)) == id) {
            if (!strcmp(part, "head") && drops) dropStack(*this, x, y, z, BLOCKS[id].item, 1);
            setBlock(ox, y, oz, 0);
        }
    }
}

// ====================================================================== neighbour updates
static bool fullSolid(uint16_t s) { return stateCollides(s) && stateOpaque(s); }

bool Server::canSupport(uint16_t st, int x, int y, int z) {
    uint16_t id = blockIdOf(st);
    const BlockDef& b = BLOCKS[id];
    uint16_t below = blockAt(x, y - 1, z);
    uint16_t belowId = blockIdOf(below);
    if (b.flags & BF_NEEDS_SUPPORT) {
        if (id == blk::Wheat || id == blk::Carrots || id == blk::Potatoes || id == blk::Beetroots || id == blk::MelonStem ||
            id == blk::PumpkinStem)
            return belowId == blk::Farmland;
        if (id == blk::SugarCane) return belowId == blk::SugarCane || belowId == blk::GrassBlock || belowId == blk::Dirt ||
                                        belowId == blk::Sand || belowId == blk::RedSand || belowId == blk::Podzol || belowId == blk::CoarseDirt;
        if (id == blk::Cactus) {
            if (belowId != blk::Cactus && belowId != blk::Sand && belowId != blk::RedSand) return false;
            for (int f = 2; f < 6; f++)
                if (stateCollides(blockAt(x + FACE_DX[f], y, z + FACE_DZ[f]))) return false;
            return true;
        }
        if (id == blk::DeadBush) return belowId == blk::Sand || belowId == blk::RedSand || nameHas(belowId, "terracotta") || belowId == blk::Dirt;
        if (id == blk::BrownMushroom || id == blk::RedMushroom) return fullSolid(below);
        // flowers, saplings, grass: need dirt-like ground (or the lower half of a tall plant)
        const char* half = getPropStr(st, "half");
        if (half && !strcmp(half, "upper")) return belowId == id;
        return belowId == blk::GrassBlock || belowId == blk::Dirt || belowId == blk::CoarseDirt || belowId == blk::Podzol ||
               belowId == blk::Farmland || belowId == blk::Mycelium;
    }
    const char* n = b.name;
    if (id == blk::TripwireHook) {
        int f = faceIndexOf(getPropStr(st,"facing"));
        return stateFaceSturdy(blockAt(x-FACE_DX[f],y,z-FACE_DZ[f]),f);
    }
    if (id == blk::RedstoneWire) return stateFaceSturdy(below, 1) || belowId == blk::Hopper;
    if (id == blk::Torch || id == blk::RedstoneTorch || id == blk::SoulTorch || endsWith(n, "_carpet") ||
        strstr(n, "pressure_plate") || id == blk::RedstoneWire || strstr(n, "rail") || id == blk::Snow ||
        id == blk::Repeater || id == blk::Comparator || (strstr(n, "_sign") && !strstr(n, "wall")) ||
        (strstr(n, "_banner") && !strstr(n, "wall")))
        return stateCollides(below) && !stateIsFluid(below);
    if (id == blk::WallTorch || id == blk::RedstoneWallTorch || id == blk::SoulWallTorch || id == blk::Ladder ||
        strstr(n, "wall_sign") || strstr(n, "wall_banner")) {
        int f = faceIndexOf(getPropStr(st, "facing"));
        int bf = oppositeFace(f);
        return stateCollides(blockAt(x + FACE_DX[bf], y, z + FACE_DZ[bf]));
    }
    if (strstr(n, "_button") || id == blk::Lever) {
        const char* face = getPropStr(st, "face");
        if (!strcmp(face, "floor")) return stateCollides(below);
        if (!strcmp(face, "ceiling")) return stateCollides(blockAt(x, y + 1, z));
        int f = faceIndexOf(getPropStr(st, "facing"));
        int bf = oppositeFace(f);
        return stateCollides(blockAt(x + FACE_DX[bf], y, z + FACE_DZ[bf]));
    }
    if (strstr(n, "_door")) {
        const char* half = getPropStr(st, "half");
        if (!strcmp(half, "upper")) return blockIdOf(below) == id;
        return stateCollides(below);
    }
    return true;
}

// Connection rules for fences, panes, bars and walls.
static bool connectsTo(uint16_t self, uint16_t other, int face) {
    uint16_t a = blockIdOf(self), b = blockIdOf(other);
    const char* an = BLOCKS[a].name;
    const char* bn = BLOCKS[b].name;
    if (a == b) return true;
    bool selfFence = endsWith(an, "_fence"), otherFence = endsWith(bn, "_fence");
    bool selfPane = endsWith(an, "glass_pane") || a == blk::IronBars;
    bool otherPane = endsWith(bn, "glass_pane") || b == blk::IronBars;
    bool selfWall = endsWith(an, "_wall"), otherWall = endsWith(bn, "_wall");
    if (selfFence && otherFence) {
        bool nether = a == blk::NetherBrickFence, onether = b == blk::NetherBrickFence;
        return nether == onether;
    }
    if (selfPane && otherPane) return true;
    if (selfWall && otherWall) return true;
    if ((selfFence || selfWall) && strstr(bn, "fence_gate")) {
        int f = faceIndexOf(getPropStr(other, "facing"));
        return (f >= 4) == (face < 4);  // gate perpendicular to the connection
    }
    if (selfPane && selfWall) return false;
    return fullSolid(other);
}

static uint16_t computeShape(Server& s, uint16_t st, int x, int y, int z) {
    uint16_t id = blockIdOf(st);
    const char* n = BLOCKS[id].name;
    bool fence = endsWith(n, "_fence"), pane = endsWith(n, "glass_pane") || id == blk::IronBars, wall = endsWith(n, "_wall");
    if (fence || pane || wall) {
        static const char* dirs[4] = {"north", "south", "west", "east"};
        static const int faces[4] = {2, 3, 4, 5};
        bool any = false;
        for (int i = 0; i < 4; i++) {
            int f = faces[i];
            bool c = connectsTo(st, s.blockAt(x + FACE_DX[f], y, z + FACE_DZ[f]), f);
            any |= c;
            if (wall) st = setPropStr(st, dirs[i], c ? "low" : "none");
            else st = setBool(st, dirs[i], c);
        }
        if (wall) {
            bool ns = !strcmp(getPropStr(st, "north"), "low") && !strcmp(getPropStr(st, "south"), "low");
            bool ew = !strcmp(getPropStr(st, "east"), "low") && !strcmp(getPropStr(st, "west"), "low");
            bool straight = (ns && !strcmp(getPropStr(st, "east"), "none") && !strcmp(getPropStr(st, "west"), "none")) ||
                            (ew && !strcmp(getPropStr(st, "north"), "none") && !strcmp(getPropStr(st, "south"), "none"));
            bool above = !stateIsAir(s.blockAt(x, y + 1, z));
            st = setBool(st, "up", !straight || above || !any);
        }
        return st;
    }
    if (endsWith(n, "_stairs")) {
        // vanilla StairBlock.getStairsShape
        int f = faceIndexOf(getPropStr(st, "facing"));
        const char* half = getPropStr(st, "half");
        auto isStairs = [&](uint16_t o) { return endsWith(bname(o), "_stairs"); };
        auto leftOf = [](int face) {  // counter clockwise
            switch (face) { case 2: return 4; case 4: return 3; case 3: return 5; default: return 2; }
        };
        auto rightOf = [](int face) {  // clockwise
            switch (face) { case 2: return 5; case 5: return 3; case 3: return 4; default: return 2; }
        };
        auto sameHalf = [&](uint16_t o) { return !strcmp(getPropStr(o, "half"), half); };
        auto canTakeShape = [&](int face) {
            uint16_t o = s.blockAt(x + FACE_DX[face], y, z + FACE_DZ[face]);
            return !isStairs(o) || faceIndexOf(getPropStr(o, "facing")) != f || !sameHalf(o);
        };
        const char* shape = "straight";
        uint16_t front = s.blockAt(x + FACE_DX[f], y, z + FACE_DZ[f]);
        if (isStairs(front) && sameHalf(front)) {
            int f2 = faceIndexOf(getPropStr(front, "facing"));
            if ((f2 >= 4) != (f >= 4) && canTakeShape(oppositeFace(f2)))
                shape = f2 == leftOf(f) ? "outer_left" : "outer_right";
        }
        if (!strcmp(shape, "straight")) {
            int bf = oppositeFace(f);
            uint16_t back = s.blockAt(x + FACE_DX[bf], y, z + FACE_DZ[bf]);
            if (isStairs(back) && sameHalf(back)) {
                int f2 = faceIndexOf(getPropStr(back, "facing"));
                if ((f2 >= 4) != (f >= 4) && canTakeShape(f2)) shape = f2 == leftOf(f) ? "inner_left" : "inner_right";
            }
        }
        (void)rightOf;
        return setPropStr(st, "shape", shape);
    }
    if (id == blk::GrassBlock || id == blk::Podzol || id == blk::Mycelium) {
        uint16_t above = blockIdOf(s.blockAt(x, y + 1, z));
        return setBool(st, "snowy", above == blk::Snow || above == blk::SnowBlock);
    }
    if (id == blk::Chest || id == blk::TrappedChest) {
        const char* type = getPropStr(st, "type");
        if (strcmp(type, "single")) {
            // vanilla: a LEFT half has its partner clockwise of its facing, RIGHT counter-clockwise
            int f = faceIndexOf(getPropStr(st, "facing"));
            int cx, cz;
            clockwiseVec(f, cx, cz);
            int sgn = !strcmp(type, "left") ? 1 : -1;
            uint16_t o = s.blockAt(x + cx * sgn, y, z + cz * sgn);
            if (blockIdOf(o) != id || !strcmp(getPropStr(o, "type"), "single")) return setPropStr(st, "type", "single");
        }
    }
    return st;
}

void Server::updateNeighbors(int x, int y, int z) {
    // iterative work list to avoid deep recursion
    struct P { int x, y, z; };
    P q[64];
    int head = 0, tail = 0;
    auto push = [&](int a, int b, int c) {
        if (!dimHasY(curDim, b) || tail - head >= 64) return;
        q[tail % 64] = {a, b, c};
        tail++;
    };
    checkPortalsAround(x, y, z);
    for (int f = 0; f < 6; f++) push(x + FACE_DX[f], y + FACE_DY[f], z + FACE_DZ[f]);
    push(x, y, z);
    int budget = 256;
    while (head < tail && budget-- > 0) {
        P p = q[head % 64];
        head++;
        uint16_t st = blockAt(p.x, p.y, p.z);
        if (stateIsAir(st) || !world.isResident(curDim, p.x >> 4, p.z >> 4)) continue;
        uint16_t id = blockIdOf(st);
        const BlockDef& b = BLOCKS[id];
        if (stateIsFluid(st) || getProp(st, "waterlogged") == 0) {
            scheduleTick(p.x, p.y, p.z, fluidDelay(id));
        } else {
            // adjacent fluids may now flow into the changed position
            for (int f = 0; f < 6; f++) {
                uint16_t n = blockAt(p.x + FACE_DX[f], p.y + FACE_DY[f], p.z + FACE_DZ[f]);
                if (stateIsFluid(n)) {
                    scheduleTick(p.x + FACE_DX[f], p.y + FACE_DY[f], p.z + FACE_DZ[f], fluidDelay(blockIdOf(n)));
                }
            }
        }
        if (!canSupport(st, p.x, p.y, p.z)) {
            breakBlock(p.x, p.y, p.z, nullptr, true);
            for (int f = 0; f < 6; f++) push(p.x + FACE_DX[f], p.y + FACE_DY[f], p.z + FACE_DZ[f]);
            continue;
        }
        if (b.flags & BF_GRAVITY) {
            uint16_t below = blockAt(p.x, p.y - 1, p.z);
            if (p.y > 0 && (stateIsAir(below) || isReplaceable(below) || stateIsFluid(below))) {
                Entity* fe = spawnEntity(EK_FALLING_BLOCK, ent::FallingBlock, p.x + 0.5, p.y, p.z + 0.5);
                if (fe) {
                    fe->blockState = st;
                    world.setBlock(curDim, p.x, p.y, p.z, 0);
                    for (int f = 0; f < 6; f++) push(p.x + FACE_DX[f], p.y + FACE_DY[f], p.z + FACE_DZ[f]);
                }
                continue;
            }
        }
        uint16_t shaped = computeShape(*this, st, p.x, p.y, p.z);
        if (shaped != st) world.setBlock(curDim, p.x, p.y, p.z, shaped);
    }
}

void Server::setBlock(int x, int y, int z, uint16_t state) {
    if (!dimHasY(curDim, y)) return;
    uint16_t old = world.setBlock(curDim, x, y, z, state);
    if (old != state) updateNeighbors(x, y, z);
}

// ====================================================================== placing
uint16_t Server::placementState(Player& p, uint16_t block, int x, int y, int z, int face, float cx, float cy, float cz) {
    const BlockDef& b = BLOCKS[block];
    const char* n = b.name;
    uint16_t st = b.defState;
    if (block == blk::RedstoneLamp) return setBool(st, "lit", redstone.bestSignal(*this, x, y, z) > 0);
    // a copper bulb placed powered turns on (CopperBulbBlock#onPlace)
    if (endsWith(n, "copper_bulb") && redstone.bestSignal(*this, x, y, z) > 0)
        return setBool(setBool(st, "lit", true), "powered", true);
    if (block == blk::LightningRod) return setPropStr(st, "facing", FACE_NAME[face]);
    if (block == blk::Crafter) {   // CrafterBlock#getStateForPlacement: front toward the player
        int front = oppositeFace(faceIndexOf(lookDirection(p)));
        char o[24];
        if (front == 0) snprintf(o, sizeof(o), "down_%s", playerFacingOpposite(p));
        else if (front == 1) snprintf(o, sizeof(o), "up_%s", playerFacing(p));
        else snprintf(o, sizeof(o), "%s_up", FACE_NAME[front]);
        return setPropStr(st, "orientation", o);
    }
    if (block == blk::RedstoneWire) {
        for (int f = 2; f < 6; ++f) st = setPropStr(st, FACE_NAME[f], "side");
        return redstone.wireShape(*this, x, y, z, st);
    }
    // wall variants
    if (block == blk::Torch || block == blk::SoulTorch || block == blk::RedstoneTorch) {
        if (face == 0) return 0;
        if (face >= 2) {
            uint16_t wall = block == blk::Torch ? blk::WallTorch : (block == blk::SoulTorch ? blk::SoulWallTorch : blk::RedstoneWallTorch);
            return setPropStr(BLOCKS[wall].defState, "facing", FACE_NAME[face]);
        }
        return st;
    }
    if (endsWith(n, "_sign") || endsWith(n, "_banner")) {
        if (face == 0) return 0;
        if (face >= 2) {
            char wname[48];
            const char* suffix = endsWith(n, "_sign") ? "_sign" : "_banner";
            size_t pre = strlen(n) - strlen(suffix);
            snprintf(wname, sizeof(wname), "%.*s_wall%s", (int)pre, n, suffix);
            int w = findBlock(wname);
            if (w < 0) return 0;
            return setPropStr(BLOCKS[w].defState, "facing", FACE_NAME[face]);
        }
        int rot = (int)floorf((p.e.yaw + 180.0f) * 16.0f / 360.0f + 0.5f) & 15;
        return setProp(st, "rotation", rot);
    }
    if (block == blk::Ladder) {
        if (face < 2) return 0;
        return setPropStr(st, "facing", FACE_NAME[face]);
    }
    if (strstr(n, "_button") || block == blk::Lever) {
        if (face == 1) return setPropStr(setPropStr(st, "face", "floor"), "facing", playerFacing(p));
        if (face == 0) return setPropStr(setPropStr(st, "face", "ceiling"), "facing", playerFacing(p));
        return setPropStr(setPropStr(st, "face", "wall"), "facing", FACE_NAME[face]);
    }
    if (endsWith(n, "_slab")) {
        bool top = face == 0 || (face != 1 && cy > 0.5f);
        return setPropStr(st, "type", top ? "top" : "bottom");
    }
    if (endsWith(n, "_stairs")) {
        st = setPropStr(st, "facing", playerFacing(p));
        bool top = face == 0 || (face != 1 && cy > 0.5f);
        return setPropStr(st, "half", top ? "top" : "bottom");
    }
    if (endsWith(n, "_trapdoor")) {
        if (face >= 2) st = setPropStr(st, "facing", FACE_NAME[face]);
        else st = setPropStr(st, "facing", playerFacingOpposite(p));
        bool top = face == 0 || (face != 1 && cy > 0.5f);
        return setPropStr(st, "half", top ? "top" : "bottom");
    }
    if (endsWith(n, "_door") || endsWith(n, "fence_gate") || endsWith(n, "_bed") || block == blk::Bell)
        return setPropStr(st, "facing", playerFacing(p));
    if (block == blk::Lantern || block == blk::SoulLantern) return setBool(st, "hanging", face == 0);
    if (block == blk::Hopper) return setPropStr(st, "facing", face == 1 ? "down" : FACE_NAME[oppositeFace(face)]);
    if (block == blk::Piston || block == blk::StickyPiston || block == blk::Dispenser || block == blk::Dropper ||
        block == blk::CommandBlock || block == blk::Barrel) {
        const char* d = lookDirection(p);
        return setPropStr(st, "facing", FACE_NAME[oppositeFace(faceIndexOf(d))]);
    }
    if (block == blk::Observer) return setPropStr(st, "facing", lookDirection(p));
    if (strstr(n, "anvil")) return setPropStr(st, "facing", HFACING[(yawQuadrant(p.e.yaw) + 1) & 3]);
    if (block == blk::Chest || block == blk::TrappedChest) {
        st = setPropStr(st, "facing", playerFacingOpposite(p));
        // join an adjacent single chest facing the same way (vanilla ChestBlock.getStateForPlacement)
        if (!(p.e.flags & EF_CROUCHING)) {
            int f = faceIndexOf(getPropStr(st, "facing"));
            int cx, cz;
            clockwiseVec(f, cx, cz);
            for (int side = 1; side >= -1; side -= 2) {
                int ox = x + cx * side, oz = z + cz * side;
                uint16_t o = blockAt(ox, y, oz);
                if (blockIdOf(o) == block && !strcmp(getPropStr(o, "type"), "single") &&
                    !strcmp(getPropStr(o, "facing"), FACE_NAME[f])) {
                    // partner clockwise -> we are LEFT and it becomes RIGHT
                    st = setPropStr(st, "type", side > 0 ? "left" : "right");
                    world.setBlock(curDim, ox, y, oz, setPropStr(o, "type", side > 0 ? "right" : "left"));
                    break;
                }
            }
        }
        return st;
    }
    // generic properties
    int fi = propIndexOf(block, "facing");
    if (fi >= 0) {
        const PropDef& pd = PROPS[BLOCK_PROPS[b.propStart + fi]];
        if (pd.n == 6) st = setPropStr(st, "facing", FACE_NAME[oppositeFace(faceIndexOf(lookDirection(p)))]);
        else st = setPropStr(st, "facing", playerFacingOpposite(p));
    }
    if (propIndexOf(block, "axis") >= 0) st = setPropStr(st, "axis", face < 2 ? "y" : (face < 4 ? "z" : "x"));
    if (propIndexOf(block, "rotation") >= 0) st = setProp(st, "rotation", (int)floorf((p.e.yaw + 180.0f) * 16.0f / 360.0f + 0.5f) & 15);
    if (propIndexOf(block, "persistent") >= 0) st = setBool(st, "persistent", true);  // player-placed leaves
    return st;
}

static uint16_t blockForItem(uint16_t item) {
    switch (item) {
        case itm::WheatSeeds: return blk::Wheat;
        case itm::Carrot: return blk::Carrots;
        case itm::Potato: return blk::Potatoes;
        case itm::BeetrootSeeds: return blk::Beetroots;
        case itm::MelonSeeds: return blk::MelonStem;
        case itm::PumpkinSeeds: return blk::PumpkinStem;
        case itm::Redstone: return blk::RedstoneWire;
        case itm::String: return blk::Tripwire;
        case itm::SweetBerries: return blk::SweetBerryBush;
        case itm::CocoaBeans: return blk::Cocoa;
        default: break;
    }
    if (item < NUM_ITEMS && ITEMS[item].block != 0xFFFF) return ITEMS[item].block;
    return 0xFFFF;
}

static bool entityBlocks(Server& s, int x, int y, int z) {
    auto hit = [&](const Entity& e) {
        double hw = e.width / 2;
        return e.x + hw > x && e.x - hw < x + 1 && e.y + e.height > y && e.y < y + 1 && e.z + hw > z && e.z - hw < z + 1;
    };
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& p = s.players[i];
        if (p.inPlay() && !p.dead && p.gamemode != GM_SPECTATOR && p.e.dim == s.curDim && hit(p.e)) return true;
    }
    for (int i = 0; i < MC_MAX_ENTITIES; i++)
        if (s.entities[i].kind == EK_MOB && !s.entities[i].removed && s.entities[i].dim == s.curDim && hit(s.entities[i]))
            return true;
    return false;
}

static void resync(Player& p, int x, int y, int z) {
    Packet pk(pkt::s2c::BlockChange);
    pk.w.u64(packPos(x, y, z));
    pk.w.varint(p.srv->blockAt(x, y, z));
    p.conn.send(pk);
}

// A refused placement: the client predicted a block next to the clicked one.
static void resyncPlace(Player& p, int x, int y, int z, int face) {
    resync(p, x, y, z);
    resync(p, x + FACE_DX[face], y + FACE_DY[face], z + FACE_DZ[face]);
    p.sendSlot(SLOT_HOTBAR_START + p.held);
}

void Player::onPlace(Reader& r) {
    int hand = r.varint();
    int x, y, z;
    unpackPos(r.u64(), x, y, z);
    int face = r.varint();
    float cx = r.f32(), cy = r.f32(), cz = r.f32();
    r.boolean();   // inside the block
    r.boolean();   // the world border was hit
    lastSequence = r.varint();
    if (!r.ok() || face < 0 || face > 5) return;
    Server& s = *srv;
    if (dead || gamemode == GM_SPECTATOR) return;
    double dx = x + 0.5 - e.x, dy = y + 0.5 - (e.y + 1.62), dz = z + 0.5 - e.z;
    if (dx * dx + dy * dy + dz * dz > 8 * 8 || !s.world.isWritable(s.curDim, x, z)) { resyncPlace(*this, x, y, z, face); return; }
    uint16_t clicked = s.blockAt(x, y, z);
    int slotIdx = hand == 1 ? SLOT_OFFHAND : SLOT_HOTBAR_START + held;
    ItemStack& it = inv[slotIdx];

    // 1) use the clicked block (unless sneaking with an item)
    clickFace = (int8_t)face;
    clickX = cx; clickY = cy; clickZ = cz;
    if (!((e.flags & EF_CROUCHING) && !it.empty())) {
        bool handled = false;
        s.interactBlock(*this, x, y, z, clicked, handled);
        if (handled) return;
    }
    if (it.empty() || gamemode == GM_ADVENTURE) return;

    // 2) tools used on blocks
    uint16_t cid = blockIdOf(clicked);
    if (cid == blk::Lectern && Books::place(s, *this, x, y, z, it)) {
        sendSlot(slotIdx); s.broadcastEquipment(*this); return;
    }
    const ItemDef& idef = ITEMS[it.id];
    if (idef.kind == IK_HOE && face != 0 && (cid == blk::GrassBlock || cid == blk::Dirt || cid == blk::DirtPath) &&
        stateIsAir(s.blockAt(x, y + 1, z))) {
        s.setBlock(x, y, z, bs::Farmland);
        s.playSound("item.hoe.till", x + 0.5, y + 0.5, z + 0.5, 1, 1, 4);
        if (isSurvivalLike()) s.damageHeldItem(*this, 1);
        return;
    }
    if (idef.kind == IK_SHOVEL && face != 0 && cid == blk::GrassBlock && stateIsAir(s.blockAt(x, y + 1, z))) {
        s.setBlock(x, y, z, bs::DirtPath);
        s.playSound("item.shovel.flatten", x + 0.5, y + 0.5, z + 0.5, 1, 1, 4);
        if (isSurvivalLike()) s.damageHeldItem(*this, 1);
        return;
    }
    if (idef.kind == IK_AXE) {
        // strip logs
        const char* cn = BLOCKS[cid].name;
        if ((endsWith(cn, "_log") || endsWith(cn, "_wood") || endsWith(cn, "_stem") || endsWith(cn, "_hyphae")) &&
            !strstr(cn, "stripped")) {
            char sname[48];
            snprintf(sname, sizeof(sname), "stripped_%s", cn);
            int sb = findBlock(sname);
            if (sb >= 0) {
                uint16_t ns = setPropStr(BLOCKS[sb].defState, "axis", getPropStr(clicked, "axis"));
                s.setBlock(x, y, z, ns);
                s.playSound("item.axe.strip", x + 0.5, y + 0.5, z + 0.5, 1, 1, 4);
                if (isSurvivalLike()) s.damageHeldItem(*this, 1);
                return;
            }
        }
    }
    if (it.id == itm::BoneMeal) {
        // grow crops / saplings instantly
        int age = getProp(clicked, "age");
        if (age >= 0 && (cid == blk::Wheat || cid == blk::Carrots || cid == blk::Potatoes || cid == blk::Beetroots)) {
            int maxAge = cid == blk::Beetroots ? 3 : 7;
            int na = age + 2 + (int)(plat::random32() % 4);
            s.setBlock(x, y, z, setProp(clicked, "age", na > maxAge ? maxAge : na));
            if (isSurvivalLike()) s.consumeHeld(*this);
            return;
        }
        if (strstr(BLOCKS[cid].name, "_sapling")) {
            s.randomTickBlock(x, y, z, setProp(clicked, "stage", 1));
            if (isSurvivalLike()) s.consumeHeld(*this);
            return;
        }
    }
    // buckets
    if (it.id == itm::Bucket || it.id == itm::WaterBucket || it.id == itm::LavaBucket) {
        if (it.id == itm::Bucket) {
            // pick up a source block: the clicked block or the one in front of the face
            int px = x, py = y, pz = z;
            uint16_t src = clicked;
            if (!isFluidSource(src, blk::Water) && !isFluidSource(src, blk::Lava)) {
                px += FACE_DX[face]; py += FACE_DY[face]; pz += FACE_DZ[face];
                src = s.blockAt(px, py, pz);
            }
            uint16_t filled = isFluidSource(src, blk::Water) ? itm::WaterBucket : (isFluidSource(src, blk::Lava) ? itm::LavaBucket : 0);
            if (!filled) return;
            s.setBlock(px, py, pz, 0);
            if (gamemode != GM_CREATIVE) {
                s.consumeHeld(*this);
                s.giveItem(*this, ItemStack::of(filled));
            }
            s.playSound(filled == itm::WaterBucket ? "item.bucket.fill" : "item.bucket.fill_lava", px, py, pz, 1, 1, 7);
            return;
        }
        int px = x, py = y, pz = z;
        if (!isReplaceable(clicked)) { px += FACE_DX[face]; py += FACE_DY[face]; pz += FACE_DZ[face]; }
        uint16_t at = s.blockAt(px, py, pz);
        if (!s.world.blockInBounds(px, pz) || !dimHasY(s.curDim, py)) return;
        if (it.id == itm::WaterBucket && s.curDim == DIM_NETHER) {   // evaporates
            s.playSound("block.fire.extinguish", px + 0.5, py + 0.5, pz + 0.5, 0.5f, 2.6f, 7);
        } else if (it.id == itm::WaterBucket && getProp(at, "waterlogged") == 1) {
            s.setBlock(px, py, pz, setBool(at, "waterlogged", true));
        } else if (isReplaceable(at)) {
            if (!stateIsAir(at) && !stateIsFluid(at)) s.breakBlock(px, py, pz, nullptr, true);
            s.setBlock(px, py, pz, it.id == itm::WaterBucket ? bs::Water : bs::Lava);
        } else {
            resync(*this, px, py, pz);
            return;
        }
        s.playSound(it.id == itm::WaterBucket ? "item.bucket.empty" : "item.bucket.empty_lava", px, py, pz, 1, 1, 7);
        if (gamemode != GM_CREATIVE) {
            it = ItemStack::of(itm::Bucket);
            sendSlot(slotIdx);
        }
        return;
    }
    if (it.id == itm::FlintAndSteel || (it.id == itm::FireCharge && cid == blk::Tnt)) {
        int px = x + FACE_DX[face], py = y + FACE_DY[face], pz = z + FACE_DZ[face];
        if (cid == blk::Tnt) {
            s.primeTnt(x, y, z, e.id);
        } else if (stateIsAir(s.blockAt(px, py, pz)) && s.lightPortal(px, py, pz)) {
            // the fire lit an obsidian frame
        } else if (stateIsAir(s.blockAt(px, py, pz)) && stateCollides(s.blockAt(px, py - 1, pz))) {
            s.setBlock(px, py, pz, bs::Fire);
            s.scheduleTick(px, py, pz, 200);  // burns out
        }
        s.playSound("item.flintandsteel.use", x + 0.5, y + 0.5, z + 0.5, 1, 1, 7);
        if (isSurvivalLike()) {
            if (it.id == itm::FireCharge) s.consumeHeld(*this);
            else s.damageHeldItem(*this, 1);
        }
        return;
    }
    // spawn eggs
    {
        const char* in = ITEMS[it.id].name;
        if (endsWith(in, "_spawn_egg")) {
            char ename[40];
            snprintf(ename, sizeof(ename), "%.*s", (int)(strlen(in) - 10), in);
            int et = findEntityType(ename);
            if (et >= 0 && isMobType(et)) {
                int px = x + FACE_DX[face], py = y + FACE_DY[face], pz = z + FACE_DZ[face];
                Entity* m = s.spawnMob((uint16_t)et, px + 0.5, py, pz + 0.5);
                if (m && gamemode != GM_CREATIVE) s.consumeHeld(*this);
            }
            return;
        }
    }

    // 3) place a block
    uint16_t block = blockForItem(it.id);
    if (block == 0xFFFF) return;
    int px = x, py = y, pz = z;
    // slab merging: clicking the matching side of a single slab
    if (endsWith(BLOCKS[block].name, "_slab") && cid == block) {
        const char* type = getPropStr(clicked, "type");
        if ((!strcmp(type, "bottom") && face == 1) || (!strcmp(type, "top") && face == 0)) {
            s.setBlock(x, y, z, setPropStr(clicked, "type", "double"));
            s.playSound("block.stone.place", x + 0.5, y + 0.5, z + 0.5, 1, 0.8f, 4);
            if (gamemode != GM_CREATIVE) s.consumeHeld(*this);
            return;
        }
    }
    // snow layers stack
    if (block == blk::Snow && cid == blk::Snow) {
        int layers = getProp(clicked, "layers");
        if (layers < 7) {
            s.setBlock(x, y, z, setProp(clicked, "layers", layers + 1));
            if (gamemode != GM_CREATIVE) s.consumeHeld(*this);
            return;
        }
    }
    if (!isReplaceable(clicked)) { px += FACE_DX[face]; py += FACE_DY[face]; pz += FACE_DZ[face]; }
    uint16_t at = s.blockAt(px, py, pz);
    if (!dimHasY(s.curDim, py) || !s.world.blockInBounds(px, pz) || !isReplaceable(at)) { resync(*this, px, py, pz); return; }
    // a slab into a slab space of the same kind
    if (endsWith(BLOCKS[block].name, "_slab") && blockIdOf(at) == block) {
        s.setBlock(px, py, pz, setPropStr(at, "type", "double"));
        if (gamemode != GM_CREATIVE) s.consumeHeld(*this);
        return;
    }
    float fcx = cx, fcy = cy, fcz = cz;
    if (px != x || py != y || pz != z) { /* cursor relative to clicked block: keep */ }
    uint16_t st = s.placementState(*this, block, px, py, pz, face, fcx, fcy, fcz);
    if (!st) { resync(*this, px, py, pz); return; }
    if (stateCollides(st) && entityBlocks(s, px, py, pz)) { resync(*this, px, py, pz); return; }
    if (!s.canSupport(st, px, py, pz)) { resync(*this, px, py, pz); return; }
    // waterlog into water sources
    if (isFluidSource(at, blk::Water) && propIndexOf(blockIdOf(st), "waterlogged") >= 0) st = setBool(st, "waterlogged", true);
    // two-block structures need the second space
    const char* bnm = BLOCKS[block].name;
    bool tall = endsWith(bnm, "_door") || block == blk::Sunflower || block == blk::Lilac || block == blk::RoseBush ||
                block == blk::Peony || block == blk::TallGrass || block == blk::LargeFern;
    if (tall) {
        uint16_t up = s.blockAt(px, py + 1, pz);
        if (py >= 255 || !isReplaceable(up)) { resync(*this, px, py, pz); return; }
    }
    int bedX = px, bedZ = pz;
    if (endsWith(bnm, "_bed")) {
        int f = faceIndexOf(getPropStr(st, "facing"));
        bedX = px + FACE_DX[f];
        bedZ = pz + FACE_DZ[f];
        if (!isReplaceable(s.blockAt(bedX, py, bedZ)) || !stateCollides(s.blockAt(bedX, py - 1, bedZ))) {
            resync(*this, px, py, pz);
            return;
        }
    }
    if (!stateIsAir(at) && !stateIsFluid(at)) s.world.setBlock(s.curDim, px, py, pz, 0);
    s.setBlock(px, py, pz, st);
    if (tall) {
        uint16_t upper = setPropStr(st, "half", "upper");
        s.setBlock(px, py + 1, pz, upper);
    }
    if (endsWith(bnm, "_bed")) s.setBlock(bedX, py, bedZ, setPropStr(st, "part", "head"));
    // block entities
    Chunk* c = s.world.get(s.curDim, px >> 4, pz >> 4);
    if (c) {
        if (block == blk::Chest || block == blk::TrappedChest || block == blk::Barrel) c->addTile(block == blk::Barrel ? TILE_BARREL : TILE_CHEST, px & 15, py, pz & 15);
        else if (block == blk::Furnace || block == blk::BlastFurnace || block == blk::Smoker) c->addTile(TILE_FURNACE, px & 15, py, pz & 15);
        else if (strstr(bnm, "_sign")) {
            c->addTile(TILE_SIGN, px & 15, py, pz & 15);
            Packet pk(pkt::s2c::OpenSignEntity);
            pk.w.u64(packPos(px, py, pz));
            pk.w.boolean(true);   // the front
            conn.send(pk);
        }
    }
    // shape of the placed block itself (fences etc.)
    uint16_t shaped = computeShape(s, s.blockAt(px, py, pz), px, py, pz);
    if (shaped != s.blockAt(px, py, pz)) s.world.setBlock(s.curDim, px, py, pz, shaped);
    {
        char snd[64];
        const char* mat = "stone";
        uint8_t tc = BLOCKS[block].toolClass;
        if (tc == TC_AXE) mat = "wood";
        else if (tc == TC_SHOVEL) mat = BLOCKS[block].flags & BF_GRAVITY ? "sand" : "gravel";
        else if (BLOCKS[block].flags & BF_NEEDS_SUPPORT) mat = "grass";
        else if (strstr(bnm, "wool")) mat = "wool";
        else if (strstr(bnm, "glass")) mat = "glass";
        snprintf(snd, sizeof(snd), "block.%s.place", mat);
        s.vibration(px + .5, py + .5, pz + .5, GE_BLOCK_PLACE);
        Packet pk(pkt::s2c::SoundEffect);
        pk.w.varint(0);        // the sound by name
        pk.w.string(snd);
        pk.w.boolean(false);   // no fixed range
        pk.w.varint(4);
        pk.w.i32((int32_t)((px + 0.5) * 8));
        pk.w.i32((int32_t)((py + 0.5) * 8));
        pk.w.i32((int32_t)((pz + 0.5) * 8));
        pk.w.f32(1);
        pk.w.f32(0.8f);
        pk.w.i64((int64_t)plat::random32());   // seed
        s.broadcastNear(pk, px >> 4, pz >> 4, this);  // the placing client plays it itself
    }
    if (gamemode != GM_CREATIVE) {
        if (--it.count == 0) it.clear();
        sendSlot(slotIdx);
        if (slotIdx == SLOT_HOTBAR_START + held) s.broadcastEquipment(*this);
    }
}

// ====================================================================== interacting
void Server::interactBlock(Player& p, int x, int y, int z, uint16_t st, bool& handled) {
    uint16_t id = blockIdOf(st);
    const char* n = BLOCKS[id].name;
    handled = true;
    if (id == blk::Lectern) {
        if (getBool(st, "has_book")) openLectern(p, x, y, z);
        else handled = false;
        return;
    }
    if (id == blk::DaylightDetector) {
        if (p.gamemode == GM_SPECTATOR || p.gamemode == GM_ADVENTURE) return;
        st = setBool(st, "inverted", !getBool(st, "inverted"));
        world.setBlock(curDim, x, y, z, st, true, 4);
        redstone.daylightDetector(*this, x, y, z, st);
        return;
    }
    if (id == blk::Hopper || id == blk::Dropper || id == blk::Dispenser || id == blk::Crafter) { openContainer(p,x,y,z); return; }
    if (id == blk::ChiseledBookshelf) { handled = useBookshelf(p, x, y, z, st); return; }
    if (id == blk::Chest || id == blk::TrappedChest || id == blk::Barrel) {
        if (id != blk::Barrel && fullSolid(blockAt(x, y + 1, z))) return;  // blocked lid
        openContainer(p, x, y, z);
        return;
    }
    if (id == blk::CraftingTable) { openCrafting(p, x, y, z); return; }
    if (id == blk::Furnace || id == blk::BlastFurnace || id == blk::Smoker) { openFurnace(p, x, y, z); return; }
    if ((endsWith(n, "_door") || endsWith(n, "_trapdoor") || endsWith(n, "fence_gate")) && id != blk::IronDoor &&
        id != blk::IronTrapdoor) {
        bool open = !getBool(st, "open");
        uint16_t ns = setBool(st, "open", open);
        if (endsWith(n, "fence_gate") && open) {
            // gates open away from the player
            const char* pf = playerFacing(p);
            const char* gf = getPropStr(st, "facing");
            if (faceIndexOf(pf) == oppositeFace(faceIndexOf(gf))) ns = setPropStr(ns, "facing", pf);
        }
        world.setBlock(curDim, x, y, z, ns);
        if (endsWith(n, "_door")) {
            const char* half = getPropStr(st, "half");
            int oy = !strcmp(half, "lower") ? y + 1 : y - 1;
            uint16_t o = blockAt(x, oy, z);
            if (blockIdOf(o) == id) world.setBlock(curDim, x, oy, z, setBool(o, "open", open));
        }
        char snd[64];
        bool wood = id != blk::IronDoor;
        const char* kind = strstr(n, "copper") ? "copper_" : wood ? "wooden_" : "iron_";   // copper: 1.21
        snprintf(snd, sizeof(snd), "block.%s%s.%s", kind,
                 endsWith(n, "_door") ? "door" : (endsWith(n, "_trapdoor") ? "trapdoor" : "door"), open ? "open" : "close");
        if (endsWith(n, "fence_gate")) snprintf(snd, sizeof(snd), "block.fence_gate.%s", open ? "open" : "close");
        playSound(snd, x + 0.5, y + 0.5, z + 0.5, 1, 1, 4);
        vibration(x + .5, y + .5, z + .5, open ? GE_OPEN : GE_CLOSE);   // block_open / block_close
        return;
    }
    if (id == blk::RedstoneWire) {
        bool dot = true, cross = true;
        for (int f = 2; f < 6; ++f) {
            bool connected = strcmp(getPropStr(st, FACE_NAME[f]), "none") != 0;
            dot &= !connected; cross &= connected;
        }
        if (dot || cross) {
            for (int f = 2; f < 6; ++f) st = setPropStr(st, FACE_NAME[f], dot ? "side" : "none");
            world.setBlock(curDim, x, y, z, redstone.wireShape(*this, x, y, z, st));
        }
        return;
    }
    if (id == blk::Lever) {
        world.setBlock(curDim, x, y, z, setBool(st, "powered", !getBool(st, "powered")));
        redstone.switchOutputChanged(*this, x, y, z, st);
        playSound("block.lever.click", x + 0.5, y + 0.5, z + 0.5, 0.3f, getBool(st, "powered") ? 0.5f : 0.6f, 4);
        return;
    }
    if (endsWith(n, "_button")) {
        if (!getBool(st, "powered")) {
            world.setBlock(curDim, x, y, z, setBool(st, "powered", true));
            redstone.switchOutputChanged(*this, x, y, z, st);
            scheduleTick(x, y, z, strstr(n, "stone") ? 20 : 30);
            playSound(strstr(n, "stone") ? "block.stone_button.click_on" : "block.wooden_button.click_on", x + 0.5, y + 0.5,
                      z + 0.5, 0.3f, 0.6f, 4);
        }
        return;
    }
    if (id == blk::NoteBlock) {
        int note = (getProp(st, "note") + 1) % 25;
        world.setBlock(curDim, x, y, z, setProp(st, "note", note));
        redstone.playNote(*this, x, y, z);
        return;
    }
    if (id == blk::Repeater) {
        world.setBlock(curDim, x, y, z, setProp(st, "delay", (getProp(st, "delay") + 1) % 4));
        return;
    }
    if (id == blk::Comparator) {
        const char* m = getPropStr(st, "mode");
        world.setBlock(curDim, x, y, z, setPropStr(st, "mode", !strcmp(m, "compare") ? "subtract" : "compare"));
        TimerEvent ev; ev.key = TimerKey::block(x, y, z, id, curDim);
        redstone.tick(*this, ev); // mode changes refresh immediately in 1.16.5
        return;
    }
    if (id == blk::Lectern) {
        if (getBool(st, "has_book")) openLectern(p, x, y, z);
        else handled = false;
        return;
    }
    if (id == blk::DaylightDetector) {
        world.setBlock(curDim, x, y, z, setBool(st, "inverted", !getBool(st, "inverted")));
        return;
    }
    if (id == blk::Cake) {
        if (p.food >= 20 && p.gamemode != GM_CREATIVE) { handled = false; return; }
        p.food = p.food + 2 > 20 ? 20 : p.food + 2;
        p.saturation += 0.4f;
        p.healthDirty = true;
        int bites = getProp(st, "bites") + 1;
        if (bites > 6) setBlock(x, y, z, 0);
        else world.setBlock(curDim, x, y, z, setProp(st, "bites", bites));
        return;
    }
    if (endsWith(n, "_bed")) {
        const char* part = getPropStr(st, "part");
        int bx = x, bz = z;
        if (!strcmp(part, "foot")) {
            int f = faceIndexOf(getPropStr(st, "facing"));
            bx += FACE_DX[f];
            bz += FACE_DZ[f];
        }
        if (curDim != DIM_OVERWORLD) {   // as in vanilla: beds explode outside the overworld
            setBlock(x, y, z, 0);
            setBlock(bx, y, bz, 0);
            explode(x + 0.5, y + 0.5, z + 0.5, 5.0f, -1);
            return;
        }
        p.hasSpawn = true;
        p.spawnX = bx; p.spawnY = y; p.spawnZ = bz;
        int64_t t = meta.timeOfDay % 24000;
        if (t >= 12542 && t <= 23459) {
            meta.timeOfDay += 24000 - t;  // skip to the next morning
            broadcastSystem("Sleeping through this night", "gray");
            for (int i = 0; i < MC_MAX_PLAYERS; i++)
                if (players[i].inPlay()) players[i].sendTime();
            if (meta.raining) { meta.weatherTimer = 1; }
        } else {
            p.sendActionBar("Respawn point set. You can only sleep at night");
        }
        return;
    }
    handled = false;
}

void Player::onUpdateSign(Reader& r) {
    int x, y, z;
    unpackPos(r.u64(), x, y, z);
    bool front = r.boolean();
    if (!front) return;   // only the front text is kept
    char lines[4][64];
    for (int i = 0; i < 4; i++) r.string(lines[i], sizeof(lines[i]));
    if (!r.ok()) return;
    Chunk* c = srv->world.get(e.dim, x >> 4, z >> 4);
    if (!c || !strstr(bname(c->get(x & 15, y, z & 15)), "_sign")) return;
    double dx = x - e.x, dz = z - e.z;
    if (dx * dx + dz * dz > 100) return;
    TileEntity* t = c->tileAt(x & 15, y, z & 15);
    if (!t) t = c->addTile(TILE_SIGN, x & 15, y, z & 15);
    for (int i = 0; i < 4; i++) {
        // strip control characters
        char* d = t->text[i];
        for (const char* s = lines[i]; *s; s++)
            if ((unsigned char)*s >= 0x20) *d++ = *s;
        *d = 0;
    }
    c->dirty = true;
    c->version++;  // a chunk send being prepared from an older snapshot must be redone
    // broadcast the new sign contents
    Packet pk(pkt::s2c::TileEntityData);
    pk.w.u64(packPos(x, y, z));
    pk.w.varint(bet::Sign);
    writeSignNbt(pk.w, *t);
    srv->broadcastNear(pk, x >> 4, z >> 4);
}

// ====================================================================== scheduled & random ticks
// A newly scheduled tick must survive even when no block state changed.
void Server::scheduleTick(int x, int y, int z, int delay, int8_t prio) {
    if (!dimHasY(curDim, y)) return;
    uint16_t id = blockIdOf(world.getBlock(curDim, x, y, z));
    if (timers.schedule(TimerKey::block(x, y, z, id, curDim), worldTick() + (uint32_t)(delay > 0 ? delay : 1), prio))
        world.markDirty(curDim, x >> 4, z >> 4);
}

void Server::runBlockTick(const TimerEvent& ev) {
    int x = ev.key.x, y = ev.key.y, z = ev.key.z;
    if (!world.isResident(curDim, x >> 4, z >> 4)) return;
    uint16_t st = blockAt(x, y, z);
    uint16_t id = blockIdOf(st);
    world.markDirty(curDim, x >> 4, z >> 4); // consuming a saved tick is a persistent change
    if (id != ev.key.data) return;   // replaced meanwhile: vanilla drops the tick too
    if (redstone.tick(*this, ev)) return;
    if (id == blk::Water || id == blk::Lava) tickFluid(x, y, z, st);
    else if (endsWith(BLOCKS[id].name, "_button") && getBool(st, "powered")) {
        world.setBlock(curDim, x, y, z, setBool(st, "powered", false));
        playSound("block.wooden_button.click_off", x + 0.5, y + 0.5, z + 0.5, 0.3f, 0.5f, 4);
    } else if (id == blk::Fire) {
        setBlock(x, y, z, 0);
    }
}

// The timer wheel's events for this tick: block ticks, furnaces, mob timers. The wheel
// runs in step with the world age, which only this tick advances.
void Server::runTimers() {
    // normally exactly one step; if the world age ever moved on without us, catch up a
    // few ticks at a time rather than drop anything
    for (int step = 0; step < 20 && (int32_t)(worldTick() - timers.now()) >= 0; step++) runTimerStep();
}

void Server::runTimerStep() {
    timerCount_ = timers.advance(timerOut_, MC_SCHED_TICKS);
    for (timerIndex_ = 0; timerIndex_ < timerCount_; ++timerIndex_) {
        const TimerEvent& ev = timerOut_[timerIndex_];
        switch (ev.key.kind) {
            case TK_BLOCK: {
                InDim in(*this, ev.key.dim);
                runBlockTick(ev);
                break;
            }
            case TK_FURNACE: {
                InDim in(*this, ev.key.dim);
                if (world.isResident(curDim, ev.key.x >> 4, ev.key.z >> 4)) updateFurnace(ev.key.x, ev.key.y, ev.key.z, true);
                break;
            }
            case TK_ENTITY: runEntityTimer(ev); break;
            default: break;
        }
    }
}

bool Server::willTickThisTick(int x, int y, int z, uint16_t id) const {
    TimerKey key = TimerKey::block(x, y, z, id, curDim);
    for (int i = timerIndex_ + 1; i < timerCount_; ++i) if (timerOut_[i].key == key) return true;
    return false;
}

static int fluidLevel(uint16_t st, uint16_t fluid) {
    if (blockIdOf(st) != fluid) return -1;
    return getProp(st, "level");
}

static bool fluidCanReplace(uint16_t st) {
    if (stateIsAir(st)) return true;
    const BlockDef& b = blockOf(st);
    if (b.flags & BF_FLUID) return false;
    return (b.flags & (BF_REPLACEABLE | BF_NEEDS_SUPPORT)) != 0 || (!stateCollides(st) && !strstr(b.name, "_sign") &&
                                                                    !strstr(b.name, "_door") && b.filterLight < 15 &&
                                                                    !strstr(b.name, "ladder") && !strstr(b.name, "rail"));
}

int Server::fluidDelay(uint16_t blockId) const {
    return blockId == blk::Lava ? (curDim == DIM_NETHER ? 10 : 30) : 5;
}

void Server::tickFluid(int x, int y, int z, uint16_t st) {
    uint16_t fluid = blockIdOf(st);
    bool lava = fluid == blk::Lava;
    int drop = lava && curDim != DIM_NETHER ? 2 : 1;   // lava flows as far as water in the Nether
    int delay = fluidDelay(fluid);
    int level = getProp(st, "level");
    uint16_t base = lava ? bs::Lava : bs::Water;
    // lava touching water hardens
    if (lava) {
        for (int f = 1; f < 6; f++) {
            if (blockIdOf(blockAt(x + FACE_DX[f], y + FACE_DY[f], z + FACE_DZ[f])) == blk::Water) {
                setBlock(x, y, z, level == 0 ? bs::Obsidian : bs::Cobblestone);
                playSound("block.lava.extinguish", x + 0.5, y + 0.5, z + 0.5, 0.5f, 2.6f, 4);
                return;
            }
        }
    }
    if (level != 0) {
        // recompute strength from neighbours
        // vanilla FlowingFluid#getNewLiquid: falling if the same fluid is above, otherwise
        // the strongest horizontal neighbour minus the drop-off (sources count as 0).
        // Level 8 is reserved for falling fluid: horizontal flow never produces it.
        int best = 99;
        int sources = 0;
        bool falling = blockIdOf(blockAt(x, y + 1, z)) == fluid;
        for (int f = 2; f < 6 && !falling; f++) {
            int nl = fluidLevel(blockAt(x + FACE_DX[f], y, z + FACE_DZ[f]), fluid);
            if (nl < 0) continue;
            if (nl == 0) sources++;
            int eff = (nl >= 8 ? 0 : nl) + drop;
            if (eff < best) best = eff;
        }
        if (falling) best = 8;
        else if (!lava && sources >= 2) {
            uint16_t below = blockAt(x, y - 1, z);
            if (stateCollides(below) || isFluidSource(below, blk::Water)) best = 0;
        }
        if (!falling && best > 7) {
            setBlock(x, y, z, 0);
            return;
        }
        if (best != level) {
            st = setProp(base, "level", best);
            setBlock(x, y, z, st);  // neighbours fed by this block re-evaluate too
            level = best;
            scheduleTick(x, y, z, delay);
        }
    }
    // flow down
    if (y > 0) {
        uint16_t below = blockAt(x, y - 1, z);
        if (lava && blockIdOf(below) == blk::Water) {
            setBlock(x, y - 1, z, bs::Stone);
            return;
        }
        int bl = fluidLevel(below, fluid);
        if (fluidCanReplace(below) || (bl > 0 && bl < 8)) {
            if (!stateIsAir(below) && bl < 0) breakBlock(x, y - 1, z, nullptr, true);
            setBlock(x, y - 1, z, setProp(base, "level", 8));
            scheduleTick(x, y - 1, z, delay);
        }
        // vanilla FlowingFluid#spread (isWaterHole): flowing fluid that can fall, or that
        // stands on the same fluid, drains down and does not spread sideways; only
        // sources do. Otherwise it spreads across the top of a pool below it.
        if (level != 0 && (bl >= 0 || fluidCanReplace(below))) return;
    }
    int spread = (level >= 8 ? 0 : level) + drop;
    if (spread > 7) return;
    for (int f = 2; f < 6; f++) {
        int nx = x + FACE_DX[f], nz = z + FACE_DZ[f];
        if (!world.isResident(curDim, nx >> 4, nz >> 4)) continue;
        uint16_t n = blockAt(nx, y, nz);
        int nl = fluidLevel(n, fluid);
        // like vanilla (FlowingFluid#canSpreadTo): never overwrite the same fluid sideways;
        // such cells recompute their own level when scheduled by the neighbour update
        if (nl >= 0) continue;
        if (!fluidCanReplace(n)) {
            if (lava && blockIdOf(n) == blk::Water) setBlock(nx, y, nz, bs::Cobblestone);
            continue;
        }
        if (!stateIsAir(n)) breakBlock(nx, y, nz, nullptr, true);
        setBlock(nx, y, nz, setProp(base, "level", spread));
        scheduleTick(nx, y, nz, delay);
    }
}

static void growTree(Server& s, int x, int y, int z, uint16_t sapling) {
    uint16_t log = bs::OakLog, leaves = bs::OakLeaves;
    switch (sapling) {
        case blk::SpruceSapling: log = bs::SpruceLog; leaves = bs::SpruceLeaves; break;
        case blk::BirchSapling: log = bs::BirchLog; leaves = bs::BirchLeaves; break;
        case blk::JungleSapling: log = bs::JungleLog; leaves = bs::JungleLeaves; break;
        case blk::AcaciaSapling: log = bs::AcaciaLog; leaves = bs::AcaciaLeaves; break;
        case blk::DarkOakSapling: log = bs::DarkOakLog; leaves = bs::DarkOakLeaves; break;
        default: break;
    }
    int h = 4 + s_brng.range(3);
    for (int dy = 1; dy <= h + 1; dy++)
        if (!stateIsAir(s.blockAt(x, y + dy, z)) && !strstr(bname(s.blockAt(x, y + dy, z)), "leaves")) return;
    int top = y + h - 1;
    for (int ly = top - 2; ly <= top + 1; ly++) {
        int r = ly >= top ? 1 : 2;
        for (int dx = -r; dx <= r; dx++)
            for (int dz = -r; dz <= r; dz++) {
                if (abs(dx) == r && abs(dz) == r && (ly >= top || s_brng.range(2))) continue;
                if (stateIsAir(s.blockAt(x + dx, ly, z + dz))) s.world.setBlock(s.curDim, x + dx, ly, z + dz, leaves);
            }
    }
    for (int ly = y; ly <= top; ly++) s.world.setBlock(s.curDim, x, ly, z, log);
    s.world.setBlock(s.curDim, x, y - 1, z, bs::Dirt);
}

void Server::randomTickBlock(int x, int y, int z, uint16_t st) {
    uint16_t id = blockIdOf(st);
    switch (id) {
        case blk::Wheat: case blk::Carrots: case blk::Potatoes: case blk::Beetroots: {
            int maxAge = id == blk::Beetroots ? 3 : 7;
            int age = getProp(st, "age");
            if (age < maxAge && s_brng.range(4) == 0) world.setBlock(curDim, x, y, z, setProp(st, "age", age + 1));
            return;
        }
        case blk::SugarCane: case blk::Cactus: {
            if (!stateIsAir(blockAt(x, y + 1, z))) return;
            int h = 1;
            while (h < 3 && blockIdOf(blockAt(x, y - h, z)) == id) h++;
            if (h >= 3) return;
            int age = getProp(st, "age");
            if (age < 15) { world.setBlock(curDim, x, y, z, setProp(st, "age", age + 1)); return; }
            world.setBlock(curDim, x, y, z, setProp(st, "age", 0));
            uint16_t ns = BLOCKS[id].defState;
            if (canSupport(ns, x, y + 1, z) || id == blk::SugarCane) setBlock(x, y + 1, z, ns);
            return;
        }
        case blk::GrassBlock: {
            uint16_t above = blockAt(x, y + 1, z);
            if (stateOpaque(above)) { world.setBlock(curDim, x, y, z, bs::Dirt); return; }
            for (int k = 0; k < 2; k++) {
                int nx = x + s_brng.between(-1, 1), ny = y + s_brng.between(-3, 1), nz = z + s_brng.between(-1, 1);
                // needs daylight: only spread onto dirt that is open to the sky
                if (blockAt(nx, ny, nz) == bs::Dirt && ny + 1 >= world.heightAt(curDim, nx, nz) &&
                    !stateOpaque(blockAt(nx, ny + 1, nz)) && !stateIsFluid(blockAt(nx, ny + 1, nz)))
                    world.setBlock(curDim, nx, ny, nz, bs::GrassBlock);
            }
            return;
        }
        default: break;
    }
    if (strstr(BLOCKS[id].name, "_sapling")) {
        int stage = getProp(st, "stage");
        if (stage == 0) world.setBlock(curDim, x, y, z, setProp(st, "stage", 1));
        else growTree(*this, x, y, z, id);
    }
}

// Blocks that react to random ticks (computed once).
static bool randomTicking(uint16_t id) {
    static uint8_t table[(NUM_BLOCKS + 7) / 8];
    static bool ready = false;
    if (!ready) {
        for (int b = 0; b < NUM_BLOCKS; b++) {
            bool t = b == blk::Wheat || b == blk::Carrots || b == blk::Potatoes || b == blk::Beetroots ||
                     b == blk::SugarCane || b == blk::Cactus || b == blk::GrassBlock || strstr(BLOCKS[b].name, "_sapling");
            if (t) table[b >> 3] |= (uint8_t)(1 << (b & 7));
        }
        ready = true;
    }
    return (table[id >> 3] >> (id & 7)) & 1;
}

void Server::randomTicks() {
    // Every 4th tick, 12 random blocks per non-empty section (same rate as vanilla's 3 per
    // tick) in the chunks around players; overlapping player areas are processed once.
    if (ticks % 4 != 0) return;
    const int R = 3;
    int32_t done[64][3];
    int nDone = 0;
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& p = players[i];
        if (!p.inPlay() || !p.viewReady) continue;
        InDim in(*this, p.e.dim);
        for (int dz = -R; dz <= R; dz++)
            for (int dx = -R; dx <= R; dx++) {
                int cx = p.centerCx + dx, cz = p.centerCz + dz;
                bool seen = false;
                for (int k = 0; k < nDone && !seen; k++) seen = done[k][0] == cx && done[k][1] == cz && done[k][2] == curDim;
                if (seen) continue;
                if (nDone < 64) { done[nDone][0] = cx; done[nDone][1] = cz; done[nDone][2] = curDim; nDone++; }
                Chunk* c = world.get(curDim, cx, cz);
                if (!c) continue;
                for (int s = 0; s < c->numSections(); s++) {
                    Section* sec = c->section(s);
                    if (!sec || sec->nonAirCount() == 0) continue;
                    // only sections that contain something that grows
                    if (!sec->anyState([](uint16_t st) { return randomTicking(blockIdOf(st)); })) continue;
                    for (int k = 0; k < 12; k++) {
                        int idx = (int)(s_brng.u32() & 4095);
                        uint16_t st = sec->get(idx);
                        if (randomTicking(blockIdOf(st))) {
                            int lx = idx & 15, lz = (idx >> 4) & 15, ly = idx >> 8;
                            randomTickBlock(c->cx * 16 + lx, c->sectionY(s) + ly, c->cz * 16 + lz, st);
                        }
                    }
                }
            }
    }
}

void Server::tickBlocks() {
    runTimers();
    randomTicks();
    // digging progress animation for other players
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& p = players[i];
        if (!p.inPlay() || !p.digging) continue;
        InDim in(*this, p.e.dim);
        float need = digTicks(p, blockAt(p.digX, p.digY, p.digZ));
        if (need <= 0 || need > 1e8f) continue;
        int stage = (int)((ticks - p.digStart) * 10 / need);
        if (stage > 9) stage = 9;
        if (stage != p.digStage) {
            p.digStage = (int8_t)stage;
            breakAnimation(*this, p, stage);
        }
    }
}

}  // namespace mc
