// The enchanting table, the anvil and the grindstone, after vanilla 1.21's menus
// (EnchantmentMenu, EnchantmentHelper#selectEnchantment, AnvilMenu#createResult,
// GrindstoneMenu). Their slots are the player's craft[] (0 and 1 the inputs, 2 an anvil's
// or grindstone's result); the table's offers come from the player's enchantment seed
// with Java's random, as vanilla's, so a seed shows the offers vanilla would.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "mc/registry.h"
#include "mc/server/mob_util.h"
#include "mc/server/server.h"
#include "mc/text.h"
#include "mc/world/vanilla/java_random.h"

namespace mc {

using vanilla::JavaRandom;

namespace {

struct Pick { uint8_t ench, level; };

int enchantability(const ItemStack& it) {
    const EnchantableItem* e = enchantableItem(it.id);
    return e ? e->enchantability : 0;
}

bool hasEnchantments(const ItemStack& it, bool stored) {
    if (it.tagSize() <= 3) return false;
    uint8_t l[ench::COUNT];
    itemEnchants(it, l, stored);
    for (int i = 0; i < ench::COUNT; i++)
        if (l[i]) return true;
    return false;
}
bool anyEnchantments(const ItemStack& it) { return hasEnchantments(it, false) || hasEnchantments(it, true); }

// ItemStack#isEnchantable: enchantable, and not enchanted yet
bool tableTakes(const ItemStack& it) { return !it.empty() && enchantability(it) > 0 && !hasEnchantments(it, false); }

// EnchantmentHelper#getAvailableEnchantmentResults: the table's enchantments for the item
// (a book takes all), each at the highest level whose costs hold `level`.
int available(int level, const ItemStack& it, Pick* out) {
    bool book = it.id == itm::Book;
    int n = 0;
    for (int k = 0; k < NUM_TABLE_ENCHANTS; k++) {
        int e = TABLE_ENCHANTS[k];
        if (!book && !enchantPrimary(e, it.id)) continue;
        const EnchantDef& d = ENCHANTS[e];
        for (int l = d.maxLevel; l >= 1; l--) {
            if (level >= d.minCost(l) && level <= d.maxCost(l)) {
                out[n++] = {(uint8_t)e, (uint8_t)l};
                break;
            }
        }
    }
    return n;
}

// WeightedRandom#getRandomItem
int weighted(JavaRandom& r, const Pick* list, int n) {
    int total = 0;
    for (int i = 0; i < n; i++) total += ENCHANTS[list[i].ench].weight;
    if (total <= 0) return -1;
    int w = r.nextInt(total);
    for (int i = 0; i < n; i++) {
        w -= ENCHANTS[list[i].ench].weight;
        if (w < 0) return i;
    }
    return -1;
}

// EnchantmentHelper#selectEnchantment
int selectEnchantments(JavaRandom& r, const ItemStack& it, int level, Pick* out) {
    int value = enchantability(it);
    if (value <= 0) return 0;
    level += 1 + r.nextInt(value / 4 + 1) + r.nextInt(value / 4 + 1);
    float f = (r.nextFloat() + r.nextFloat() - 1.0f) * 0.15f;
    level = (int)floorf((float)level + (float)level * f + 0.5f);
    if (level < 1) level = 1;
    Pick list[ench::COUNT];
    int n = available(level, it, list), picked = 0;
    if (!n) return 0;
    int i = weighted(r, list, n);
    if (i >= 0) out[picked++] = list[i];
    while (r.nextInt(50) <= level) {
        if (picked) {   // filterCompatibleEnchantments: those that go with the last pick
            int k = 0;
            for (int j = 0; j < n; j++)
                if (enchantsCompatible(out[picked - 1].ench, list[j].ench)) list[k++] = list[j];
            n = k;
        }
        if (!n) break;
        i = weighted(r, list, n);
        if (i >= 0 && picked < ench::COUNT) out[picked++] = list[i];
        level /= 2;
    }
    return picked;
}

// EnchantmentHelper#getEnchantmentCost
int enchantmentCost(JavaRandom& r, int slot, int shelves, const ItemStack& it) {
    if (enchantability(it) <= 0) return 0;
    if (shelves > 15) shelves = 15;
    int j = r.nextInt(8) + 1 + (shelves >> 1) + r.nextInt(shelves + 1);
    if (slot == 0) return j / 3 > 1 ? j / 3 : 1;
    return slot == 1 ? j * 2 / 3 + 1 : (j > shelves * 2 ? j : shelves * 2);
}

// EnchantmentMenu#getEnchantmentList: re-seeded with the seed + the offer; a book gets one
// enchantment fewer
int offerList(JavaRandom& r, int32_t seed, const ItemStack& it, int slot, int cost, Pick* out) {
    r.setSeed((int64_t)(int32_t)((uint32_t)seed + (uint32_t)slot));   // (Java: int addition, then long)
    int n = selectEnchantments(r, it, cost, out);
    if (it.id == itm::Book && n > 1) {
        int drop = r.nextInt(n);
        for (int i = drop; i < n - 1; i++) out[i] = out[i + 1];
        n--;
    }
    return n;
}

void setProperty(Player& p, int id, int value) {
    Packet pk(pkt::s2c::CraftProgressBar);
    pk.w.varint(p.winId);
    pk.w.i16((int16_t)id);
    pk.w.i16((int16_t)value);
    p.conn.send(pk);
}

void levelEvent(Server& s, int type, int x, int y, int z) {
    Packet pk(pkt::s2c::WorldEvent);
    pk.w.i32(type);
    pk.w.u64(packPos(x, y, z));
    pk.w.i32(0);
    pk.w.boolean(false);
    s.broadcastNear(pk, x >> 4, z >> 4, nullptr);
}

// Player#giveExperienceLevels with a negative count
void takeLevels(Player& p, int levels) {
    p.xpLevel -= levels;
    if (p.xpLevel < 0) {
        p.xpLevel = 0;
        p.xpProgress = 0;
        p.xpTotal = 0;
    }
    p.sendXp();
}

}  // namespace

// EnchantingTableBlock#isValidBookShelf: bookshelves two blocks away (on the table's level
// and one up), with the block between them and the table air or replaceable.
int Server::enchantingShelves(int x, int y, int z) {
    int n = 0;
    for (int dy = 0; dy <= 1; dy++)
        for (int dx = -2; dx <= 2; dx++)
            for (int dz = -2; dz <= 2; dz++) {
                if (dx != -2 && dx != 2 && dz != -2 && dz != 2) continue;
                if (blockIdOf(blockAt(x + dx, y + dy, z + dz)) != blk::Bookshelf) continue;
                uint16_t between = blockAt(x + dx / 2, y + dy, z + dz / 2);   // (Java's division: towards 0)
                if (stateIsAir(between) || (blockOf(between).flags & BF_REPLACEABLE)) n++;
            }
    return n;
}

void Server::openEnchanting(Player& p, int x, int y, int z) {
    closeWindow(p, true);
    p.winX = x; p.winY = y; p.winZ = z;
    for (auto& c : p.craft) c.clear();
    openWorkstation(p, WK_ENCHANTING, menu::Enchantment, "container.enchant");
    workstationChanged(p);
}

void Server::openAnvil(Player& p, int x, int y, int z) {
    closeWindow(p, true);
    p.winX = x; p.winY = y; p.winZ = z;
    for (auto& c : p.craft) c.clear();
    p.anvilName[0] = 0;
    openWorkstation(p, WK_ANVIL, menu::Anvil, "container.repair");
    workstationChanged(p);
}

void Server::openGrindstone(Player& p, int x, int y, int z) {
    closeWindow(p, true);
    p.winX = x; p.winY = y; p.winZ = z;
    for (auto& c : p.craft) c.clear();
    openWorkstation(p, WK_GRINDSTONE, menu::Grindstone, "container.grindstone_title");
    workstationChanged(p);
}

// ------------------------------------------------------------------ the enchanting table
// EnchantmentMenu#slotsChanged: three offers for the item, from the seed and the shelves.
static void tableOffers(Server& s, Player& p) {
    const ItemStack& it = p.craft[0];
    for (int k = 0; k < 3; k++) {
        p.tableCost[k] = 0;
        p.tableClue[k] = p.tableLevel[k] = -1;
    }
    if (tableTakes(it)) {
        int shelves = s.enchantingShelves(p.winX, p.winY, p.winZ);
        JavaRandom r(p.enchantSeed);
        for (int k = 0; k < 3; k++) {
            int c = enchantmentCost(r, k, shelves, it);
            p.tableCost[k] = (int16_t)(c < k + 1 ? 0 : c);
        }
        for (int k = 0; k < 3; k++) {
            if (p.tableCost[k] <= 0) continue;
            Pick list[ench::COUNT];
            int n = offerList(r, p.enchantSeed, it, k, p.tableCost[k], list);
            if (!n) continue;
            const Pick& shown = list[r.nextInt(n)];
            p.tableClue[k] = (int8_t)shown.ench;
            p.tableLevel[k] = (int8_t)shown.level;
        }
    }
    for (int k = 0; k < 3; k++) setProperty(p, k, p.tableCost[k]);
    setProperty(p, 3, p.enchantSeed & -16);
    for (int k = 0; k < 3; k++) setProperty(p, 4 + k, p.tableClue[k]);
    for (int k = 0; k < 3; k++) setProperty(p, 7 + k, p.tableLevel[k]);
}

// EnchantmentMenu#clickMenuButton: offer `button` (0 to 2) for button + 1 lapis and levels.
void Server::enchantButton(Player& p, int button) {
    if (p.winKind != WK_ENCHANTING || button < 0 || button > 2) return;
    ItemStack& it = p.craft[0];
    ItemStack& lapis = p.craft[1];
    int need = button + 1;
    bool creative = p.gamemode == GM_CREATIVE;
    if ((lapis.empty() || lapis.id != itm::LapisLazuli || lapis.count < need) && !creative) return;
    int cost = p.tableCost[button];
    if (cost <= 0 || it.empty() || (!creative && (p.xpLevel < need || p.xpLevel < cost))) return;
    Pick list[ench::COUNT];
    JavaRandom r;
    int n = offerList(r, p.enchantSeed, it, button, cost, list);
    if (!n) return;
    // Player#onEnchantmentPerformed: the levels, a new seed
    if (!creative) takeLevels(p, need);
    p.enchantSeed = (int32_t)mobs::rng().u32();
    bool book = it.id == itm::Book;
    ItemStack out = book ? ItemStack::of(itm::EnchantedBook) : it;
    uint8_t lvl[ench::COUNT];
    itemEnchants(out, lvl, book);
    for (int i = 0; i < n; i++) lvl[list[i].ench] = list[i].level;
    setItemEnchants(out, lvl, book);
    it = out;
    if (!creative) {
        lapis.count = (uint8_t)(lapis.count - need);
        if (!lapis.count) lapis.clear();
    }
    playSound("block.enchantment_table.use", p.winX + 0.5, p.winY + 0.5, p.winZ + 0.5, 1, 0.9f + mobs::rng().unit() * 0.1f, 7);
    workstationChanged(p);
    resendWindow(p);
}

// ------------------------------------------------------------------ the anvil
// What repairs an item (its repairable component): its material.
static bool repairsWith(uint16_t item, uint16_t material) {
    const char* n = ITEMS[item].name;
    auto made = [&](const char* prefix) { return !strncmp(n, prefix, strlen(prefix)); };
    if (made("wooden_") || item == itm::Shield) return strstr(ITEMS[material].name, "_planks") != nullptr;
    if (made("stone_"))
        return material == itm::Cobblestone || material == itm::CobbledDeepslate || material == itm::Blackstone;
    if (made("iron_") || made("chainmail_")) return material == itm::IronIngot;
    if (made("golden_")) return material == itm::GoldIngot;
    if (made("diamond_")) return material == itm::Diamond;
    if (made("netherite_")) return material == itm::NetheriteIngot;
    if (made("leather_")) return material == itm::Leather;
    if (item == itm::TurtleHelmet) return material == itm::TurtleScute;
    if (item == itm::Elytra) return material == itm::PhantomMembrane;
    if (item == itm::Mace) return material == itm::BreezeRod;
    if (item == itm::WolfArmor) return material == itm::ArmadilloScute;
    return false;
}

static int increasedRepairCost(int c) { return (int)fminf(c * 2.0f + 1.0f, 2147483647.0f); }

// AnvilMenu#createResult: the result, its cost (in levels) and the materials it uses.
static void anvilResult(Player& p) {
    const ItemStack& a = p.craft[0];
    const ItemStack& b = p.craft[1];
    ItemStack& out = p.craft[2];
    out.clear();
    p.anvilCost = 0;
    p.anvilMaterials = 0;
    p.anvilRenameOnly = false;
    if (a.empty()) return;
    bool creative = p.gamemode == GM_CREATIVE;
    ItemStack res = a;
    bool aBook = a.id == itm::EnchantedBook;
    uint8_t lvl[ench::COUNT];
    itemEnchants(res, lvl, aBook);
    int cost = 0, rename = 0;
    long base = (long)itemRepairCost(a) + (b.empty() ? 0 : itemRepairCost(b));
    uint16_t maxDamage = ITEMS[a.id].durability;
    if (!b.empty()) {
        bool bBook = b.id == itm::EnchantedBook;
        if (maxDamage && repairsWith(a.id, b.id)) {   // the material: a quarter each
            int fix = res.damage < maxDamage / 4 ? res.damage : maxDamage / 4;
            if (fix <= 0) return;
            int used = 0;
            for (; fix > 0 && used < b.count; used++) {
                res.damage = (uint16_t)(res.damage - fix);
                cost++;
                fix = res.damage < maxDamage / 4 ? res.damage : maxDamage / 4;
            }
            p.anvilMaterials = (uint8_t)used;
        } else {
            if (!bBook && (b.id != a.id || !maxDamage)) return;
            if (maxDamage && !bBook) {   // two of a kind: both durabilities and 12% more
                int left = maxDamage - a.damage, other = ITEMS[b.id].durability - b.damage;
                int damage = maxDamage - (left + other + maxDamage * 12 / 100);
                if (damage < 0) damage = 0;
                if (damage < res.damage) {
                    res.damage = (uint16_t)damage;
                    cost += 2;
                }
            }
            uint8_t add[ench::COUNT];
            itemEnchants(b, add, bBook);
            bool any = false, rejected = false;
            for (int e = 0; e < ench::COUNT; e++) {
                if (!add[e]) continue;
                int have = lvl[e], l = add[e];
                l = have == l ? l + 1 : (l > have ? l : have);
                bool fits = enchantFits(e, a.id) && a.id != itm::Book;
                if (creative || aBook) fits = true;
                for (int o = 0; o < ench::COUNT; o++) {
                    if (lvl[o] && o != e && !enchantsCompatible(e, o)) {
                        fits = false;
                        cost++;
                    }
                }
                if (!fits) {
                    rejected = true;
                    continue;
                }
                any = true;
                if (l > ENCHANTS[e].maxLevel) l = ENCHANTS[e].maxLevel;
                lvl[e] = (uint8_t)l;
                int c = ENCHANTS[e].anvilCost;
                if (bBook) c = c / 2 > 1 ? c / 2 : 1;
                cost += c * l;
                if (a.count > 1) cost = 40;
            }
            if (rejected && !any) return;
        }
    }
    // the name: a new one, or the custom one taken off
    if (p.anvilName[0]) {
        char json[200];
        textJson(json, sizeof(json), p.anvilName, nullptr);
        bool same = false;
        if (hasCustomName(a)) {   // (the same custom name: nothing to pay)
            ItemStack named = a;
            setItemName(named, json);
            same = named.sameItem(a);
        } else {
            same = !strcmp(p.anvilName, ITEMS[a.id].name);
        }
        if (!same) {
            rename = 1;
            cost += 1;
            setItemName(res, json);
        }
    } else if (hasCustomName(a)) {
        rename = 1;
        cost += 1;
        setItemName(res, nullptr);
    }
    long total = cost <= 0 ? 0 : base + cost;
    p.anvilCost = total > 2147483647L ? 2147483647 : (int32_t)total;
    if (cost <= 0) return;
    if (rename == cost && rename > 0) {
        if (p.anvilCost >= 40) p.anvilCost = 39;
        p.anvilRenameOnly = true;
    }
    if (p.anvilCost >= 40 && !creative) return;   // too expensive
    int repair = itemRepairCost(res);
    if (!b.empty() && repair < itemRepairCost(b)) repair = itemRepairCost(b);
    if (rename != cost || rename == 0) repair = increasedRepairCost(repair);
    setItemRepairCost(res, repair);
    setItemEnchants(res, lvl, aBook);
    out = res;
}

void Server::anvilRename(Player& p, const char* name) {
    if (p.winKind != WK_ANVIL) return;
    // AnvilMenu#validateName: printable, at most 50 characters
    char clean[201];
    int n = 0, chars = 0;
    for (const char* q = name; *q && n < 200; q++) {
        unsigned char ch = (unsigned char)*q;
        if (ch < 0x20 || ch == 0x7F) continue;
        bool starts = (ch & 0xC0) != 0x80;   // the first byte of a character
        if (starts && chars == 50) break;
        if (starts && ch >= 0xC0 && n + 4 > 200) break;
        chars += starts;
        clean[n++] = (char)ch;
    }
    clean[n] = 0;
    bool blank = true;
    for (int i = 0; i < n; i++)
        if (clean[i] != ' ') blank = false;
    snprintf(p.anvilName, sizeof(p.anvilName), "%s", blank ? "" : clean);
    workstationChanged(p);
    resendWindow(p);
}

// ------------------------------------------------------------------ the grindstone
static int increasedForEach(int n) {
    int c = 0;
    for (int i = 0; i < n; i++) c = increasedRepairCost(c);
    return c;
}

// GrindstoneMenu#removeNonCursesFrom: the curses stay; a book without any is a book again.
static ItemStack withoutEnchantments(ItemStack it) {
    bool book = it.id == itm::EnchantedBook;
    uint8_t lvl[ench::COUNT];
    itemEnchants(it, lvl, book);
    int left = 0;
    for (int e = 0; e < ench::COUNT; e++) {
        if (!(ENCHANTS[e].flags & EF_CURSE)) lvl[e] = 0;
        left += lvl[e] != 0;
    }
    setItemEnchants(it, lvl, book);
    if (book && !left) {
        uint16_t damage = it.damage;
        ItemStack plain = it;
        uint8_t none[ench::COUNT] = {};
        setItemEnchants(plain, none, true);
        plain.id = itm::Book;
        plain.damage = damage;
        it = plain;
    }
    setItemRepairCost(it, increasedForEach(left));
    return it;
}

bool grindstoneTakes(const ItemStack& it) { return !it.empty() && (ITEMS[it.id].durability || anyEnchantments(it)); }

static ItemStack grindstoneResult(const ItemStack& a, const ItemStack& b) {
    if (a.empty() && b.empty()) return ItemStack();
    if (a.count > 1 || b.count > 1) return ItemStack();
    if (a.empty() || b.empty()) {
        const ItemStack& one = a.empty() ? b : a;
        return anyEnchantments(one) ? withoutEnchantments(one) : ItemStack();
    }
    // GrindstoneMenu#mergeItems: two of the same item, their durabilities and 5% more
    if (a.id != b.id) return ItemStack();
    int maxDamage = ITEMS[a.id].durability;
    ItemStack r = a;
    if (maxDamage) {
        int sum = (maxDamage - a.damage) + (maxDamage - b.damage) + maxDamage * 5 / 100;
        r.damage = (uint16_t)(maxDamage - sum > 0 ? maxDamage - sum : 0);
    } else {
        if (maxStack(a.id) < 2 || !a.sameItem(b)) return ItemStack();
        r.count = 2;
    }
    // mergeEnchantsFrom: b's (curses only if r has none of that one), the higher level
    bool book = r.id == itm::EnchantedBook;
    uint8_t lvl[ench::COUNT], add[ench::COUNT];
    itemEnchants(r, lvl, book);
    itemEnchants(b, add, b.id == itm::EnchantedBook);
    for (int e = 0; e < ench::COUNT; e++)
        if (add[e] && (!(ENCHANTS[e].flags & EF_CURSE) || !lvl[e]) && add[e] > lvl[e]) lvl[e] = add[e];
    setItemEnchants(r, lvl, book);
    return withoutEnchantments(r);
}

// GrindstoneMenu#getExperienceAmount: the minimum costs of the enchantments taken off.
static int grindstoneXp(const ItemStack& it) {
    int xp = 0;
    for (int stored = 0; stored < 2; stored++) {
        uint8_t lvl[ench::COUNT];
        itemEnchants(it, lvl, stored != 0);
        for (int e = 0; e < ench::COUNT; e++)
            if (lvl[e] && !(ENCHANTS[e].flags & EF_CURSE)) xp += ENCHANTS[e].minCost(lvl[e]);
    }
    return xp;
}

// ------------------------------------------------------------------ all three
void Server::workstationChanged(Player& p) {
    switch (p.winKind) {
        case WK_ENCHANTING: tableOffers(*this, p); break;
        case WK_ANVIL:
            anvilResult(p);
            setProperty(p, 0, p.anvilCost);
            break;
        case WK_GRINDSTONE: p.craft[2] = grindstoneResult(p.craft[0], p.craft[1]); break;
        default: break;
    }
}

// AnvilMenu#mayPickup: enough levels, and something to pay (the result shown at all)
bool Server::workstationMayTake(Player& p) {
    if (p.craft[2].empty()) return false;
    if (p.winKind == WK_ANVIL) return p.anvilCost > 0 && (p.gamemode == GM_CREATIVE || p.xpLevel >= p.anvilCost);
    return p.winKind == WK_GRINDSTONE;
}

// The result taken (AnvilMenu#onTake, GrindstoneMenu's result slot): the inputs used up.
void Server::workstationTaken(Player& p) {
    p.craft[2].clear();
    if (p.winKind == WK_ANVIL) {
        bool creative = p.gamemode == GM_CREATIVE;
        if (!creative) takeLevels(p, p.anvilCost);
        ItemStack& b = p.craft[1];
        if (p.anvilMaterials > 0) {
            if (!b.empty() && b.count > p.anvilMaterials) b.count = (uint8_t)(b.count - p.anvilMaterials);
            else b.clear();
        } else if (!p.anvilRenameOnly) {
            b.clear();
        }
        p.craft[0].clear();
        p.anvilCost = 0;
        // 12%: the anvil is worn (chipped, damaged, gone)
        uint16_t st = blockAt(p.winX, p.winY, p.winZ);
        uint16_t id = blockIdOf(st);
        bool anvil = id == blk::Anvil || id == blk::ChippedAnvil || id == blk::DamagedAnvil;
        if (!creative && anvil && mobs::rng().unit() < 0.12f) {
            if (id == blk::DamagedAnvil) {
                setBlock(p.winX, p.winY, p.winZ, 0);
                levelEvent(*this, 1029, p.winX, p.winY, p.winZ);   // destroyed
                closeWindow(p, true);
                return;
            }
            uint16_t worn = (uint16_t)((id == blk::Anvil ? bs::ChippedAnvil : bs::DamagedAnvil) +
                                       (st - BLOCKS[id].minState));   // the same facing
            setBlock(p.winX, p.winY, p.winZ, worn);
        }
        levelEvent(*this, 1030, p.winX, p.winY, p.winZ);   // used
    } else if (p.winKind == WK_GRINDSTONE) {
        int xp = grindstoneXp(p.craft[0]) + grindstoneXp(p.craft[1]);
        if (xp > 0) {
            int half = (xp + 1) / 2;
            giveXp(p, half + mobs::rng().range(half));
        }
        p.craft[0].clear();
        p.craft[1].clear();
        levelEvent(*this, 1042, p.winX, p.winY, p.winZ);
    }
    workstationChanged(p);
}

}  // namespace mc
