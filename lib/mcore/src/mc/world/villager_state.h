// A villager's trades and state, kept with the entity and saved with its chunk
// (villagers.cpp has what they do).
#pragma once
#include <stdint.h>
#include <vector>
#include "mc/item.h"

namespace mc {

// A villager's trade (MerchantOffer): what it wants (the first price before demand), what
// it gives, how often until it restocks, the experience it earns it, and the demand that
// raises the first price.
struct TradeOffer {
    ItemStack buyA, buyB, sell;
    uint16_t uses = 0, maxUses = 12;
    uint8_t xp = 1;
    float priceMult = 0.05f;
    int16_t demand = 0;
    int16_t special = 0;
};

// What a villager remembers of a player (GossipContainer): weighted, they make its prices
// (major positive 5 a point: cured; minor positive 1: cured; trading 1; minor negative
// -1: hurt; major negative -5: a villager killed). who: the player's UUID, its first half.
struct Gossip {
    uint64_t who = 0;
    uint8_t majorPos = 0, minorPos = 0, trading = 0, minorNeg = 0, majorNeg = 0;
    int reputation() const { return majorPos * 5 + minorPos + trading - minorNeg - majorNeg * 5; }
};

// A villager's own state (Villager, VillagerData): profession, level, experience, its job
// site, restocking, and its trades; what it thinks of players; a zombie villager's cure.
struct VillagerState {
    uint8_t profession = 0;   // VillagerProfession registry id (0: none)
    uint8_t level = 1;        // 1 novice .. 5 master
    int32_t xp = 0;
    bool hasJob = false;
    int32_t jobX = 0, jobY = 0, jobZ = 0;
    uint8_t restocks = 0;     // on day restockDay
    uint32_t restockDay = 0;
    uint32_t lastRestock = 0; // world age
    std::vector<TradeOffer> trades;
    static const int GOSSIPS = 4;
    Gossip gossip[GOSSIPS];
    uint32_t gossipDay = 0;   // the day of the last decay
    int16_t convertTicks = 0; // a zombie villager being cured: ticks left (0: not)
    uint64_t convertBy = 0;   // who cured it
    // its life (villager_life.cpp): its bed, food (points: bread 4, a carrot 1, ...), the
    // wait after breeding, and a child's age (negative: ticks until grown up)
    bool hasHome = false;
    int32_t homeX = 0, homeY = 0, homeZ = 0;
    uint8_t food = 0;
    int16_t breedCooldown = 0;
    int32_t growAge = 0;
    // a wandering trader (wandering_trader.cpp): ticks until it leaves, where it heads
    int32_t despawnDelay = 0;
    bool hasWander = false;
    int32_t wanderX = 0, wanderY = 0, wanderZ = 0;
    int reputation(uint64_t who) const {
        for (const Gossip& g : gossip)
            if (g.who == who && who) return g.reputation();
        return 0;
    }
    Gossip* gossipOf(uint64_t who) {   // a slot for the player (the least known one given up)
        for (Gossip& g : gossip)
            if (g.who == who) return &g;
        Gossip* slot = nullptr;
        for (Gossip& g : gossip)
            if (!g.who && !slot) slot = &g;
        auto weight = [](const Gossip& g) { int r = g.reputation(); return r < 0 ? -r : r; };
        if (!slot) {
            slot = &gossip[0];
            for (Gossip& g : gossip)
                if (weight(g) < weight(*slot)) slot = &g;
        }
        *slot = Gossip();
        slot->who = who;
        return slot;
    }
};

}  // namespace mc
