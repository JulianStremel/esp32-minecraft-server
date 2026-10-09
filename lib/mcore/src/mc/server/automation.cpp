#include "mc/server/automation.h"
#include "mc/server/server.h"
#include "mc/registry.h"
#include "mc/world/noise.h"
#include <algorithm>
#include <math.h>
#include <string.h>

namespace mc {
namespace {
const char* const faces[] = {"down", "up", "north", "south", "west", "east"};
Rng autoRandom(0x48505052);
double gaussian() {
    double u = std::max(1e-9, (double)autoRandom.unit());
    return sqrt(-2 * log(u)) * cos(2 * M_PI * autoRandom.unit());
}
void effect(Server& s, PistonPos p, int event, int data = 0) {
    Packet packet(pkt::s2c::WorldEvent);
    packet.w.i32(event);
    packet.w.u64(packPos(p.x, p.y, p.z));
    packet.w.i32(data);
    packet.w.boolean(false);
    s.broadcastNear(packet, p.x >> 4, p.z >> 4);
}
int facing(uint16_t state) {
    const char* name = getPropStr(state, "facing");
    for (int i = 0; i < 6; ++i)
        if (name && !strcmp(name, faces[i])) return i;
    return 0;
}
struct Inventory {
    TileEntity* parts[2] = {};
    PistonPos positions[2];
    int size() const { return (parts[0] ? parts[0]->slotCount() : 0) + (parts[1] ? parts[1]->slotCount() : 0); }
    TileEntity* tile(int slot) const { return slot < parts[0]->slotCount() ? parts[0] : parts[1]; }
    ItemStack& stack(int slot) const {
        return slot < parts[0]->slotCount() ? parts[0]->items[slot] : parts[1]->items[slot - parts[0]->slotCount()];
    }
    PistonPos pos(int slot) const { return positions[slot < parts[0]->slotCount() ? 0 : 1]; }
};
TileEntity* containerTile(Server& s, PistonPos pos) {
    uint16_t state = s.blockAt(pos.x, pos.y, pos.z);
    uint8_t type = Automation::tileType(blockIdOf(state));
    if (!type) return nullptr;
    Chunk* c = s.world.get(s.curDim, pos.x >> 4, pos.z >> 4);
    if (!c || c->readOnly) return nullptr;
    TileEntity* t = c->tileAt(pos.x & 15, pos.y, pos.z & 15);
    if (!t) {
        t = c->addTile(type, pos.x & 15, pos.y, pos.z & 15);
        c->dirty = true;
        if (t) t->tickOrder = ++s.blockEntitySequence;
    }
    if (t && t->type == TILE_FURNACE) s.updateFurnace(pos.x, pos.y, pos.z, true);
    return t && t->type == type ? t : nullptr;
}
Inventory inventory(Server& s, PistonPos p) {
    Inventory inv;
    inv.parts[0] = containerTile(s, p);
    inv.positions[0] = p;
    uint16_t state = s.blockAt(p.x, p.y, p.z), id = blockIdOf(state);
    if (!inv.parts[0] || (id != blk::Chest && id != blk::TrappedChest)) return inv;
    const char* type = getPropStr(state, "type");
    if (!strcmp(type, "single")) return inv;
    int f = facing(state), cw = f == 2 ? 5 : f == 5 ? 3 : f == 3 ? 4 : 2;
    PistonPos partner = p.offset(!strcmp(type, "left") ? cw : cw ^ 1);
    uint16_t other = s.blockAt(partner.x, partner.y, partner.z);
    if (blockIdOf(other) != id || facing(other) != f || !strcmp(getPropStr(other, "type"), type) ||
        !strcmp(getPropStr(other, "type"), "single"))
        return inv;
    inv.parts[1] = containerTile(s, partner);
    inv.positions[1] = partner;
    // The right half precedes the left half in Java's combined container.
    if (inv.parts[1] && !strcmp(type, "left")) {
        std::swap(inv.parts[0], inv.parts[1]);
        std::swap(inv.positions[0], inv.positions[1]);
    }
    return inv;
}
bool empty(const TileEntity& t) {
    for (int i = 0; i < t.slotCount(); ++i)
        if (!t.items[i].empty()) return false;
    return true;
}
bool canInsert(const TileEntity& t, int slot, const ItemStack& st, int face) {
    if (t.type == TILE_BOOKSHELF) return t.items[slot].empty() && Automation::isShelfBook(st.id);
    if (t.type == TILE_CRAFTER) return Automation::crafterAccepts(t, slot, st);
    if (t.type != TILE_FURNACE) return true;
    if (slot == 2) return false;
    if (face >= 0 && slot != (face == 1 ? 0 : 1)) return false;
    return slot != 1 || furnaceFuelTicks(st.id) > 0 || (st.id == itm::Bucket && t.items[1].id != itm::Bucket);
}
bool canExtract(const TileEntity& t, int slot, int face) {
    if (t.type != TILE_FURNACE) return true;
    if (face == 1) return slot == 0;
    if (face != 0) return slot == 1;
    return slot == 2 || (slot == 1 && (t.items[1].id == itm::Bucket || t.items[1].id == itm::WaterBucket));
}
bool insert(Server& s, Inventory& inv, ItemStack& from, int face, TileEntity* source, int limit) {
    bool changed = false;
    for (int i = 0; i < inv.size() && !from.empty() && limit > 0; ++i) {
        TileEntity* t = inv.tile(i);
        int slot = i < inv.parts[0]->slotCount() ? i : i - inv.parts[0]->slotCount();
        ItemStack& to = inv.stack(i);
        if (!canInsert(*t, slot, from, face) || (!to.empty() && !to.sameItem(from))) continue;
        int room = t->type == TILE_BOOKSHELF ? 1 : maxStack(from.id);   // one book per shelf slot
        int count = std::min(limit, std::min((int)from.count, room - (to.empty() ? 0 : to.count)));
        if (count <= 0) continue;
        if (t->type == TILE_BOOKSHELF) t->lastSlot = (int8_t)slot;
        bool wasEmpty = empty(*t);
        if (to.empty()) {
            to = from;
            to.count = 0;
        }
        to.count += count;
        from.count -= count;
        if (from.empty()) from.clear();
        limit -= count;
        if (wasEmpty && t->type == TILE_HOPPER && t->transferCooldown <= 8)
            t->transferCooldown = 8 - (source && source->type == TILE_HOPPER && t->updated >= source->updated ? 1 : 0);
        PistonPos p = inv.pos(i);
        s.containerChanged(p.x, p.y, p.z);
        changed = true;
    }
    return changed;
}
} // namespace
uint8_t Automation::tileType(uint16_t id) {
    if (id == blk::Chest || id == blk::TrappedChest) return TILE_CHEST;
    if (id == blk::Barrel) return TILE_BARREL;
    if (id == blk::Furnace || id == blk::BlastFurnace || id == blk::Smoker) return TILE_FURNACE;
    if (id == blk::Hopper) return TILE_HOPPER;
    if (id == blk::Dropper) return TILE_DROPPER;
    if (id == blk::Dispenser) return TILE_DISPENSER;
    if (id == blk::ChiseledBookshelf) return TILE_BOOKSHELF;
    if (id == blk::Crafter) return TILE_CRAFTER;
    return TILE_NONE;
}
// The #bookshelf_books item tag.
bool Automation::isShelfBook(uint16_t item) {
    return item == itm::Book || item == itm::WrittenBook || item == itm::WritableBook || item == itm::EnchantedBook ||
           item == itm::KnowledgeBook;
}
// Vanilla's CrafterBlockEntity#canPlaceItem: not a disabled slot, room in it, and no
// later enabled slot that is empty or holds fewer of the same item (so automation fills
// the grid evenly).
bool Automation::crafterAccepts(const TileEntity& t, int slot, const ItemStack& st) {
    if (t.disabledSlots & (1u << slot)) return false;
    const ItemStack& here = t.items[slot];
    if (here.empty()) return true;
    if (here.count >= maxStack(here.id) || !here.sameItem(st)) return false;
    for (int i = slot + 1; i < 9; i++) {
        if (t.disabledSlots & (1u << i)) continue;
        const ItemStack& o = t.items[i];
        if (o.empty() || (o.count < here.count && o.sameItem(here))) return false;
    }
    return true;
}
TileEntity* Automation::container(Server& s, PistonPos pos) { return containerTile(s, pos); }

// CrafterBlock#dispenseFrom: the grid's recipe result goes into the container in front,
// the rest out of the front face; the grid is used up (buckets stay). Level events:
// 1049 crafted, 1050 failed, 2010 particles toward the front.
void Automation::craft(Server& s, PistonPos p) {
    uint16_t state = s.blockAt(p.x, p.y, p.z);
    if (blockIdOf(state) != blk::Crafter) return;
    TileEntity* t = containerTile(s, p);
    if (!t) return;
    ItemStack result = matchCraftingRecipe(t->items, 3);
    if (result.empty()) {
        effect(s, p, 1050);
        return;
    }
    // orientation "<front>_<top>", e.g. north_up
    const char* o = getPropStr(state, "orientation");
    int face = 2;
    for (int i = 0; i < 6; ++i)
        if (o && !strncmp(o, faces[i], strlen(faces[i])) && o[strlen(faces[i])] == '_') face = i;
    s.world.setBlock(s.curDim, p.x, p.y, p.z, setBool(state, "crafting", true), true, 2);
    s.scheduleTick(p.x, p.y, p.z, 6);   // ends the crafting look (craftPending is false)
    Inventory target = inventory(s, p.offset(face));
    if (target.parts[0]) insert(s, target, result, face ^ 1, t, 64);
    if (!result.empty()) {
        double x = p.x + .5 + FACE_DX[face] * .7, y = p.y + .5 + FACE_DY[face] * .7, z = p.z + .5 + FACE_DZ[face] * .7;
        Entity* item = s.dropItem(x, y, z, result, false);
        if (item) {
            item->vx = FACE_DX[face] * .2 + gaussian() * .045;
            item->vy = FACE_DY[face] * .2 + .1;
            item->vz = FACE_DZ[face] * .2 + gaussian() * .045;
        }
    }
    consumeCraftingGrid(t->items, 9);
    s.containerChanged(p.x, p.y, p.z);
    effect(s, p, 1049);
    effect(s, p, 2010, face);
}

bool Automation::insertOne(Server& s, PistonPos p, ItemStack& from, int face, TileEntity* source) {
    Inventory to = inventory(s, p);
    return insert(s, to, from, face, source, 1);
}
void Automation::hopper(Server& s, PistonPos p) {
    uint16_t state = s.blockAt(p.x, p.y, p.z);
    if (blockIdOf(state) != blk::Hopper) return;
    TileEntity* t = containerTile(s, p);
    if (!t) return;
    int previous = t->transferCooldown;
    t->transferCooldown = std::max(0, (int)t->transferCooldown - 1);
    t->updated = s.worldTick();
    if (previous != t->transferCooldown) s.world.markDirty(s.curDim, p.x >> 4, p.z >> 4);
    if (t->transferCooldown > 0 || !getBool(state, "enabled")) return;
    bool changed = false;
    int face = facing(state);
    Inventory target = inventory(s, p.offset(face));
    for (int i = 0; i < 5; ++i)
        if (!t->items[i].empty() && insert(s, target, t->items[i], face ^ 1, t, 1)) {
            changed = true;
            break;
        }
    bool full = true;
    for (int i = 0; i < 5; ++i)
        if (t->items[i].empty() || t->items[i].count < maxStack(t->items[i].id)) full = false;
    if (!full) {
        Inventory self;
        self.parts[0] = t;
        self.positions[0] = p;
        Inventory above = inventory(s, p.offset(1));
        if (above.parts[0]) {
            // Furnace DOWN exposes output, then the bucket/fuel slot.
            int order[54];
            int count = above.size();
            for (int i = 0; i < count; ++i)
                order[i] = i;
            if (above.parts[0]->type == TILE_FURNACE) {
                count = 2;
                order[0] = 2;
                order[1] = 1;
            }
            for (int n = 0; n < count; ++n) {
                int i = order[n], slot = i < above.parts[0]->slotCount() ? i : i - above.parts[0]->slotCount();
                ItemStack& from = above.stack(i);
                if (from.empty() || !canExtract(*above.tile(i), slot, 0)) continue;
                if (insert(s, self, from, -1, above.tile(i), 1)) {
                    PistonPos q = above.pos(i);
                    if (above.tile(i)->type == TILE_BOOKSHELF) above.tile(i)->lastSlot = (int8_t)slot;
                    s.containerChanged(q.x, q.y, q.z);
                    changed = true;
                    break;
                }
            }
        } else {
            const PistonBox inside{{p.x + .125, p.y + .6875, p.z + .125}, {p.x + .875, p.y + 1.0, p.z + .875}};
            const PistonBox aboveBox{{p.x + 0.0, p.y + 1.0, p.z + 0.0}, {p.x + 1.0, p.y + 2.0, p.z + 1.0}};
            for (Entity& e : s.entities) {
                if (e.kind != EK_ITEM || e.removed || e.dim != s.curDim) continue;
                PistonBox box{{e.x - e.width * .5, e.y, e.z - e.width * .5},
                              {e.x + e.width * .5, e.y + e.height, e.z + e.width * .5}};
                if (!inside.intersects(box) && !aboveBox.intersects(box)) continue;
                if (insert(s, self, e.item, -1, nullptr, 64)) {
                    e.metaDirty = true;
                    if (e.item.empty()) {
                        s.removeEntity(e);
                        changed = true;
                        break;
                    }
                }
            }
        }
    }
    if (changed) {
        t->transferCooldown = 8;
        s.containerChanged(p.x, p.y, p.z);
    }
}
void Automation::dispense(Server& s, PistonPos p) {
    uint16_t state = s.blockAt(p.x, p.y, p.z);
    int id = blockIdOf(state);
    if (id != blk::Dropper && id != blk::Dispenser) return;
    TileEntity* t = containerTile(s, p);
    if (!t) return;
    int selected = -1, seen = 0;
    for (int i = 0; i < 9; ++i)
        if (!t->items[i].empty() && autoRandom.range(++seen) == 0) selected = i;
    if (selected < 0) {
        effect(s, p, 1001);
        return;
    }
    int face = facing(state);
    PistonPos dest = p.offset(face);
    Inventory target = inventory(s, dest);
    if (id == blk::Dropper && target.parts[0]) {
        if (insert(s, target, t->items[selected], face ^ 1, t, 1)) s.containerChanged(p.x, p.y, p.z);
        return;
    }
    ItemStack& stack = t->items[selected];
    ItemStack one = stack;
    one.count = 1;
    if (id == blk::Dispenser) {
        uint16_t ahead = s.blockAt(dest.x, dest.y, dest.z);
        auto consume = [&]() {
            --stack.count;
            if (stack.empty()) stack.clear();
        };
        auto finish = [&](bool success, int sound = 1000) {
            s.containerChanged(p.x, p.y, p.z);
            effect(s, p, success ? sound : 1001);
            effect(s, p, 2000, face);
        };
        auto replace = [&](uint16_t item) {
            if (stack.count == 1)
                stack = ItemStack::of(item);
            else {
                consume();
                ItemStack returned = ItemStack::of(item);
                Inventory self;
                self.parts[0] = t;
                self.positions[0] = p;
                if (!insert(s, self, returned, -1, nullptr, 1))
                    s.dropItem(p.x + .5 + FACE_DX[face] * .7, p.y + .5 + FACE_DY[face] * .7,
                               p.z + .5 + FACE_DZ[face] * .7, returned);
            }
        };
        if (one.id == itm::Tnt) {
            Entity* primed = s.spawnEntity(EK_TNT, ent::Tnt, dest.x + .5, dest.y, dest.z + .5);
            if (primed) {
                primed->fuse = 80;
                primed->vy = .2;
                double angle = autoRandom.unit() * M_PI * 2;
                primed->vx = -sin(angle) * .02;
                primed->vz = -cos(angle) * .02;
                consume();
                s.playSound("entity.tnt.primed", dest.x + .5, dest.y, dest.z + .5, 1, 1, 4);
            }
            finish(primed != nullptr);
            return;
        }
        if (one.id == itm::Arrow || one.id == itm::SpectralArrow || one.id == itm::TippedArrow) {
            Entity* arrow = s.spawnEntity(EK_ARROW, one.id == itm::SpectralArrow ? ent::SpectralArrow : ent::Arrow,
                                          p.x + .5 + FACE_DX[face] * .7, p.y + .5 + FACE_DY[face] * .7,
                                          p.z + .5 + FACE_DZ[face] * .7);
            if (arrow) {
                double dx = FACE_DX[face], dy = FACE_DY[face] + .1, dz = FACE_DZ[face],
                       length = sqrt(dx * dx + dy * dy + dz * dz);
                arrow->vx = (dx / length + gaussian() * .045) * 1.1;
                arrow->vy = (dy / length + gaussian() * .045) * 1.1;
                arrow->vz = (dz / length + gaussian() * .045) * 1.1;
                arrow->damage = 2;
                arrow->velDirty = true;
                consume();
            }
            finish(arrow != nullptr, 1002);
            return;
        }
        if (one.id == itm::WaterBucket || one.id == itm::LavaBucket) {
            bool water = one.id == itm::WaterBucket;
            bool accepts = stateIsAir(ahead) || stateIsFluid(ahead) || (blockOf(ahead).flags & BF_REPLACEABLE) ||
                           (water && getProp(ahead, "waterlogged") == 1);
            if (accepts) {
                if (water && s.curDim == DIM_NETHER)
                    s.playSound("block.fire.extinguish", dest.x + .5, dest.y + .5, dest.z + .5, .5f, 2.6f, 4);
                else {
                    uint16_t state = water && getProp(ahead, "waterlogged") == 1 ? setBool(ahead, "waterlogged", true)
                                     : water                                     ? bs::Water
                                                                                 : bs::Lava;
                    s.setBlock(dest.x, dest.y, dest.z, state);
                    s.scheduleTick(dest.x, dest.y, dest.z, s.fluidDelay(water ? blk::Water : blk::Lava));
                    s.playSound(water ? "item.bucket.empty" : "item.bucket.empty_lava", dest.x + .5, dest.y + .5,
                                dest.z + .5, 1, 1, 4);
                }
                replace(itm::Bucket);
                finish(true);
                return;
            }
        } else if (one.id == itm::Bucket) {
            int fluid = blockIdOf(ahead);
            if ((fluid == blk::Water || fluid == blk::Lava) && getProp(ahead, "level") == 0) {
                s.setBlock(dest.x, dest.y, dest.z, bs::Air);
                replace(fluid == blk::Water ? itm::WaterBucket : itm::LavaBucket);
                finish(true);
                return;
            }
            if (getProp(ahead, "waterlogged") == 0) {
                s.setBlock(dest.x, dest.y, dest.z, setBool(ahead, "waterlogged", false));
                replace(itm::WaterBucket);
                finish(true);
                return;
            }
        } else if (one.id == itm::FlintAndSteel) {
            bool success = false;
            if (blockIdOf(ahead) == blk::Tnt)
                success = s.primeTnt(dest.x, dest.y, dest.z) != nullptr;
            else if ((blockIdOf(ahead) == blk::Campfire || blockIdOf(ahead) == blk::SoulCampfire) &&
                     !getBool(ahead, "lit") && !getBool(ahead, "waterlogged")) {
                s.setBlock(dest.x, dest.y, dest.z, setBool(ahead, "lit", true));
                success = true;
            } else if (stateIsAir(ahead) && stateCollides(s.blockAt(dest.x, dest.y - 1, dest.z))) {
                s.setBlock(dest.x, dest.y, dest.z, bs::Fire);
                s.scheduleTick(dest.x, dest.y, dest.z, 200);
                success = true;
            }
            if (success && ++stack.damage >= ITEMS[stack.id].durability) stack.clear();
            finish(success);
            return;
        }
    }
    // The ordinary dispense behaviour ejects one item. Item-specific dispenser
    // behaviours are dispatched here as they are implemented.
    double x = p.x + .5 + FACE_DX[face] * .7, y = p.y + .5 + FACE_DY[face] * .7 - (face < 2 ? .125 : .15625),
           z = p.z + .5 + FACE_DZ[face] * .7;
    Entity* item = s.dropItem(x, y, z, one, false);
    if (!item) return;
    double speed = .2 + autoRandom.unit() * .1;
    item->vx = FACE_DX[face] * speed + gaussian() * .045;
    item->vy = .2 + gaussian() * .045;
    item->vz = FACE_DZ[face] * speed + gaussian() * .045;
    --stack.count;
    if (stack.empty()) stack.clear();
    s.containerChanged(p.x, p.y, p.z);
    effect(s, p, 1000);
    effect(s, p, 2000, face);
}
} // namespace mc

namespace mc {
void Server::tickBlockEntities() {
    Server& s = *this;
    // Snapshot positions, not tile pointers: finalization can notify another
    // piston and remove/recreate block entities. No world blocks are scanned.
    struct Entry {
        PistonPos pos;
        uint8_t dim;
        uint64_t order;
    };
    int count = 0;
    for (int i = 0; i < s.world.tableSize(); ++i)
        if (Chunk* c = s.world.slot(i)) count += c->tickingBlockEntities();
    if (!count) return;
    if (count > tileTickCapacity_) {
        int capacity = tileTickCapacity_ ? tileTickCapacity_ : 32;
        while (capacity < count && capacity < 65536)
            capacity *= 2;
        void* expanded = capacity >= count ? plat::bigAlloc(sizeof(Entry) * capacity) : nullptr;
        if (!expanded) {
            ++redstone.failures;
            MC_LOGE("Block-entity tick capacity/allocation failed");
            return;
        }
        plat::bigFree(tileTickList_);
        tileTickList_ = expanded;
        tileTickCapacity_ = capacity;
    }
    Entry* entries = (Entry*)tileTickList_;
    int n = 0;
    for (int i = 0; i < s.world.tableSize(); ++i)
        if (Chunk* c = s.world.slot(i); c && c->tickingBlockEntities())
            for (TileEntity* t = c->tiles(); t; t = t->next)
                if (t->type == TILE_PISTON || t->type == TILE_HOPPER || t->type == TILE_DAYLIGHT) {
                    if (!t->tickOrder) t->tickOrder = ++s.blockEntitySequence;
                    entries[n++] = {{c->cx * 16 + t->lx, t->y, c->cz * 16 + t->lz}, c->dim, t->tickOrder};
                }
    std::sort(entries, entries + n, [](const Entry& a, const Entry& b) { return a.order < b.order; });
    for (int i = 0; i < n; ++i) {
        Server::InDim in(s, entries[i].dim);
        PistonPos p = entries[i].pos;
        Chunk* c = s.world.get(s.curDim, p.x >> 4, p.z >> 4);
        TileEntity* t = c ? c->tileAt(p.x & 15, p.y, p.z & 15) : nullptr;
        if (!t || t->tickOrder != entries[i].order) continue;
        if (t->type == TILE_HOPPER)
            Automation::hopper(s, p);
        else if (t->type == TILE_PISTON)
            Pistons::tick(s, p);
        else if (t->type == TILE_DAYLIGHT && s.worldTick() % 20 == 0)
            s.redstone.daylightDetector(s, p.x, p.y, p.z, s.blockAt(p.x, p.y, p.z));
    }
}
} // namespace mc
