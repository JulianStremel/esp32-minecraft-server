// What enchantments do, after vanilla 1.21's data pack (data/minecraft/enchantment: each
// enchantment's effects) and EnchantmentHelper: which items take them and which go
// together; damage and knockback in melee, protection, thorns, durability (unbreaking,
// mending), loot (looting, fortune, silk touch), breathing, burning, and the attributes
// the client moves and digs by (efficiency, aqua affinity, depth strider, swift sneak,
// respiration). Bows (power, punch, flame, infinity) are in blocks.cpp where they shoot,
// the curses in survival.cpp (vanishing) and inventory.cpp (binding).
#include <math.h>
#include "mc/registry.h"
#include "mc/server/mob_util.h"
#include "mc/server/server.h"

namespace mc {

const EnchantableItem* enchantableItem(uint16_t item) {
    int lo = 0, hi = NUM_ENCHANTABLE_ITEMS - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (ENCHANTABLE_ITEMS[mid].item == item) return &ENCHANTABLE_ITEMS[mid];
        if (ENCHANTABLE_ITEMS[mid].item < item) lo = mid + 1;
        else hi = mid - 1;
    }
    return nullptr;
}

// Enchantment#canEnchant (supported_items): the anvil and /enchant; a book takes any.
bool enchantFits(int e, uint16_t item) {
    if (e < 0 || e >= ench::COUNT) return false;
    if (item == itm::Book || item == itm::EnchantedBook) return true;
    const EnchantableItem* it = enchantableItem(item);
    return it && (it->tags & ENCHANTS[e].supported);
}

// Enchantment#isPrimaryItem (primary_items, else supported_items): the enchanting table.
bool enchantPrimary(int e, uint16_t item) {
    if (e < 0 || e >= ench::COUNT) return false;
    const EnchantableItem* it = enchantableItem(item);
    return it && (it->tags & ENCHANTS[e].primary);
}

// Enchantment#areCompatible: different, and neither in the other's exclusive set.
bool enchantsCompatible(int a, int b) {
    if (a == b) return false;
    return !(ENCHANTS[a].exclusive & (1ull << b)) && !(ENCHANTS[b].exclusive & (1ull << a));
}

bool isArthropod(uint16_t type) {
    return type == ent::Spider || type == ent::CaveSpider || type == ent::Bee || type == ent::Silverfish || type == ent::Endermite;
}

// minecraft:damage effects of the weapon against a target: sharpness everywhere, smite
// on the undead, bane of arthropods on arthropods.
float enchantAttackBonus(const ItemStack& weapon, const Entity& target) {
    if (weapon.tagSize() <= 3) return 0;
    uint8_t lvl[ench::COUNT];
    itemEnchants(weapon, lvl);
    float bonus = 0;
    if (lvl[ench::Sharpness]) bonus += 0.5f * lvl[ench::Sharpness] + 0.5f;
    bool mob = target.kind == EK_MOB;
    if (lvl[ench::Smite] && mob && isUndead(target.type)) bonus += 2.5f * lvl[ench::Smite];
    if (lvl[ench::BaneOfArthropods] && mob && isArthropod(target.type)) bonus += 2.5f * lvl[ench::BaneOfArthropods];
    return bonus;
}

// minecraft:item_damage (unbreaking): each point of damage is taken away with a chance:
// level / (level + 1) for tools, 40% of that for armour.
int unbrokenDamage(const ItemStack& it, int amount, bool armor) {
    int l = itemEnchant(it, ench::Unbreaking);
    if (!l || amount <= 0) return amount;
    float skip = armor ? 0.4f * l / (l + 1) : (float)l / (l + 1);
    int left = 0;
    for (int i = 0; i < amount; i++)
        if (!mobs::rng().chance(skip)) left++;
    return left;
}

uint16_t silkTouchItem(uint16_t blockId) {
    int lo = 0, hi = NUM_SILK_TOUCH - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (SILK_TOUCH[mid][0] == blockId) return SILK_TOUCH[mid][1];
        if (SILK_TOUCH[mid][0] < blockId) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0;
}

// ApplyBonusCount's formulas: ore_drops (count times 1 to level + 1, more often 1) and
// uniform_bonus_count (0 to multiplier * level more).
int fortuneOreDrops(int count, int level) {
    if (level <= 0) return count;
    int i = mobs::rng().range(level + 2) - 1;
    return count * ((i < 0 ? 0 : i) + 1);
}
int fortuneUniform(int count, int level, int multiplier) {
    return level <= 0 ? count : count + mobs::rng().range(multiplier * level + 1);
}

// --------------------------------------------------------------- on a player
// The item in each equipment slot an enchantment can act from (EquipmentSlotGroup).
static const ItemStack* slotItem(const Player& p, int bit) {
    switch (bit) {
        case ES_MAINHAND: return &p.inv[SLOT_HOTBAR_START + p.held];
        case ES_OFFHAND: return &p.inv[SLOT_OFFHAND];
        case ES_HEAD: return &p.inv[SLOT_ARMOR_START];
        case ES_CHEST: return &p.inv[SLOT_ARMOR_START + 1];
        case ES_LEGS: return &p.inv[SLOT_ARMOR_START + 2];
        case ES_FEET: return &p.inv[SLOT_ARMOR_START + 3];
        default: return nullptr;
    }
}

// EnchantmentHelper#getEnchantmentLevel: the highest level among the slots it acts from.
int Server::enchantOn(const Player& p, int id) const {
    int best = 0;
    for (int bit = 1; bit < ES_BODY; bit <<= 1) {
        if (!(ENCHANTS[id].slots & bit)) continue;
        const ItemStack* it = slotItem(p, bit);
        if (it && !it->empty()) {
            int l = itemEnchant(*it, id);
            if (l > best) best = l;
        }
    }
    return best;
}

// EnchantmentHelper#getDamageProtection: protection 1 a level against everything but the
// void and /kill; fire, blast and projectile protection 2 a level against theirs; feather
// falling 3 a level against falls; summed over the armour, then at most 20 (4% each).
float Server::enchantProtection(const Player& p, uint8_t cause) const {
    if (cause == DC_VOID || cause == DC_KILL) return 1;
    bool fire = cause == DC_FIRE || cause == DC_LAVA || cause == DC_FIREBALL;
    bool projectile = cause == DC_ARROW || cause == DC_FIREBALL;
    int epf = 0;
    for (int i = SLOT_ARMOR_START; i < SLOT_ARMOR_START + 4; i++) {
        const ItemStack& a = p.inv[i];
        if (a.empty() || a.tagSize() <= 3) continue;
        uint8_t lvl[ench::COUNT];
        itemEnchants(a, lvl);
        epf += lvl[ench::Protection];
        if (fire) epf += 2 * lvl[ench::FireProtection];
        if (cause == DC_EXPLOSION) epf += 2 * lvl[ench::BlastProtection];
        if (projectile) epf += 2 * lvl[ench::ProjectileProtection];
        if (cause == DC_FALL && i == SLOT_ARMOR_START + 3) epf += 3 * lvl[ench::FeatherFalling];
    }
    if (epf > 20) epf = 20;
    return 1.0f - epf / 25.0f;
}

// Thorns on the victim's armour: 15% a level, 1 to 5 damage to the attacker and 2 more
// wear on that piece.
void Server::thorns(Player& victim, int32_t attacker) {
    if (attacker < 0 || attacker == victim.e.id) return;
    for (int i = SLOT_ARMOR_START; i < SLOT_ARMOR_START + 4; i++) {
        ItemStack& a = victim.inv[i];
        int l = a.empty() ? 0 : itemEnchant(a, ench::Thorns);
        if (!l || !mobs::rng().chance(0.15f * l)) continue;
        Entity* att = findEntity(attacker);
        if (!att) {
            Player* pl = playerByEntity(attacker);
            att = pl ? &pl->e : nullptr;
        }
        if (!att || att->dim != victim.e.dim) return;
        damageEntity(*att, 1.0f + mobs::rng().unit() * 4.0f, DC_THORNS, victim.e.id);
        playSound("enchant.thorns.hit", victim.e.x, victim.e.y, victim.e.z, 1, 1, 7);
        if (ITEMS[a.id].durability && victim.isSurvivalLike()) {
            a.damage = (uint16_t)(a.damage + unbrokenDamage(a, 2, true));
            if (a.damage >= ITEMS[a.id].durability) {
                a.clear();
                playSound("entity.item.break", victim.e.x, victim.e.y, victim.e.z, 1, 1, 7);
            }
            victim.sendSlot(i);
            broadcastEquipment(victim);
        }
    }
}

// Mending (minecraft:repair_with_xp, 2 durability a point): experience picked up repairs
// a random damaged mending item the player wears or holds first; what is left is theirs.
void Server::mendItems(Player& p, int& xp) {
    while (xp > 0) {
        static const int SLOTS[6] = {-1, SLOT_OFFHAND, SLOT_ARMOR_START, SLOT_ARMOR_START + 1, SLOT_ARMOR_START + 2,
                                     SLOT_ARMOR_START + 3};
        int found[6], n = 0;
        for (int k = 0; k < 6; k++) {
            int slot = SLOTS[k] < 0 ? SLOT_HOTBAR_START + p.held : SLOTS[k];
            const ItemStack& it = p.inv[slot];
            if (!it.empty() && it.damage > 0 && itemEnchant(it, ench::Mending)) found[n++] = slot;
        }
        if (!n) return;
        int slot = found[mobs::rng().range(n)];
        ItemStack& it = p.inv[slot];
        int fix = xp * 2 < it.damage ? xp * 2 : it.damage;
        it.damage = (uint16_t)(it.damage - fix);
        p.sendSlot(slot);
        xp -= fix / 2;   // (vanilla's integer arithmetic: an odd point of repair is free)
    }
}

// The burning_time attribute (fire protection: 15% shorter a level, summed), rounded up
// as LivingEntity#setRemainingFireTicks does.
int Server::burnTicks(const Player& p, int ticks) const {
    int l = 0;
    for (int i = SLOT_ARMOR_START; i < SLOT_ARMOR_START + 4; i++)
        if (!p.inv[i].empty()) l += itemEnchant(p.inv[i], ench::FireProtection);
    if (!l) return ticks;
    double f = 1.0 - 0.15 * l;
    return f <= 0 ? 0 : (int)ceil(ticks * f - 1e-9);
}

// The attributes the client digs, swims, sneaks and holds its breath by, packed (4 bits
// each): efficiency (held), aqua affinity, respiration (head), swift sneak (legs), depth
// strider (feet). Changed: they are sent again (sendAttributes).
uint32_t Server::enchantAttributes(const Player& p) const {
    auto lv = [&](int slot, int id) {
        const ItemStack& it = p.inv[slot];
        int l = it.empty() || it.tagSize() <= 3 ? 0 : itemEnchant(it, id);
        return (uint32_t)(l > 15 ? 15 : l);
    };
    return lv(SLOT_HOTBAR_START + p.held, ench::Efficiency) | lv(SLOT_ARMOR_START, ench::AquaAffinity) << 4 |
           lv(SLOT_ARMOR_START, ench::Respiration) << 8 | lv(SLOT_ARMOR_START + 2, ench::SwiftSneak) << 12 |
           lv(SLOT_ARMOR_START + 3, ench::DepthStrider) << 16;
}

void Server::equipmentChanged(Player& p) {
    if (enchantAttributes(p) != p.enchAttrs) sendAttributes(p);
}

}  // namespace mc
