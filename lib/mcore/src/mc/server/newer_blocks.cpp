// Blocks of 1.17 to 1.21 and what they do:
// - copper (blocks, cut, stairs, slabs, doors, trapdoors, grates, bulbs, chiseled):
//   oxidation by random ticks (WeatheringCopper#changeOverTime), waxing with honeycomb,
//   scraping wax or one stage of oxidation off with an axe
// - candles: up to four in a block, lit with flint and steel or a fire charge, blown
//   out with an empty hand; a candle on an uneaten cake makes a candle cake
// - budding amethyst grows buds into clusters on its faces
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc/platform.h"
#include "mc/registry.h"
#include "mc/server/server.h"

namespace mc {

namespace {

Rng s_newRng(0x1171);
const char* const FACE_NAMES[6] = {"down", "up", "north", "south", "west", "east"};

bool startsWith(const char* s, const char* p) { return strncmp(s, p, strlen(p)) == 0; }
bool endsWith(const char* s, const char* suf) {
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcmp(s + a - b, suf);
}
int faceIndexOf(const char* dir) {
    for (int i = 0; i < 6; i++)
        if (dir && !strcmp(FACE_NAMES[i], dir)) return i;
    return 0;
}

// ------------------------------------------------------------------ copper
const char* const STAGES[] = {"", "exposed_", "weathered_", "oxidized_"};

// A block of the copper family: its base name ("copper" for the plain block, "cut_copper",
// "copper_door", ...), oxidation stage (0..3) and whether it is waxed; false otherwise.
bool copperParts(uint16_t id, char* base, size_t cap, int& stage, bool& waxed) {
    const char* n = BLOCKS[id].name;
    if (!strstr(n, "copper") || strstr(n, "ore") || startsWith(n, "raw_")) return false;
    waxed = startsWith(n, "waxed_");
    const char* rest = waxed ? n + 6 : n;
    stage = 0;
    for (int k = 1; k < 4; k++)
        if (startsWith(rest, STAGES[k])) {
            stage = k;
            rest += strlen(STAGES[k]);
        }
    if (!strcmp(rest, "copper_block")) rest = "copper";
    snprintf(base, cap, "%s", rest);
    return true;
}

int copperBlock(const char* base, int stage, bool waxed) {
    char core[64], name[72];
    if (stage == 0) snprintf(core, sizeof(core), "%s", !strcmp(base, "copper") ? "copper_block" : base);
    else snprintf(core, sizeof(core), "%s%s", STAGES[stage], base);
    snprintf(name, sizeof(name), "%s%s", waxed ? "waxed_" : "", core);
    return findBlock(name);
}

// The same state of another block of the family (every property carried over by name).
uint16_t convert(uint16_t from, int toBlock) {
    const BlockDef& a = blockOf(from);
    uint16_t st = BLOCKS[toBlock].defState;
    for (int i = 0; i < a.propCount; i++) {
        const char* pn = PROPS[BLOCK_PROPS[a.propStart + i]].name;
        const char* v = getPropStr(from, pn);
        if (v) st = setPropStr(st, pn, v);
    }
    return st;
}

// ------------------------------------------------------------------ candles
bool isCandle(uint16_t id) {
    const char* n = BLOCKS[id].name;
    return !strcmp(n, "candle") || (endsWith(n, "_candle"));
}
bool isCandleCake(uint16_t id) { return endsWith(BLOCKS[id].name, "candle_cake"); }

}  // namespace

// Turns the copper block at (x, y, z) into another block of its family; doors change
// both halves. Returns false if there is no such block.
static bool changeCopper(Server& s, int x, int y, int z, uint16_t st, const char* base, int stage, bool waxed) {
    int nb = copperBlock(base, stage, waxed);
    if (nb < 0) return false;
    s.world.setBlock(s.curDim, x, y, z, convert(st, nb), true, 3);
    if (endsWith(base, "_door")) {
        int oy = !strcmp(getPropStr(st, "half"), "upper") ? y - 1 : y + 1;
        uint16_t other = s.blockAt(x, oy, z);
        if (blockIdOf(other) == blockIdOf(st)) s.world.setBlock(s.curDim, x, oy, z, convert(other, nb), true, 3);
    }
    return true;
}

static void worldEvent(Server& s, int event, int x, int y, int z) {
    Packet pk(pkt::s2c::WorldEvent);
    pk.w.i32(event);
    pk.w.u64(packPos(x, y, z));
    pk.w.i32(0);
    pk.w.boolean(false);
    s.broadcastNear(pk, x >> 4, z >> 4);
}

bool Server::useItemOnNewerBlock(Player& p, int x, int y, int z, uint16_t st, ItemStack& it) {
    uint16_t id = blockIdOf(st);
    char base[64];
    int stage;
    bool waxed;
    // honeycomb waxes copper (event 3003: the wax-on particles and sound)
    if (it.id == itm::Honeycomb && copperParts(id, base, sizeof(base), stage, waxed) && !waxed) {
        if (!changeCopper(*this, x, y, z, st, base, stage, true)) return false;
        worldEvent(*this, 3003, x, y, z);
        if (p.isSurvivalLike()) consumeHeld(p);
        return true;
    }
    // an axe takes the wax off (3004), else one stage of oxidation (3005)
    if (ITEMS[it.id].kind == IK_AXE && copperParts(id, base, sizeof(base), stage, waxed) && (waxed || stage > 0)) {
        if (!changeCopper(*this, x, y, z, st, base, waxed ? stage : stage - 1, false)) return false;
        worldEvent(*this, waxed ? 3004 : 3005, x, y, z);
        playSound(waxed ? "item.axe.wax_off" : "item.axe.scrape", x + 0.5, y + 0.5, z + 0.5, 1, 1, 4);
        if (p.isSurvivalLike()) damageHeldItem(p, 1);
        return true;
    }
    // candles: another of the same colour joins (up to four)
    if (itemIsBlock(it.id) && ITEMS[it.id].block == id && isCandle(id)) {
        int n = getProp(st, "candles");   // value index: 0..3 for 1..4 candles
        if (n >= 3) return false;
        world.setBlock(curDim, x, y, z, setProp(st, "candles", n + 1), true, 3);
        playSound("block.candle.place", x + 0.5, y + 0.5, z + 0.5, 1, 1, 4);
        if (p.gamemode != GM_CREATIVE) consumeHeld(p);
        return true;
    }
    // a candle on a cake nobody ate from
    if (itemIsBlock(it.id) && isCandle(ITEMS[it.id].block) && id == blk::Cake && getProp(st, "bites") == 0) {
        char name[48];
        snprintf(name, sizeof(name), "%s_cake", BLOCKS[ITEMS[it.id].block].name);
        int cb = findBlock(name);
        if (cb < 0) return false;
        world.setBlock(curDim, x, y, z, BLOCKS[cb].defState, true, 3);
        playSound("block.cake.add_candle", x + 0.5, y + 0.5, z + 0.5, 1, 1, 4);
        if (p.gamemode != GM_CREATIVE) consumeHeld(p);
        return true;
    }
    // flint and steel or a fire charge lights candles and candle cakes (not under water)
    if ((it.id == itm::FlintAndSteel || it.id == itm::FireCharge) && (isCandle(id) || isCandleCake(id)) &&
        !getBool(st, "lit") && getProp(st, "waterlogged") != 0) {
        world.setBlock(curDim, x, y, z, setBool(st, "lit", true), true, 11);
        playSound(it.id == itm::FireCharge ? "item.firecharge.use" : "item.flintandsteel.use", x + 0.5, y + 0.5, z + 0.5, 1, 1, 7);
        if (p.isSurvivalLike()) {
            if (it.id == itm::FireCharge) consumeHeld(p);
            else damageHeldItem(p, 1);
        }
        return true;
    }
    return false;
}

bool Server::interactNewerBlock(Player& p, int x, int y, int z, uint16_t st) {
    uint16_t id = blockIdOf(st);
    bool emptyHand = p.heldItem().empty();
    // a candle goes on an uneaten cake before anyone eats from it (CakeBlock#useItemOn)
    const ItemStack& held = p.heldItem();
    if (id == blk::Cake && !emptyHand && itemIsBlock(held.id) && isCandle(ITEMS[held.id].block) && getProp(st, "bites") == 0)
        return useItemOnNewerBlock(p, x, y, z, st, p.heldItem());
    // blowing out candles takes an empty hand
    if (isCandle(id) && getBool(st, "lit") && emptyHand && p.gamemode != GM_ADVENTURE) {
        world.setBlock(curDim, x, y, z, setBool(st, "lit", false), true, 11);
        playSound("block.candle.extinguish", x + 0.5, y + 0.5, z + 0.5, 1, 1, 4);
        return true;
    }
    if (isCandleCake(id)) {
        // flint and steel or a fire charge light it (the item's use, not eating)
        if (held.id == itm::FlintAndSteel || held.id == itm::FireCharge) return false;
        if (getBool(st, "lit") && emptyHand) {
            world.setBlock(curDim, x, y, z, setBool(st, "lit", false), true, 11);
            playSound("block.candle.extinguish", x + 0.5, y + 0.5, z + 0.5, 1, 1, 4);
            return true;
        }
        // eating takes the candle off: it drops, a slice is eaten (CandleCakeBlock)
        if (p.food >= 20 && p.gamemode != GM_CREATIVE) return false;
        p.food = p.food + 2 > 20 ? 20 : p.food + 2;
        p.saturation += 0.4f;
        p.healthDirty = true;
        char candle[48];
        snprintf(candle, sizeof(candle), "%.*s", (int)(strlen(BLOCKS[id].name) - 5), BLOCKS[id].name);   // without "_cake"
        int cb = findBlock(candle);
        world.setBlock(curDim, x, y, z, setProp(bs::Cake, "bites", 1), true, 3);
        if (cb >= 0 && BLOCKS[cb].item != 0xFFFF) dropItem(x + 0.5, y + 0.5, z + 0.5, ItemStack::of(BLOCKS[cb].item, 1));
        return true;
    }
    return false;
}

bool newerRandomTicking(uint16_t id) {
    char base[64];
    int stage;
    bool waxed;
    if (id == blk::BuddingAmethyst) return true;
    return copperParts(id, base, sizeof(base), stage, waxed) && !waxed && stage < 3;
}

void Server::randomTickNewerBlock(int x, int y, int z, uint16_t st) {
    uint16_t id = blockIdOf(st);
    if (id == blk::BuddingAmethyst) {
        // BuddingAmethystBlock#randomTick: a fifth of the time, on a random face
        if (s_newRng.range(5) != 0) return;
        int f = (int)s_newRng.range(6);
        int tx = x + FACE_DX[f], ty = y + FACE_DY[f], tz = z + FACE_DZ[f];
        uint16_t at = blockAt(tx, ty, tz);
        uint16_t aid = blockIdOf(at);
        static const uint16_t GROWTH[] = {blk::SmallAmethystBud, blk::MediumAmethystBud, blk::LargeAmethystBud, blk::AmethystCluster};
        int next = -1;
        bool water = at == bs::Water;   // a source block
        if (stateIsAir(at) || water) next = 0;
        else
            for (int k = 0; k < 3; k++)
                if (aid == GROWTH[k] && faceIndexOf(getPropStr(at, "facing")) == f) next = k + 1;
        if (next < 0) return;
        uint16_t ns = setPropStr(BLOCKS[GROWTH[next]].defState, "facing", FACE_NAMES[f]);
        if (next == 0) ns = setBool(ns, "waterlogged", water);
        else ns = setBool(ns, "waterlogged", getBool(at, "waterlogged"));
        world.setBlock(curDim, tx, ty, tz, ns, true, 3);
        return;
    }
    // WeatheringCopper#changeOverTime: 0.0569 of the random ticks look at the copper
    // within 4 blocks (Manhattan): none may be less oxidized, and the more of them are
    // further along, the likelier the change (unoxidized copper 0.75 as likely)
    char base[64];
    int stage;
    bool waxed;
    if (!copperParts(id, base, sizeof(base), stage, waxed) || waxed || stage >= 3) return;
    if (s_newRng.unit() >= 0.05688889f) return;
    int same = 0, further = 0;
    for (int dy = -4; dy <= 4; dy++)
        for (int dz = -4; dz <= 4; dz++)
            for (int dx = -4; dx <= 4; dx++) {
                int d = abs(dx) + abs(dy) + abs(dz);
                if (d == 0 || d > 4) continue;
                uint16_t o = blockAt(x + dx, y + dy, z + dz);
                char ob[64];
                int os;
                bool ow;
                if (!copperParts(blockIdOf(o), ob, sizeof(ob), os, ow) || ow) continue;
                if (os < stage) return;
                if (os > stage) further++;
                else same++;
            }
    float f = (float)(further + 1) / (float)(further + same + 1);
    float chance = f * f * (stage == 0 ? 0.75f : 1.0f);
    if (s_newRng.unit() < chance) changeCopper(*this, x, y, z, st, base, stage + 1, false);
}

// Breaking: amethyst buds give nothing (without silk touch), a cluster four shards with a
// pickaxe (else two), budding amethyst nothing. false: the usual drops.
bool newerBlockDrops(Server& s, Player* by, int x, int y, int z, uint16_t st) {
    uint16_t id = blockIdOf(st);
    if (id == blk::SmallAmethystBud || id == blk::MediumAmethystBud || id == blk::LargeAmethystBud || id == blk::BuddingAmethyst)
        return true;
    if (id == blk::AmethystCluster) {
        bool pick = by && !by->heldItem().empty() && ITEMS[by->heldItem().id].kind == IK_PICKAXE;
        s.dropItem(x + 0.5, y + 0.5, z + 0.5, ItemStack::of(itm::AmethystShard, pick ? 4 : 2));
        return true;
    }
    return false;
}

// Support: buds and clusters hang on the block they grew from, candles stand on a solid
// top. -1: not one of these blocks.
int newerBlockSupported(Server& s, uint16_t st, int x, int y, int z) {
    uint16_t id = blockIdOf(st);
    if (id == blk::SmallAmethystBud || id == blk::MediumAmethystBud || id == blk::LargeAmethystBud || id == blk::AmethystCluster) {
        int f = faceIndexOf(getPropStr(st, "facing"));
        int b = f ^ 1;   // the opposite face
        return stateFaceSturdy(s.blockAt(x + FACE_DX[b], y + FACE_DY[b], z + FACE_DZ[b]), f);
    }
    if (isCandle(id)) return stateCollides(s.blockAt(x, y - 1, z));
    return -1;
}

}  // namespace mc
