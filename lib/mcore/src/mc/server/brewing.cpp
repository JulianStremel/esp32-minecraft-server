// The brewing stand after vanilla 1.21 (BrewingStandBlockEntity, PotionBrewing): three
// bottles, an ingredient and blaze powder (20 brews each); a brew takes 20 s and turns
// every bottle that has a mix with the ingredient. Like furnaces, a stand is brought up to
// date when someone looks at or touches it and at its next event in the timer wheel (the
// brew done), so stands cost nothing in between. The tile keeps: items 0-2 bottles, 3 the
// ingredient, 4 fuel; burnTime the fuel left, cookTime the brew time left, burnTotal the
// ingredient the brew started with.
#include "mc/registry.h"
#include "mc/server/server.h"

namespace mc {

namespace {

struct Mix { uint8_t from; uint16_t ingredient; uint8_t to; };

// PotionBrewing#addVanillaMixes (addStartMix: water + X makes mundane, awkward + X the potion)
const Mix MIXES[] = {
    {potion::Water, itm::GlisteringMelonSlice, potion::Mundane}, {potion::Water, itm::GhastTear, potion::Mundane},
    {potion::Water, itm::RabbitFoot, potion::Mundane}, {potion::Water, itm::BlazePowder, potion::Mundane},
    {potion::Water, itm::SpiderEye, potion::Mundane}, {potion::Water, itm::Sugar, potion::Mundane},
    {potion::Water, itm::MagmaCream, potion::Mundane}, {potion::Water, itm::GlowstoneDust, potion::Thick},
    {potion::Water, itm::Redstone, potion::Mundane}, {potion::Water, itm::NetherWart, potion::Awkward},
    {potion::Water, itm::BreezeRod, potion::Mundane}, {potion::Water, itm::SlimeBlock, potion::Mundane},
    {potion::Water, itm::Stone, potion::Mundane}, {potion::Water, itm::Cobweb, potion::Mundane},
    {potion::Water, itm::FermentedSpiderEye, potion::Weakness},
    {potion::Awkward, itm::GoldenCarrot, potion::NightVision},
    {potion::NightVision, itm::Redstone, potion::LongNightVision},
    {potion::NightVision, itm::FermentedSpiderEye, potion::Invisibility},
    {potion::LongNightVision, itm::FermentedSpiderEye, potion::LongInvisibility},
    {potion::Invisibility, itm::Redstone, potion::LongInvisibility},
    {potion::Awkward, itm::MagmaCream, potion::FireResistance},
    {potion::FireResistance, itm::Redstone, potion::LongFireResistance},
    {potion::Awkward, itm::RabbitFoot, potion::Leaping},
    {potion::Leaping, itm::Redstone, potion::LongLeaping},
    {potion::Leaping, itm::GlowstoneDust, potion::StrongLeaping},
    {potion::Leaping, itm::FermentedSpiderEye, potion::Slowness},
    {potion::LongLeaping, itm::FermentedSpiderEye, potion::LongSlowness},
    {potion::Slowness, itm::Redstone, potion::LongSlowness},
    {potion::Slowness, itm::GlowstoneDust, potion::StrongSlowness},
    {potion::Awkward, itm::TurtleHelmet, potion::TurtleMaster},
    {potion::TurtleMaster, itm::Redstone, potion::LongTurtleMaster},
    {potion::TurtleMaster, itm::GlowstoneDust, potion::StrongTurtleMaster},
    {potion::Swiftness, itm::FermentedSpiderEye, potion::Slowness},
    {potion::LongSwiftness, itm::FermentedSpiderEye, potion::LongSlowness},
    {potion::Awkward, itm::Sugar, potion::Swiftness},
    {potion::Swiftness, itm::Redstone, potion::LongSwiftness},
    {potion::Swiftness, itm::GlowstoneDust, potion::StrongSwiftness},
    {potion::Awkward, itm::Pufferfish, potion::WaterBreathing},
    {potion::WaterBreathing, itm::Redstone, potion::LongWaterBreathing},
    {potion::Awkward, itm::GlisteringMelonSlice, potion::Healing},
    {potion::Healing, itm::GlowstoneDust, potion::StrongHealing},
    {potion::Healing, itm::FermentedSpiderEye, potion::Harming},
    {potion::StrongHealing, itm::FermentedSpiderEye, potion::StrongHarming},
    {potion::Harming, itm::GlowstoneDust, potion::StrongHarming},
    {potion::Poison, itm::FermentedSpiderEye, potion::Harming},
    {potion::LongPoison, itm::FermentedSpiderEye, potion::Harming},
    {potion::StrongPoison, itm::FermentedSpiderEye, potion::StrongHarming},
    {potion::Awkward, itm::SpiderEye, potion::Poison},
    {potion::Poison, itm::Redstone, potion::LongPoison},
    {potion::Poison, itm::GlowstoneDust, potion::StrongPoison},
    {potion::Awkward, itm::GhastTear, potion::Regeneration},
    {potion::Regeneration, itm::Redstone, potion::LongRegeneration},
    {potion::Regeneration, itm::GlowstoneDust, potion::StrongRegeneration},
    {potion::Awkward, itm::BlazePowder, potion::Strength},
    {potion::Strength, itm::Redstone, potion::LongStrength},
    {potion::Strength, itm::GlowstoneDust, potion::StrongStrength},
    {potion::Weakness, itm::Redstone, potion::LongWeakness},
    {potion::Awkward, itm::PhantomMembrane, potion::SlowFalling},
    {potion::SlowFalling, itm::Redstone, potion::LongSlowFalling},
    {potion::Awkward, itm::BreezeRod, potion::WindCharged},
    {potion::Awkward, itm::SlimeBlock, potion::Oozing},
    {potion::Awkward, itm::Stone, potion::Infested},
    {potion::Awkward, itm::Cobweb, potion::Weaving},
};

constexpr int BREW_TICKS = 400, FUEL_USES = 20;

bool isPotionItem(uint16_t id) { return id == itm::Potion || id == itm::SplashPotion || id == itm::LingeringPotion; }

void levelEvent(Server& s, int type, int x, int y, int z) {
    Packet pk(pkt::s2c::WorldEvent);
    pk.w.i32(type);
    pk.w.u64(packPos(x, y, z));
    pk.w.i32(0);
    pk.w.boolean(false);
    s.broadcastNear(pk, x >> 4, z >> 4, nullptr);
}

}  // namespace

bool brewingIngredient(uint16_t item) {
    if (item == itm::Gunpowder || item == itm::DragonBreath) return true;
    for (const Mix& m : MIXES)
        if (m.ingredient == item) return true;
    return false;
}

// What a slot of the stand takes (BrewingStandMenu's slots).
bool brewingAccepts(int slot, const ItemStack& st) {
    if (slot < 3) return isPotionItem(st.id) || st.id == itm::GlassBottle;
    if (slot == 3) return brewingIngredient(st.id);
    return st.id == itm::BlazePowder;
}

// PotionBrewing#mix: the bottle brewed with the ingredient (empty: no mix).
ItemStack brewResult(const ItemStack& bottle, uint16_t ingredient) {
    if (bottle.empty() || !isPotionItem(bottle.id)) return ItemStack();
    int p = potionOf(bottle);
    if (p < 0) return ItemStack();
    if (ingredient == itm::Gunpowder) return bottle.id == itm::Potion ? potionItem(itm::SplashPotion, p) : ItemStack();
    if (ingredient == itm::DragonBreath) return bottle.id == itm::SplashPotion ? potionItem(itm::LingeringPotion, p) : ItemStack();
    for (const Mix& m : MIXES)
        if (m.from == p && m.ingredient == ingredient) return potionItem(bottle.id, m.to);
    return ItemStack();
}

static bool canBrew(const TileEntity& t) {
    const ItemStack& ing = t.items[3];
    if (ing.empty() || !brewingIngredient(ing.id)) return false;
    for (int i = 0; i < 3; i++)
        if (!brewResult(t.items[i], ing.id).empty()) return true;
    return false;
}

// BrewingStandBlockEntity#doBrew
static void doBrew(Server& s, TileEntity& t, int x, int y, int z) {
    ItemStack& ing = t.items[3];
    uint16_t used = ing.id;
    for (int i = 0; i < 3; i++) {
        ItemStack r = brewResult(t.items[i], used);
        if (!r.empty()) t.items[i] = r;
    }
    if (--ing.count == 0) ing.clear();
    if (used == itm::DragonBreath) {   // its bottle stays (crafting remainder)
        if (ing.empty()) ing = ItemStack::of(itm::GlassBottle);
        else s.dropItem(x + 0.5, y + 0.5, z + 0.5, ItemStack::of(itm::GlassBottle));
    }
    levelEvent(s, 1035, x, y, z);   // the brewing stand's sound
}

void Server::updateBrewing(int x, int y, int z, bool reschedule) {
    TimerKey key = TimerKey::furnace(x, y, z, curDim);
    Chunk* c = world.peek(curDim, x >> 4, z >> 4);
    uint16_t st = c ? c->get(x & 15, y, z & 15) : 0;
    TileEntity* t = c ? c->tileAt(x & 15, y, z & 15) : nullptr;
    if (!t || t->type != TILE_BREWING || blockIdOf(st) != blk::BrewingStand) {
        timers.cancel(key);
        return;
    }
    uint32_t now = worldTick();
    uint32_t elapsed = t->updated && (int32_t)(now - t->updated) > 0 ? now - t->updated : 0;
    t->updated = now;
    bool changed = false;
    for (int guard = 0; guard < 1000; guard++) {
        if (t->burnTime <= 0 && t->items[4].id == itm::BlazePowder) {   // refuel
            t->burnTime = FUEL_USES;
            if (--t->items[4].count == 0) t->items[4].clear();
            changed = true;
        }
        bool brewable = canBrew(*t);
        if (t->cookTime > 0) {
            if (!brewable || t->items[3].id != (uint16_t)t->burnTotal) {   // the ingredient taken away
                t->cookTime = 0;
                changed = true;
                continue;
            }
            uint32_t step = elapsed < (uint32_t)t->cookTime ? elapsed : (uint32_t)t->cookTime;
            t->cookTime = (int16_t)(t->cookTime - step);
            elapsed -= step;
            if (t->cookTime > 0) break;
            doBrew(*this, *t, x, y, z);
            changed = true;
        } else if (brewable && t->burnTime > 0) {
            t->burnTime--;
            t->cookTime = BREW_TICKS;
            t->burnTotal = (int16_t)t->items[3].id;
            changed = true;
        } else {
            break;
        }
    }
    // the bottles show on the block
    uint16_t shown = st;
    shown = setBool(shown, "has_bottle_0", !t->items[0].empty());
    shown = setBool(shown, "has_bottle_1", !t->items[1].empty());
    shown = setBool(shown, "has_bottle_2", !t->items[2].empty());
    if (shown != st) world.setBlock(curDim, x, y, z, shown);
    if (changed) {
        c->dirty = true;
        redstone.analogChanged(*this, x, y, z);
        for (int k = 0; k < MC_MAX_PLAYERS; k++) {
            Player& p = players[k];
            if (!p.inPlay() || p.e.dim != curDim || p.winKind != WK_BREWING || p.winX != x || p.winY != y || p.winZ != z)
                continue;
            sendBrewingProps(p, *t);
            resendWindow(p);
        }
    }
    if (!reschedule) return;
    timers.cancel(key);
    if (t->cookTime > 0) timers.schedule(key, now + (uint32_t)t->cookTime);
}

void Server::sendBrewingProps(Player& p, const TileEntity& t) {
    int16_t vals[2] = {t.cookTime, t.burnTime};   // BrewingStandMenu's data: brew time, fuel
    for (int i = 0; i < 2; i++) {
        Packet pk(pkt::s2c::CraftProgressBar);
        pk.w.varint(p.winId);
        pk.w.i16((int16_t)i);
        pk.w.i16(vals[i]);
        p.conn.send(pk);
    }
}

}  // namespace mc
