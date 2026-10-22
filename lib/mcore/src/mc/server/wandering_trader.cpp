// The wandering trader, after vanilla 1.21 (WanderingTrader, WanderingTraderSpawner,
// VillagerTrades.WANDERING_TRADER_TRADES): once a day a chance (25%, a quarter more each
// time it did not come, at most 75%) that one comes near a player, towards the village
// bell if one is near; five ordinary trades and a rare one, no levels, no restocking;
// after 40 minutes it leaves. (The trades are 1.21's: 1.21.5's new buying trades are not
// here; no trader llamas.)
#include <math.h>
#include <stdlib.h>
#include "mc/registry.h"
#include "mc/server/mob_util.h"
#include "mc/server/server.h"

namespace mc {

namespace {

// ItemsForEmeralds(item, emeralds, count, maxUses, 1 experience)
struct Ware { uint16_t item; uint8_t emeralds, count, uses; };

const Ware COMMON[] = {
    {itm::SeaPickle, 2, 1, 5}, {itm::SlimeBall, 4, 1, 5}, {itm::Glowstone, 2, 1, 5}, {itm::NautilusShell, 5, 1, 5},
    {itm::Fern, 1, 1, 12}, {itm::SugarCane, 1, 1, 8}, {itm::Pumpkin, 1, 1, 4}, {itm::Kelp, 3, 1, 12}, {itm::Cactus, 3, 1, 8},
    {itm::Dandelion, 1, 1, 12}, {itm::Poppy, 1, 1, 12}, {itm::BlueOrchid, 1, 1, 8}, {itm::Allium, 1, 1, 12},
    {itm::AzureBluet, 1, 1, 12}, {itm::RedTulip, 1, 1, 12}, {itm::OrangeTulip, 1, 1, 12}, {itm::WhiteTulip, 1, 1, 12},
    {itm::PinkTulip, 1, 1, 12}, {itm::OxeyeDaisy, 1, 1, 12}, {itm::Cornflower, 1, 1, 12}, {itm::LilyOfTheValley, 1, 1, 7},
    {itm::WheatSeeds, 1, 1, 12}, {itm::BeetrootSeeds, 1, 1, 12}, {itm::PumpkinSeeds, 1, 1, 12}, {itm::MelonSeeds, 1, 1, 12},
    {itm::AcaciaSapling, 5, 1, 8}, {itm::BirchSapling, 5, 1, 8}, {itm::DarkOakSapling, 5, 1, 8}, {itm::JungleSapling, 5, 1, 8},
    {itm::OakSapling, 5, 1, 8}, {itm::SpruceSapling, 5, 1, 8}, {itm::CherrySapling, 5, 1, 8}, {itm::MangrovePropagule, 5, 1, 8},
    {itm::RedDye, 1, 3, 12}, {itm::WhiteDye, 1, 3, 12}, {itm::BlueDye, 1, 3, 12}, {itm::PinkDye, 1, 3, 12},
    {itm::BlackDye, 1, 3, 12}, {itm::GreenDye, 1, 3, 12}, {itm::LightGrayDye, 1, 3, 12}, {itm::MagentaDye, 1, 3, 12},
    {itm::YellowDye, 1, 3, 12}, {itm::GrayDye, 1, 3, 12}, {itm::PurpleDye, 1, 3, 12}, {itm::LightBlueDye, 1, 3, 12},
    {itm::LimeDye, 1, 3, 12}, {itm::OrangeDye, 1, 3, 12}, {itm::BrownDye, 1, 3, 12}, {itm::CyanDye, 1, 3, 12},
    {itm::BrainCoralBlock, 3, 1, 8}, {itm::BubbleCoralBlock, 3, 1, 8}, {itm::FireCoralBlock, 3, 1, 8},
    {itm::HornCoralBlock, 3, 1, 8}, {itm::TubeCoralBlock, 3, 1, 8}, {itm::Vine, 1, 1, 12}, {itm::BrownMushroom, 1, 1, 12},
    {itm::RedMushroom, 1, 1, 12}, {itm::LilyPad, 1, 2, 5}, {itm::SmallDripleaf, 1, 2, 5}, {itm::Sand, 1, 8, 8},
    {itm::RedSand, 1, 4, 6}, {itm::PointedDripstone, 1, 2, 5}, {itm::RootedDirt, 1, 2, 5}, {itm::MossBlock, 1, 2, 5},
};
const Ware RARE[] = {
    {itm::TropicalFishBucket, 5, 1, 4}, {itm::PufferfishBucket, 5, 1, 4}, {itm::PackedIce, 3, 1, 6},
    {itm::BlueIce, 6, 1, 6}, {itm::Gunpowder, 1, 1, 8}, {itm::Podzol, 3, 3, 6},
};

void pick(VillagerState& v, const Ware* list, int n, int count) {
    int idx[80];
    for (int i = 0; i < n; i++) idx[i] = i;
    for (int k = 0; k < count && n > 0; k++) {
        int j = mobs::rng().range(n);
        const Ware& w = list[idx[j]];
        idx[j] = idx[--n];
        TradeOffer o;
        o.buyA = ItemStack::of(itm::Emerald, w.emeralds);
        o.sell = ItemStack::of(w.item, w.count);
        o.maxUses = w.uses;
        o.xp = 1;
        o.priceMult = 0.05f;
        v.trades.push_back(o);
    }
}

}  // namespace

// WanderingTrader#updateTrades: five of the ordinary, one of the rare
void addWanderingOffers(VillagerState& v) {
    v.trades.clear();
    pick(v, COMMON, (int)(sizeof(COMMON) / sizeof(COMMON[0])), 5);
    pick(v, RARE, (int)(sizeof(RARE) / sizeof(RARE[0])), 1);
}

// Each tick: trading keeps it still; it heads for its goal until 2 blocks from it; when
// its time is up and nobody trades with it, it leaves (WanderingTrader#maybeDespawn).
// true: it stands still.
bool Server::tickTrader(Entity& e) {
    VillagerState& v = e.villager;
    if (e.tradingWith >= 0) {
        Player& p = players[e.tradingWith];
        if (!p.inPlay() || p.winKind != WK_MERCHANT || p.merchant != e.id) e.tradingWith = -1;
    }
    if (v.despawnDelay > 0 && e.tradingWith < 0 && --v.despawnDelay == 0) {
        removeEntity(e);
        return true;
    }
    if (v.hasWander) {
        double dx = v.wanderX + 0.5 - e.x, dz = v.wanderZ + 0.5 - e.z;
        if (dx * dx + dz * dz < 2 * 2) v.hasWander = false;
    }
    villagerFlee(e);
    if (e.tradingWith >= 0) {
        Player& p = players[e.tradingWith];
        double dx = p.e.x - e.x, dz = p.e.z - e.z;
        e.yaw = e.headYaw = (float)(atan2(-dx, dz) * 180.0 / M_PI);
        e.vx = e.vz = 0;
        return true;
    }
    return false;
}

void Server::initTrader(Entity& e) {
    addWanderingOffers(e.villager);
    e.villager.despawnDelay = 48000;
}

// A spot for it within `r` blocks of (x, z): on the surface, dry, room to stand
// (WanderingTraderSpawner#findSpawnPositionNear).
bool Server::traderSpot(uint8_t dim, int x, int z, int r, int& sx, int& sy, int& sz) {
    Rng& rng = mobs::rng();
    for (int t = 0; t < 10; t++) {
        int px = x + rng.between(-r, r), pz = z + rng.between(-r, r);
        if (!world.isResident(dim, px >> 4, pz >> 4)) continue;
        int y = world.heightAt(dim, px, pz);
        uint16_t below = world.getBlock(dim, px, y - 1, pz);
        if (!stateCollides(below) || stateIsFluid(world.getBlock(dim, px, y, pz)) || stateCollides(world.getBlock(dim, px, y, pz)) ||
            stateCollides(world.getBlock(dim, px, y + 1, pz)))
            continue;
        sx = px; sy = y; sz = pz;
        return true;
    }
    return false;
}

// WanderingTraderSpawner#tick: every 1200 ticks a day's worth counted down; then a chance.
void Server::tickTraderSpawner() {
    if (!cfg.spawnMobs || ticks % 1200 != 0) return;
    traderDelay -= 1200;
    if (traderDelay > 0) return;
    traderDelay = 24000;
    int chance = traderChance;
    traderChance = traderChance + 25 > 75 ? 75 : traderChance + 25;
    if (mobs::rng().range(100) > chance) return;
    if (spawnTrader(nullptr)) traderChance = 25;
}

// One near a random player in the overworld (or `near`), heading for the bell if there is
// one within 48 blocks of that player.
bool Server::spawnTrader(Player* near) {
    Player* cands[MC_MAX_PLAYERS];
    int n = 0;
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        Player& p = players[i];
        if (p.inPlay() && !p.dead && p.gamemode != GM_SPECTATOR && p.e.dim == DIM_OVERWORLD) cands[n++] = &p;
    }
    Player* p = near ? near : (n ? cands[mobs::rng().range(n)] : nullptr);
    if (!p) return false;
    InDim in(*this, DIM_OVERWORLD);
    int px = (int)floor(p->e.x), pz = (int)floor(p->e.z);
    int bx = px, by = (int)floor(p->e.y), bz = pz;
    Entity probe = Entity();
    probe.x = p->e.x; probe.y = p->e.y; probe.z = p->e.z;
    probe.dim = DIM_OVERWORLD;
    bool bell = bellNear(probe, 48, bx, by, bz);
    int sx, sy, sz;
    if (!traderSpot(DIM_OVERWORLD, bell ? bx : px, bell ? bz : pz, bell ? 10 : 48, sx, sy, sz)) return false;
    Entity* t = spawnMob(ent::WanderingTrader, sx + 0.5, sy, sz + 0.5);
    if (!t) return false;
    t->villager.hasWander = true;
    t->villager.wanderX = bx; t->villager.wanderY = by; t->villager.wanderZ = bz;
    return true;
}

}  // namespace mc
