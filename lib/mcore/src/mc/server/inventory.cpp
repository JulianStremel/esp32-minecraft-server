// Inventories and windows: clicks (all modes), crafting, chests, furnaces,
// creative inventory, item giving/consuming.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "mc/registry.h"
#include "mc/server/server.h"

namespace mc {

// menu type registry ids (1.16.5)
enum { MENU_9X3 = 2, MENU_9X6 = 5, MENU_CRAFTING = 11, MENU_FURNACE = 13, MENU_BLAST_FURNACE = 9, MENU_SMOKER = 21 };

// ---------------------------------------------------------------- smelting / fuel
struct Smelt { uint16_t in, out; };
static const Smelt SMELTING[] = {
    {itm::IronOre, itm::IronIngot}, {itm::GoldOre, itm::GoldIngot}, {itm::Sand, itm::Glass}, {itm::RedSand, itm::Glass},
    {itm::Cobblestone, itm::Stone}, {itm::Stone, itm::SmoothStone}, {itm::ClayBall, itm::Brick}, {itm::Clay, itm::Terracotta},
    {itm::Netherrack, itm::NetherBrick}, {itm::Cactus, itm::GreenDye}, {itm::WetSponge, itm::Sponge},
    {itm::Beef, itm::CookedBeef}, {itm::Porkchop, itm::CookedPorkchop}, {itm::Chicken, itm::CookedChicken},
    {itm::Mutton, itm::CookedMutton}, {itm::Rabbit, itm::CookedRabbit}, {itm::Cod, itm::CookedCod},
    {itm::Salmon, itm::CookedSalmon}, {itm::Potato, itm::BakedPotato}, {itm::Kelp, itm::DriedKelp},
    {itm::Sandstone, itm::SmoothSandstone}, {itm::RedSandstone, itm::SmoothRedSandstone}, {itm::QuartzBlock, itm::SmoothQuartz},
    {itm::StoneBricks, itm::CrackedStoneBricks}, {itm::DiamondOre, itm::Diamond}, {itm::EmeraldOre, itm::Emerald},
    {itm::CoalOre, itm::Coal}, {itm::LapisOre, itm::LapisLazuli}, {itm::RedstoneOre, itm::Redstone},
    {itm::NetherQuartzOre, itm::Quartz}, {itm::AncientDebris, itm::NetheriteScrap}, {itm::ChorusFruit, itm::PoppedChorusFruit},
    {itm::SeaPickle, itm::LimeDye}, {itm::NetherGoldOre, itm::GoldIngot},
};

static uint16_t smeltResult(uint16_t in) {
    for (const Smelt& s : SMELTING)
        if (s.in == in) return s.out;
    const char* n = ITEMS[in].name;
    size_t l = strlen(n);
    if ((l > 4 && !strcmp(n + l - 4, "_log")) || (l > 5 && !strcmp(n + l - 5, "_wood"))) return itm::Charcoal;
    return 0;
}

static int fuelTicks(uint16_t item) {
    switch (item) {
        case itm::Coal: case itm::Charcoal: return 1600;
        case itm::CoalBlock: return 16000;
        case itm::LavaBucket: return 20000;
        case itm::BlazeRod: return 2400;
        case itm::DriedKelpBlock: return 4001;
        case itm::Stick: case itm::Bowl: return 100;
        case itm::Bamboo: case itm::Scaffolding: return 50;
        case itm::CraftingTable: case itm::Chest: case itm::TrappedChest: case itm::Bookshelf: case itm::Barrel: return 300;
        default: break;
    }
    const char* n = ITEMS[item].name;
    if (strstr(n, "planks") || strstr(n, "_log") || strstr(n, "_wood") || strstr(n, "fence")) return 300;
    if (!strncmp(n, "wooden_", 7)) return 200;
    if (strstr(n, "_sapling") || strstr(n, "_wool")) return 100;
    if (strstr(n, "_carpet")) return 67;
    if (strstr(n, "_slab") && ITEMS[item].block != 0xFFFF && BLOCKS[ITEMS[item].block].toolClass == TC_AXE) return 150;
    if (strstr(n, "_boat")) return 1200;
    return 0;
}

// ---------------------------------------------------------------- window slot mapping
static int containerSize(const Player& p) {
    switch (p.winKind) {
        case WK_CHEST: return 27;
        case WK_LARGE_CHEST: return 54;
        case WK_CRAFTING: return 10;
        case WK_FURNACE: return 3;
        default: return 9;  // player inventory: 0..8 are result/grid/armor
    }
}

static TileEntity* tileAt(Server& s, int x, int y, int z, uint8_t type) {
    Chunk* c = s.world.load(x >> 4, z >> 4);
    TileEntity* t = c->tileAt(x & 15, y, z & 15);
    if (!t) t = c->addTile(type, x & 15, y, z & 15);
    return t;
}

static void markTileDirty(Server& s, int x, int z) {
    Chunk* c = s.world.get(x >> 4, z >> 4);
    if (c) c->dirty = true;
}

// Returns the stack behind a window slot, or nullptr for an invalid slot.
static ItemStack* slotRef(Server& s, Player& p, int slot) {
    if (slot < 0) return nullptr;
    if (p.winKind == WK_NONE) return slot < INV_SIZE ? &p.inv[slot] : nullptr;
    int cs = containerSize(p);
    if (slot >= cs) {
        int inv = SLOT_MAIN_START + (slot - cs);
        return inv < SLOT_OFFHAND ? &p.inv[inv] : nullptr;
    }
    switch (p.winKind) {
        case WK_CHEST: return &tileAt(s, p.winX, p.winY, p.winZ, TILE_CHEST)->items[slot];
        case WK_LARGE_CHEST:
            if (slot < 27) return &tileAt(s, p.winX, p.winY, p.winZ, TILE_CHEST)->items[slot];
            return &tileAt(s, p.winX2, p.winY, p.winZ2, TILE_CHEST)->items[slot - 27];
        case WK_CRAFTING: return &p.craft[slot];
        case WK_FURNACE: return &tileAt(s, p.winX, p.winY, p.winZ, TILE_FURNACE)->items[slot];
        default: return nullptr;
    }
}

static bool isResultSlot(const Player& p, int slot) {
    return (p.winKind == WK_NONE && slot == SLOT_CRAFT_RESULT) || (p.winKind == WK_CRAFTING && slot == 0) ||
           (p.winKind == WK_FURNACE && slot == 2);
}

static bool isCraftResult(const Player& p, int slot) {
    return (p.winKind == WK_NONE && slot == SLOT_CRAFT_RESULT) || (p.winKind == WK_CRAFTING && slot == 0);
}

static void sendWindow(Server& s, Player& p) {
    int total = p.winKind == WK_NONE ? INV_SIZE : containerSize(p) + 36;
    Packet pk(pkt::s2c::WindowItems);
    pk.w.u8((uint8_t)p.winId);
    pk.w.i16((int16_t)total);
    for (int i = 0; i < total; i++) {
        ItemStack* st = slotRef(s, p, i);
        writeSlot(pk.w, st ? *st : ItemStack());
    }
    p.conn.send(pk);
    Packet cur(pkt::s2c::SetSlot);
    cur.w.i8(-1);
    cur.w.i16(-1);
    writeSlot(cur.w, p.cursor);
    p.conn.send(cur);
}

// ---------------------------------------------------------------- crafting
static ItemStack matchRecipe(const ItemStack* grid, int size) {
    // bounding box of used cells
    int minX = size, minY = size, maxX = -1, maxY = -1, used = 0;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++)
            if (!grid[y * size + x].empty()) {
                used++;
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
    if (!used) return ItemStack();
    int bw = maxX - minX + 1, bh = maxY - minY + 1;
    for (int r = 0; r < NUM_RECIPES; r++) {
        const RecipeDef& rd = RECIPES[r];
        const uint16_t* ing = RECIPE_INGREDIENTS + rd.start;
        if (rd.shapeless) {
            if (rd.w != used) continue;
            bool taken[9] = {false};
            bool ok = true;
            for (int k = 0; k < rd.w && ok; k++) {
                bool found = false;
                for (int c = 0; c < size * size; c++) {
                    if (taken[c] || grid[c].empty() || grid[c].id != ing[k]) continue;
                    taken[c] = true;
                    found = true;
                    break;
                }
                ok = found;
            }
            if (ok) return ItemStack::of(rd.result, rd.count);
            continue;
        }
        if (rd.w != bw || rd.h != bh) continue;
        for (int mirror = 0; mirror < 2; mirror++) {
            bool ok = true;
            for (int y = 0; y < bh && ok; y++)
                for (int x = 0; x < bw && ok; x++) {
                    int rx = mirror ? bw - 1 - x : x;
                    uint16_t want = ing[y * bw + rx];
                    const ItemStack& have = grid[(minY + y) * size + (minX + x)];
                    if (want == 0) ok = have.empty();
                    else ok = !have.empty() && have.id == want;
                }
            if (ok) return ItemStack::of(rd.result, rd.count);
        }
    }
    return ItemStack();
}

static void updateCraftResult(Player& p) {
    if (p.winKind == WK_CRAFTING) {
        p.craft[0] = matchRecipe(p.craft + 1, 3);
    } else if (p.winKind == WK_NONE) {
        p.inv[SLOT_CRAFT_RESULT] = matchRecipe(p.inv + SLOT_CRAFT_START, 2);
    }
}

static void consumeCraftGrid(Player& p) {
    ItemStack* grid = p.winKind == WK_CRAFTING ? p.craft + 1 : p.inv + SLOT_CRAFT_START;
    int n = p.winKind == WK_CRAFTING ? 9 : 4;
    for (int i = 0; i < n; i++) {
        ItemStack& g = grid[i];
        if (g.empty()) continue;
        uint16_t remainder = 0;
        if (g.id == itm::WaterBucket || g.id == itm::LavaBucket || g.id == itm::MilkBucket) remainder = itm::Bucket;
        if (g.id == itm::HoneyBottle) remainder = itm::GlassBottle;
        if (--g.count == 0) g.clear();
        if (remainder && g.empty()) g = ItemStack::of(remainder);
    }
}

// ---------------------------------------------------------------- giving / consuming
int Server::giveItem(Player& p, ItemStack st) {
    if (st.empty()) return 0;
    int maxS = maxStack(st.id);
    int order[37];
    int n = 0;
    order[n++] = SLOT_HOTBAR_START + p.held;
    order[n++] = SLOT_OFFHAND;
    for (int i = SLOT_HOTBAR_START; i < SLOT_OFFHAND; i++) order[n++] = i;
    for (int i = SLOT_MAIN_START; i < SLOT_HOTBAR_START; i++) order[n++] = i;
    // merge into matching stacks
    for (int k = 0; k < n && st.count; k++) {
        ItemStack& s = p.inv[order[k]];
        if (s.empty() || !s.sameItem(st) || s.count >= maxS) continue;
        int mv = maxS - s.count < st.count ? maxS - s.count : st.count;
        s.count = (uint8_t)(s.count + mv);
        st.count = (uint8_t)(st.count - mv);
        p.sendSlot(order[k]);
    }
    // empty slots (not the offhand)
    for (int k = 2; k < n && st.count; k++) {
        ItemStack& s = p.inv[order[k]];
        if (!s.empty()) continue;
        s = st;
        if (s.count > maxS) s.count = (uint8_t)maxS;
        st.count = (uint8_t)(st.count - s.count);
        p.sendSlot(order[k]);
    }
    if (order[0] == SLOT_HOTBAR_START + p.held) broadcastEquipment(p);
    if (p.winKind != WK_NONE) sendWindow(*this, p);
    return st.count;
}

void Server::damageHeldItem(Player& p, int amount) {
    if (amount <= 0 || p.gamemode == GM_CREATIVE) return;
    ItemStack& h = p.heldItem();
    if (h.empty() || !ITEMS[h.id].durability) return;
    h.damage = (uint16_t)(h.damage + amount);
    if (h.damage >= ITEMS[h.id].durability) {
        h.clear();
        playSound("entity.item.break", p.e.x, p.e.y, p.e.z, 0.8f, 1, 7);
        broadcastEquipment(p);
    }
    p.sendSlot(SLOT_HOTBAR_START + p.held);
}

void Server::consumeHeld(Player& p, int amount) {
    ItemStack& h = p.heldItem();
    if (h.empty()) return;
    h.count = (uint8_t)(h.count > amount ? h.count - amount : 0);
    if (!h.count) h.clear();
    p.sendSlot(SLOT_HOTBAR_START + p.held);
    broadcastEquipment(p);
}

// ---------------------------------------------------------------- windows
static void chestLid(Server& s, int x, int y, int z, int viewersDelta) {
    uint16_t st = s.blockAt(x, y, z);
    uint16_t id = blockIdOf(st);
    if (id != blk::Chest && id != blk::TrappedChest) return;
    int viewers = 0;
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& o = s.players[i];
        if (!o.inPlay() || (o.winKind != WK_CHEST && o.winKind != WK_LARGE_CHEST)) continue;
        if ((o.winX == x && o.winY == y && o.winZ == z) || (o.winKind == WK_LARGE_CHEST && o.winX2 == x && o.winZ2 == z && o.winY == y))
            viewers++;
    }
    viewers += viewersDelta;
    if (viewers < 0) viewers = 0;
    Packet pk(pkt::s2c::BlockAction);
    pk.w.u64(packPos(x, y, z));
    pk.w.u8(1);
    pk.w.u8((uint8_t)viewers);
    pk.w.varint(id);
    s.broadcastNear(pk, x >> 4, z >> 4);
    s.playSound(viewersDelta > 0 ? "block.chest.open" : "block.chest.close", x + 0.5, y + 0.5, z + 0.5, 0.5f, 0.95f, 4);
}

static void openWindow(Server& s, Player& p, uint8_t kind, int menuType, const char* title) {
    p.winKind = kind;
    p.winId = p.nextWinId;
    p.nextWinId = (int8_t)(p.nextWinId % 100 + 1);
    char json[128];
    snprintf(json, sizeof(json), "{\"translate\":\"%s\"}", title);
    Packet pk(pkt::s2c::OpenWindow);
    pk.w.varint(p.winId);
    pk.w.varint(menuType);
    pk.w.string(json);
    p.conn.send(pk);
    sendWindow(s, p);
}

void Server::openContainer(Player& p, int x, int y, int z) {
    closeWindow(p, true);
    uint16_t st = blockAt(x, y, z);
    uint16_t id = blockIdOf(st);
    p.winX = x; p.winY = y; p.winZ = z;
    if (id == blk::Barrel) {
        tileAt(*this, x, y, z, TILE_BARREL);
        openWindow(*this, p, WK_CHEST, MENU_9X3, "container.barrel");
        world.setBlock(x, y, z, setBool(st, "open", true));
        playSound("block.barrel.open", x + 0.5, y + 0.5, z + 0.5, 0.5f, 1, 4);
        return;
    }
    const char* type = getPropStr(st, "type");
    if (type && strcmp(type, "single")) {
        // partner: LEFT half -> clockwise of facing
        int f = 2;
        const char* fs = getPropStr(st, "facing");
        if (!strcmp(fs, "south")) f = 3; else if (!strcmp(fs, "west")) f = 4; else if (!strcmp(fs, "east")) f = 5;
        int cx = 0, cz = 0;
        switch (f) { case 2: cx = 1; break; case 5: cz = 1; break; case 3: cx = -1; break; default: cz = -1; break; }
        int sgn = !strcmp(type, "left") ? 1 : -1;
        int ox = x + cx * sgn, oz = z + cz * sgn;
        if (blockIdOf(blockAt(ox, y, oz)) == id) {
            // the RIGHT half holds the upper 27 slots (vanilla DoubleBlockCombiner order)
            bool right = !strcmp(type, "right");
            p.winX = right ? x : ox; p.winZ = right ? z : oz;
            p.winX2 = right ? ox : x; p.winZ2 = right ? oz : z;
            tileAt(*this, p.winX, y, p.winZ, TILE_CHEST);
            tileAt(*this, p.winX2, y, p.winZ2, TILE_CHEST);
            chestLid(*this, x, y, z, 1);
            chestLid(*this, ox, y, oz, 1);
            openWindow(*this, p, WK_LARGE_CHEST, MENU_9X6, "container.chestDouble");
            return;
        }
    }
    tileAt(*this, x, y, z, TILE_CHEST);
    chestLid(*this, x, y, z, 1);
    openWindow(*this, p, WK_CHEST, MENU_9X3, "container.chest");
}

void Server::openCrafting(Player& p, int x, int y, int z) {
    closeWindow(p, true);
    p.winX = x; p.winY = y; p.winZ = z;
    for (auto& c : p.craft) c.clear();
    openWindow(*this, p, WK_CRAFTING, MENU_CRAFTING, "container.crafting");
}

// furnaces that are currently burning/cooking get ticked
static int32_t s_furnaces[32][3];
static int s_furnaceCount = 0;

static void activateFurnace(int x, int y, int z) {
    for (int i = 0; i < s_furnaceCount; i++)
        if (s_furnaces[i][0] == x && s_furnaces[i][1] == y && s_furnaces[i][2] == z) return;
    if (s_furnaceCount < 32) {
        s_furnaces[s_furnaceCount][0] = x;
        s_furnaces[s_furnaceCount][1] = y;
        s_furnaces[s_furnaceCount][2] = z;
        s_furnaceCount++;
    }
}

static void sendFurnaceProps(Player& p, const TileEntity& t) {
    int16_t vals[4] = {t.burnTime, t.burnTotal, t.cookTime, 200};
    for (int i = 0; i < 4; i++) {
        Packet pk(pkt::s2c::CraftProgressBar);
        pk.w.u8((uint8_t)p.winId);
        pk.w.i16((int16_t)i);
        pk.w.i16(vals[i]);
        p.conn.send(pk);
    }
}

void Server::openFurnace(Player& p, int x, int y, int z) {
    closeWindow(p, true);
    uint16_t id = blockIdOf(blockAt(x, y, z));
    p.winX = x; p.winY = y; p.winZ = z;
    TileEntity* t = tileAt(*this, x, y, z, TILE_FURNACE);
    int menu = id == blk::BlastFurnace ? MENU_BLAST_FURNACE : (id == blk::Smoker ? MENU_SMOKER : MENU_FURNACE);
    const char* title = id == blk::BlastFurnace ? "container.blast_furnace" : (id == blk::Smoker ? "container.smoker" : "container.furnace");
    openWindow(*this, p, WK_FURNACE, menu, title);
    sendFurnaceProps(p, *t);
    activateFurnace(x, y, z);
}

void Server::closeWindow(Player& p, bool sendClose) {
    // return crafting grid contents
    if (p.winKind == WK_CRAFTING) {
        for (int i = 1; i <= 9; i++)
            if (!p.craft[i].empty()) {
                int left = giveItem(p, p.craft[i]);
                if (left) { ItemStack d = p.craft[i]; d.count = (uint8_t)left; throwItem(p, d); }
                p.craft[i].clear();
            }
        p.craft[0].clear();
    }
    if (p.winKind == WK_CHEST || p.winKind == WK_LARGE_CHEST) {
        uint16_t id = blockIdOf(blockAt(p.winX, p.winY, p.winZ));
        if (id == blk::Barrel) {
            world.setBlock(p.winX, p.winY, p.winZ, setBool(blockAt(p.winX, p.winY, p.winZ), "open", false));
        } else {
            uint8_t kind = p.winKind;
            p.winKind = WK_NONE;  // not counted as viewer any more
            chestLid(*this, p.winX, p.winY, p.winZ, 0);
            if (kind == WK_LARGE_CHEST) chestLid(*this, p.winX2, p.winY, p.winZ2, 0);
        }
    }
    if (!p.cursor.empty() && p.winKind != WK_NONE) {
        int left = giveItem(p, p.cursor);
        if (left) { ItemStack d = p.cursor; d.count = (uint8_t)left; throwItem(p, d); }
        p.cursor.clear();
    }
    if (sendClose && p.winKind != WK_NONE && p.winId) {
        Packet pk(pkt::s2c::CloseWindow);
        pk.w.u8((uint8_t)p.winId);
        p.conn.send(pk);
    }
    p.winKind = WK_NONE;
    p.winId = 0;
}

void Player::onCloseWindow(Reader& r) {
    r.u8();
    Server& s = *srv;
    if (winKind == WK_NONE) {
        // the 2x2 grid and cursor go back to the inventory
        for (int i = SLOT_CRAFT_START; i < SLOT_CRAFT_START + 4; i++)
            if (!inv[i].empty()) {
                ItemStack st = inv[i];
                inv[i].clear();
                int left = s.giveItem(*this, st);
                if (left) { st.count = (uint8_t)left; s.throwItem(*this, st); }
            }
        inv[SLOT_CRAFT_RESULT].clear();
        if (!cursor.empty()) {
            ItemStack st = cursor;
            cursor.clear();
            int left = s.giveItem(*this, st);
            if (left) { st.count = (uint8_t)left; s.throwItem(*this, st); }
        }
        sendInventory();
        return;
    }
    s.closeWindow(*this, false);
    sendInventory();
}

void Server::containerChanged(int x, int y, int z) {
    markTileDirty(*this, x, z);
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& o = players[i];
        if (!o.inPlay() || o.winKind == WK_NONE || o.winKind == WK_CRAFTING) continue;
        bool same = (o.winX == x && o.winY == y && o.winZ == z) ||
                    (o.winKind == WK_LARGE_CHEST && o.winX2 == x && o.winY == y && o.winZ2 == z);
        if (same) sendWindow(*this, o);
    }
    if (blockIdOf(blockAt(x, y, z)) == blk::Furnace || blockIdOf(blockAt(x, y, z)) == blk::BlastFurnace ||
        blockIdOf(blockAt(x, y, z)) == blk::Smoker)
        activateFurnace(x, y, z);
}

void Server::tickFurnaces() {
    for (int i = 0; i < s_furnaceCount;) {
        int x = s_furnaces[i][0], y = s_furnaces[i][1], z = s_furnaces[i][2];
        Chunk* c = world.get(x >> 4, z >> 4);
        uint16_t st = c ? c->get(x & 15, y, z & 15) : 0;
        uint16_t id = blockIdOf(st);
        TileEntity* t = c ? c->tileAt(x & 15, y, z & 15) : nullptr;
        if (!t || t->type != TILE_FURNACE || (id != blk::Furnace && id != blk::BlastFurnace && id != blk::Smoker)) {
            s_furnaces[i][0] = s_furnaces[--s_furnaceCount][0];
            s_furnaces[i][1] = s_furnaces[s_furnaceCount][1];
            s_furnaces[i][2] = s_furnaces[s_furnaceCount][2];
            continue;
        }
        ItemStack& in = t->items[0];
        ItemStack& fuel = t->items[1];
        ItemStack& out = t->items[2];
        bool wasBurning = t->burnTime > 0;
        if (t->burnTime > 0) t->burnTime--;
        uint16_t res = in.empty() ? 0 : smeltResult(in.id);
        bool canSmelt = res && (out.empty() || (out.id == res && out.count < maxStack(res)));
        bool changed = false;
        if (canSmelt) {
            if (t->burnTime == 0 && !fuel.empty() && fuelTicks(fuel.id) > 0) {
                t->burnTime = t->burnTotal = (int16_t)(fuelTicks(fuel.id) > 32000 ? 32000 : fuelTicks(fuel.id));
                if (fuel.id == itm::LavaBucket) fuel = ItemStack::of(itm::Bucket);
                else if (--fuel.count == 0) fuel.clear();
                changed = true;
            }
            int speed = (id == blk::BlastFurnace || id == blk::Smoker) ? 2 : 1;
            if (t->burnTime > 0) {
                t->cookTime = (int16_t)(t->cookTime + speed);
                if (t->cookTime >= 200) {
                    t->cookTime = 0;
                    if (out.empty()) out = ItemStack::of(res);
                    else out.count++;
                    if (--in.count == 0) in.clear();
                    changed = true;
                }
            }
        } else {
            t->cookTime = 0;
        }
        bool burning = t->burnTime > 0;
        if (burning != wasBurning) {
            world.setBlock(x, y, z, setBool(st, "lit", burning));
            changed = true;
        }
        // viewers
        for (int k = 0; k < MC_MAX_PLAYERS; k++) {
            Player& p = players[k];
            if (!p.inPlay() || p.winKind != WK_FURNACE || p.winX != x || p.winY != y || p.winZ != z) continue;
            if (ticks % 5 == 0 || changed) sendFurnaceProps(p, *t);
            if (changed) sendWindow(*this, p);
        }
        if (changed) c->dirty = true;
        if (!burning && !canSmelt) {
            s_furnaces[i][0] = s_furnaces[--s_furnaceCount][0];
            s_furnaces[i][1] = s_furnaces[s_furnaceCount][1];
            s_furnaces[i][2] = s_furnaces[s_furnaceCount][2];
            continue;
        }
        i++;
    }
}

// ---------------------------------------------------------------- clicks
static void confirm(Player& p, int8_t windowId, int16_t action, bool ok) {
    Packet pk(pkt::s2c::Transaction);
    pk.w.i8(windowId);
    pk.w.i16(action);
    pk.w.boolean(ok);
    p.conn.send(pk);
}

// Can `st` go into window slot `slot`? (armor slots, furnace output, result slots)
static bool slotAccepts(const Player& p, int slot, const ItemStack& st) {
    if (isResultSlot(p, slot)) return false;
    if (p.winKind == WK_NONE && slot >= SLOT_ARMOR_START && slot < SLOT_ARMOR_START + 4) {
        const ItemDef& d = ITEMS[st.id];
        int want = slot - SLOT_ARMOR_START;  // 0 helmet .. 3 boots
        if (d.kind == IK_ARMOR) return d.armorSlot == want;
        return want == 0 && (st.id == itm::CarvedPumpkin || strstr(d.name, "_head") || strstr(d.name, "skull"));
    }
    if (p.winKind == WK_FURNACE && slot == 1) return fuelTicks(st.id) > 0 || st.id == itm::Bucket;
    return true;
}

static int slotLimit(const Player& p, int slot, const ItemStack& st) {
    if (p.winKind == WK_NONE && slot >= SLOT_ARMOR_START && slot < SLOT_ARMOR_START + 4) return 1;
    return maxStack(st.id);
}

// Moves `st` into window slots [from, to) (merging first). Returns what is left.
static void moveInto(Server& s, Player& p, ItemStack& st, int from, int to, bool reverse) {
    int maxS = maxStack(st.id);
    for (int pass = 0; pass < 2 && !st.empty(); pass++) {
        for (int k = 0; k < to - from && !st.empty(); k++) {
            int i = reverse ? to - 1 - k : from + k;
            ItemStack* t = slotRef(s, p, i);
            if (!t || !slotAccepts(p, i, st)) continue;
            int lim = slotLimit(p, i, st) < maxS ? slotLimit(p, i, st) : maxS;
            if (pass == 0) {
                if (t->empty() || !t->sameItem(st) || t->count >= lim) continue;
                int mv = lim - t->count < st.count ? lim - t->count : st.count;
                t->count = (uint8_t)(t->count + mv);
                st.count = (uint8_t)(st.count - mv);
            } else {
                if (!t->empty()) continue;
                *t = st;
                if (t->count > lim) t->count = (uint8_t)lim;
                st.count = (uint8_t)(st.count - t->count);
            }
        }
    }
    if (!st.count) st.clear();
}

static void shiftClick(Server& s, Player& p, int slot) {
    ItemStack* src = slotRef(s, p, slot);
    if (!src || src->empty()) return;
    int cs = containerSize(p);
    if (isCraftResult(p, slot)) {
        // craft as many as fit
        for (int guard = 0; guard < 64; guard++) {
            ItemStack res = *src;
            if (res.empty()) break;
            int invFrom = p.winKind == WK_NONE ? SLOT_MAIN_START : cs;
            int invTo = p.winKind == WK_NONE ? SLOT_OFFHAND : cs + 36;
            // stop when the result no longer fits completely
            int room = 0;
            for (int i = invFrom; i < invTo; i++) {
                ItemStack* t = slotRef(s, p, i);
                if (!t) continue;
                if (t->empty()) room += maxStack(res.id);
                else if (t->sameItem(res)) room += maxStack(res.id) - t->count;
            }
            if (room < res.count) break;
            ItemStack tmp = res;
            moveInto(s, p, tmp, invFrom, invTo, true);
            consumeCraftGrid(p);
            updateCraftResult(p);
            if (src->empty() || src->id != res.id) break;
        }
        return;
    }
    ItemStack st = *src;
    src->clear();
    if (p.winKind == WK_NONE) {
        const ItemDef& d = ITEMS[st.id];
        if (slot >= SLOT_MAIN_START && d.kind == IK_ARMOR && p.inv[SLOT_ARMOR_START + d.armorSlot].empty()) {
            p.inv[SLOT_ARMOR_START + d.armorSlot] = st;
            st.clear();
        } else if (slot >= SLOT_HOTBAR_START && slot < SLOT_OFFHAND) {
            moveInto(s, p, st, SLOT_MAIN_START, SLOT_HOTBAR_START, false);
        } else if (slot >= SLOT_MAIN_START && slot < SLOT_HOTBAR_START) {
            moveInto(s, p, st, SLOT_HOTBAR_START, SLOT_OFFHAND, false);
        } else {
            moveInto(s, p, st, SLOT_MAIN_START, SLOT_OFFHAND, false);
        }
    } else if (slot < cs) {
        moveInto(s, p, st, cs, cs + 36, true);
    } else {
        if (p.winKind == WK_FURNACE) {
            if (smeltResult(st.id)) moveInto(s, p, st, 0, 1, false);
            if (!st.empty() && fuelTicks(st.id)) moveInto(s, p, st, 1, 2, false);
        } else if (p.winKind == WK_CRAFTING) {
            moveInto(s, p, st, 1, 10, false);
        } else {
            moveInto(s, p, st, 0, cs, false);
        }
        if (!st.empty()) {
            // move between main inventory and hotbar
            int rel = slot - cs;
            if (rel >= 27) moveInto(s, p, st, cs, cs + 27, false);
            else moveInto(s, p, st, cs + 27, cs + 36, false);
        }
    }
    if (!st.empty()) {
        // put back what did not move
        if (src->empty()) *src = st;
        else moveInto(s, p, st, 0, p.winKind == WK_NONE ? INV_SIZE : cs + 36, false);
    }
}

static void takeResult(Server& s, Player& p, int slot) {
    ItemStack* res = slotRef(s, p, slot);
    if (!res || res->empty()) return;
    if (p.cursor.empty()) {
        p.cursor = *res;
    } else if (p.cursor.sameItem(*res) && p.cursor.count + res->count <= maxStack(res->id)) {
        p.cursor.count = (uint8_t)(p.cursor.count + res->count);
    } else {
        return;
    }
    if (isCraftResult(p, slot)) {
        consumeCraftGrid(p);
        updateCraftResult(p);
    } else {
        res->clear();  // furnace output
    }
}

void Player::onWindowClick(Reader& r) {
    int8_t windowId = (int8_t)r.u8();
    int16_t slot = r.i16();
    int8_t button = r.i8();
    int16_t action = r.i16();
    int8_t mode = r.i8();
    ItemStack clicked;
    readSlot(r, clicked);
    if (!r.ok()) return;
    Server& s = *srv;
    if (dead || windowId != (winKind == WK_NONE ? 0 : winId)) {
        confirm(*this, windowId, action, false);
        return;
    }
    int total = winKind == WK_NONE ? INV_SIZE : containerSize(*this) + 36;
    bool touchesContainer = false;
    auto slotValid = [&](int i) { return i >= 0 && i < total; };

    switch (mode) {
        case 0: {  // click
            if (slot == -999) {
                if (!cursor.empty()) {
                    ItemStack d = cursor;
                    if (button == 1) { d.count = 1; cursor.count--; if (!cursor.count) cursor.clear(); }
                    else cursor.clear();
                    s.throwItem(*this, d);
                }
                break;
            }
            if (!slotValid(slot)) break;
            if (isResultSlot(*this, slot)) { takeResult(s, *this, slot); touchesContainer = true; break; }
            ItemStack* t = slotRef(s, *this, slot);
            if (!t) break;
            touchesContainer = winKind != WK_NONE && slot < containerSize(*this);
            if (button == 0) {
                if (cursor.empty()) { cursor = *t; t->clear(); }
                else if (t->empty()) {
                    if (!slotAccepts(*this, slot, cursor)) break;
                    int lim = slotLimit(*this, slot, cursor);
                    *t = cursor;
                    if (t->count > lim) { t->count = (uint8_t)lim; cursor.count = (uint8_t)(cursor.count - lim); }
                    else cursor.clear();
                } else if (t->sameItem(cursor)) {
                    int lim = slotLimit(*this, slot, cursor);
                    int mv = lim - t->count < cursor.count ? lim - t->count : cursor.count;
                    if (mv > 0) { t->count = (uint8_t)(t->count + mv); cursor.count = (uint8_t)(cursor.count - mv); }
                    if (!cursor.count) cursor.clear();
                } else if (slotAccepts(*this, slot, cursor) && cursor.count <= slotLimit(*this, slot, cursor)) {
                    ItemStack tmp = *t; *t = cursor; cursor = tmp;
                }
            } else {
                if (cursor.empty()) {
                    if (t->empty()) break;
                    int take = (t->count + 1) / 2;
                    cursor = *t;
                    cursor.count = (uint8_t)take;
                    t->count = (uint8_t)(t->count - take);
                    if (!t->count) t->clear();
                } else if (t->empty() || (t->sameItem(cursor) && t->count < slotLimit(*this, slot, cursor))) {
                    if (!slotAccepts(*this, slot, cursor)) break;
                    if (t->empty()) { *t = cursor; t->count = 1; }
                    else t->count++;
                    if (--cursor.count == 0) cursor.clear();
                } else if (slotAccepts(*this, slot, cursor) && cursor.count <= slotLimit(*this, slot, cursor)) {
                    ItemStack tmp = *t; *t = cursor; cursor = tmp;
                }
            }
            break;
        }
        case 1:  // shift click
            if (!slotValid(slot)) break;
            touchesContainer = winKind != WK_NONE;
            shiftClick(s, *this, slot);
            break;
        case 2: {  // number key: swap with hotbar slot `button` (40 = offhand)
            if (!slotValid(slot)) break;
            int hb = button == 40 ? SLOT_OFFHAND : SLOT_HOTBAR_START + button;
            if (button != 40 && (button < 0 || button > 8)) break;
            ItemStack* t = slotRef(s, *this, slot);
            if (!t) break;
            touchesContainer = winKind != WK_NONE && slot < containerSize(*this);
            if (isResultSlot(*this, slot)) {
                if (!inv[hb].empty()) break;
                ItemStack res = *t;
                if (res.empty()) break;
                inv[hb] = res;
                if (isCraftResult(*this, slot)) { consumeCraftGrid(*this); updateCraftResult(*this); }
                else t->clear();
                break;
            }
            ItemStack tmp = *t;
            if (!inv[hb].empty() && !slotAccepts(*this, slot, inv[hb])) break;
            *t = inv[hb];
            inv[hb] = tmp;
            break;
        }
        case 3:  // middle click (creative clone)
            if (gamemode == GM_CREATIVE && cursor.empty() && slotValid(slot)) {
                ItemStack* t = slotRef(s, *this, slot);
                if (t && !t->empty()) { cursor = *t; cursor.count = (uint8_t)maxStack(t->id); }
            }
            break;
        case 4: {  // drop (Q)
            if (!slotValid(slot) || !cursor.empty()) break;
            if (isResultSlot(*this, slot)) {
                ItemStack* t = slotRef(s, *this, slot);
                if (!t || t->empty()) break;
                ItemStack d = *t;
                if (isCraftResult(*this, slot)) { consumeCraftGrid(*this); updateCraftResult(*this); }
                else t->clear();
                s.throwItem(*this, d);
                break;
            }
            ItemStack* t = slotRef(s, *this, slot);
            if (!t || t->empty()) break;
            touchesContainer = winKind != WK_NONE && slot < containerSize(*this);
            ItemStack d = *t;
            if (button == 0) { d.count = 1; if (--t->count == 0) t->clear(); }
            else t->clear();
            s.throwItem(*this, d);
            break;
        }
        case 5: {  // drag
            int phase = button & 3;          // 0 start, 1 add, 2 end
            int kind = button >> 2;          // 0 left, 1 right, 2 middle
            if (phase == 0) { dragMode = (int8_t)kind; dragCount = 0; }
            else if (phase == 1) {
                if (dragMode == kind && slotValid(slot) && dragCount < 64 && !isResultSlot(*this, slot)) dragSlots[dragCount++] = (uint8_t)slot;
            } else {
                if (dragMode != kind || cursor.empty() || dragCount == 0) { dragMode = -1; break; }
                if (kind == 2 && gamemode != GM_CREATIVE) { dragMode = -1; break; }
                int per = kind == 0 ? cursor.count / dragCount : 1;
                if (kind == 2) per = maxStack(cursor.id);
                if (per < 1) per = 1;
                for (int k = 0; k < dragCount && (kind == 2 || cursor.count > 0); k++) {
                    int sl = dragSlots[k];
                    ItemStack* t = slotRef(s, *this, sl);
                    if (!t || !slotAccepts(*this, sl, cursor)) continue;
                    if (!t->empty() && !t->sameItem(cursor)) continue;
                    int lim = slotLimit(*this, sl, cursor);
                    int have = t->empty() ? 0 : t->count;
                    int add = per;
                    if (have + add > lim) add = lim - have;
                    if (kind != 2 && add > cursor.count) add = cursor.count;
                    if (add <= 0) continue;
                    if (t->empty()) { *t = cursor; t->count = 0; }
                    t->count = (uint8_t)(t->count + add);
                    if (kind != 2) cursor.count = (uint8_t)(cursor.count - add);
                    if (winKind != WK_NONE && sl < containerSize(*this)) touchesContainer = true;
                }
                if (!cursor.count) cursor.clear();
                dragMode = -1;
            }
            break;
        }
        case 6: {  // double click: collect matching items
            if (cursor.empty()) break;
            int lim = maxStack(cursor.id);
            for (int pass = 0; pass < 2 && cursor.count < lim; pass++)
                for (int i = 0; i < total && cursor.count < lim; i++) {
                    if (isResultSlot(*this, i)) continue;
                    ItemStack* t = slotRef(s, *this, i);
                    if (!t || t->empty() || !t->sameItem(cursor)) continue;
                    if (pass == 0 && t->count == maxStack(t->id)) continue;  // full stacks last
                    int mv = lim - cursor.count < t->count ? lim - cursor.count : t->count;
                    cursor.count = (uint8_t)(cursor.count + mv);
                    t->count = (uint8_t)(t->count - mv);
                    if (!t->count) t->clear();
                    if (winKind != WK_NONE && i < containerSize(*this)) touchesContainer = true;
                }
            break;
        }
        default: break;
    }
    // crafting grids may have changed
    updateCraftResult(*this);
    confirm(*this, windowId, action, true);
    // authoritative resync of the whole window (cheap and avoids prediction drift)
    sendWindow(s, *this);
    if (winKind != WK_NONE && winKind != WK_CRAFTING && touchesContainer) {
        s.containerChanged(winX, winY, winZ);
        if (winKind == WK_LARGE_CHEST) s.containerChanged(winX2, winY, winZ2);
    }
    s.broadcastEquipment(*this);
}

void Player::onCreativeSlot(Reader& r) {
    int16_t slot = r.i16();
    ItemStack st;
    readSlot(r, st);
    if (!r.ok() || gamemode != GM_CREATIVE) return;
    if (slot == -1) {
        if (!st.empty()) srv->throwItem(*this, st);
        return;
    }
    if (slot < 1 || slot >= INV_SIZE) return;
    inv[slot] = st;
    if (slot >= SLOT_CRAFT_START && slot < SLOT_CRAFT_START + 4) updateCraftResult(*this);
    if (slot == SLOT_HOTBAR_START + held || slot == SLOT_OFFHAND || (slot >= SLOT_ARMOR_START && slot < SLOT_ARMOR_START + 4))
        srv->broadcastEquipment(*this);
}

void Player::onPickItem(Reader& r) {
    int slot = r.varint();
    if (!r.ok() || slot < 0 || slot > 35) return;
    // protocol slot numbers: 0-8 hotbar, 9-35 main
    int inv1 = slot < 9 ? SLOT_HOTBAR_START + slot : slot;
    int hb = SLOT_HOTBAR_START + held;
    ItemStack t = inv[inv1];
    inv[inv1] = inv[hb];
    inv[hb] = t;
    sendSlot(inv1);
    sendSlot(hb);
    srv->broadcastEquipment(*this);
}

}  // namespace mc
