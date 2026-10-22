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

// A villager's own state (Villager, VillagerData): profession, level, experience, its job
// site, restocking, and its trades.
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
};

}  // namespace mc
