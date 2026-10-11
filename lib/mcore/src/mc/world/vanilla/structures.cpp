// Vanilla's structures: placement, starts and the pieces of the hard-coded structures
// (structures.h). After vanilla 1.21.8's StructurePlacement, RandomSpreadStructurePlacement,
// ChunkGenerator#tryGenerateStructure, SinglePieceStructure and the pieces' postProcess.
#include "mc/world/vanilla/structures.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "mc/platform.h"
#include "mc/registry.h"
#include "mc/world/chunk.h"
#include "mc/world/vanilla/java_random.h"
#include "mc/world/vanilla/structure_build.h"

namespace mc {
namespace vanilla {

StructureStats structureStats;
using namespace sb;

namespace sb {
const char* const BLOCK_SPEC[B_NUM] = {
    "air", "sandstone", "cut_sandstone", "chiseled_sandstone", "sandstone_stairs", "sandstone_slab", "orange_terracotta",
    "blue_terracotta", "tnt", "stone_pressure_plate", "chest", "cobblestone", "mossy_cobblestone", "cobblestone_stairs",
    "chiseled_stone_bricks", "tripwire_hook", "tripwire", "redstone_wire", "dispenser", "vine", "lever", "sticky_piston",
    "repeater", "spruce_planks", "oak_log", "oak_fence", "potted_red_mushroom", "crafting_table", "cauldron",
    "spruce_stairs", "sand", "stone", "andesite", "granite", "diorite",
    "stone_bricks", "cracked_stone_bricks", "mossy_stone_bricks", "infested_stone_bricks", "cave_air", "wall_torch",
    "smooth_stone_slab", "iron_bars", "iron_door", "oak_door", "stone_button", "water", "oak_planks", "ladder", "cobweb",
    "bookshelf", "torch", "lava", "stone_brick_stairs", "end_portal_frame", "end_portal", "spawner",
    "nether_bricks", "nether_brick_fence", "nether_brick_stairs", "soul_sand", "nether_wart",
    "rail", "dark_oak_planks", "dark_oak_fence", "chain",
};
uint16_t B[B_NUM];
}  // namespace sb

namespace {

// ------------------------------------------------------------------ types
// the generator version a type is built from (0: not yet; chosen, it builds nothing)
uint8_t typeSince(uint8_t type) {
    switch (type) {
        case ST_DESERT_PYRAMID: case ST_JUNGLE_TEMPLE: case ST_SWAMP_HUT: case ST_BURIED_TREASURE: return 10;
        case ST_STRONGHOLD: return 11;
        case ST_FORTRESS: return 12;
        case ST_MINESHAFT: return 13;
        default: return 0;
    }
}
// how many chunks from its start chunk a type's pieces reach
int typeReach(uint8_t type) {
    switch (type) {
        case ST_BURIED_TREASURE: return 0;
        case ST_DESERT_PYRAMID: case ST_JUNGLE_TEMPLE: case ST_SWAMP_HUT: case ST_IGLOO: return 2;
        case ST_MINESHAFT: return 6;   // 80 blocks from its room
        default: return 8;
    }
}
bool hasBit(const uint8_t* bits, uint8_t b) { return b < 128 && (bits[b >> 3] >> (b & 7) & 1); }

}  // namespace

struct Structures::Start {
    uint8_t structure;
    int32_t cx, cz;
    int64_t seed;
    SBox box;
    uint16_t n;
    Piece pieces[1];   // n of them
};
struct Structures::Entry {
    int16_t set = -1;
    int32_t cx = 0, cz = 0;
    Start* start = nullptr;   // nullptr: no structure starts here
};
struct StructureList {
    int n;
    Structures::Start* starts[1];   // copies, n of them (freed with the list)
};

namespace {
constexpr int CACHE = 1024;
int16_t lootItem[512];
bool resolved = false;

Structures::Start* newStart(int n) {
    size_t size = sizeof(Structures::Start) + sizeof(Piece) * (n > 0 ? n - 1 : 0);
    Structures::Start* s = (Structures::Start*)plat::bigAlloc(size);
    if (s) memset(s, 0, size);
    return s;
}
size_t startSize(const Structures::Start* s) { return sizeof(Structures::Start) + sizeof(Piece) * (s->n > 0 ? s->n - 1 : 0); }

void resolveBlocks() {
    if (resolved) return;
    for (int i = 0; i < B_NUM; i++) {
        int b = findBlock(BLOCK_SPEC[i]);
        B[i] = b >= 0 ? BLOCKS[b].defState : 0;
    }
    for (int i = 0; i < SD_NUM_LOOT_ITEMS && i < 512; i++) lootItem[i] = (int16_t)findItem(SD_LOOT_ITEMS[i]);
    resolved = true;
}

int lootTableIndex(const char* name) {
    for (int i = 0; i < SD_NUM_LOOT_TABLES; i++)
        if (!strcmp(SD_LOOT_TABLES[i].name, name)) return i;
    return -1;
}

}  // namespace

// LootTable#fill, simplified: the pools' rolls, the stacks split over random free slots
void sb::fillLoot(TileEntity* t, const char* table, int64_t seed) {
    int ti = lootTableIndex(table);
    if (!t || ti < 0) return;
    JavaRandom r(seed);
    struct Stack { int16_t item; uint8_t count; };
    Stack items[27];
    int n = 0;
    const LootTable& lt = SD_LOOT_TABLES[ti];
    for (int p = 0; p < lt.pools; p++) {
        const LootPool& pool = SD_LOOT_POOLS[lt.firstPool + p];
        int rolls = pool.rollsMin + (pool.rollsMax > pool.rollsMin ? r.nextInt(pool.rollsMax - pool.rollsMin + 1) : 0);
        int total = 0;
        for (int e = 0; e < pool.count; e++) total += SD_LOOT_ENTRIES[pool.first + e].weight;
        for (int k = 0; k < rolls && total > 0; k++) {
            int w = r.nextInt(total);
            const LootEntry* pick = nullptr;
            for (int e = 0; e < pool.count; e++) {
                w -= SD_LOOT_ENTRIES[pool.first + e].weight;
                if (w < 0) { pick = &SD_LOOT_ENTRIES[pool.first + e]; break; }
            }
            if (!pick || pick->item < 0 || lootItem[pick->item] < 0) continue;
            int count = pick->min + (pick->max > pick->min ? r.nextInt(pick->max - pick->min + 1) : 0);
            if (count <= 0 || n >= 27) continue;
            int mx = maxStack((uint16_t)lootItem[pick->item]);
            items[n++] = {lootItem[pick->item], (uint8_t)(count > mx ? mx : count)};
        }
    }
    // LootTable#getAvailableSlots: the free slots shuffled
    int slots[27], ns = 27;
    for (int i = 0; i < 27; i++) slots[i] = i;
    for (int i = 26; i > 0; i--) {
        int j = r.nextInt(i + 1), t2 = slots[i];
        slots[i] = slots[j];
        slots[j] = t2;
    }
    // LootTable#shuffleAndSplitItems: stacks of more than one split while slots are free
    Stack split[27];
    int nsplit = 0, m = 0;
    for (int i = 0; i < n; i++) {
        if (items[i].count > 1) split[nsplit++] = items[i];
        else items[m++] = items[i];
    }
    n = m;
    while (ns - n - nsplit > 0 && nsplit > 0 && n + nsplit < 27) {
        int k = r.nextInt(nsplit);
        Stack st = split[k];
        for (int i = k; i < nsplit - 1; i++) split[i] = split[i + 1];
        nsplit--;
        int part = 1 + r.nextInt(st.count / 2);
        Stack other = {st.item, (uint8_t)part};
        st.count = (uint8_t)(st.count - part);
        if (st.count > 1 && r.nextBoolean()) split[nsplit++] = st;
        else items[n++] = st;
        if (other.count > 1 && r.nextBoolean()) split[nsplit++] = other;
        else items[n++] = other;
    }
    for (int i = 0; i < nsplit && n < 27; i++) items[n++] = split[i];
    for (int i = n - 1; i > 0; i--) {   // Util.shuffle
        int j = r.nextInt(i + 1);
        Stack t2 = items[i];
        items[i] = items[j];
        items[j] = t2;
    }
    if (n > ns) n = ns;
    for (int i = 0; i < n; i++) {
        ItemStack& slot = t->items[slots[ns - 1 - i]];
        slot.id = (uint16_t)items[i].item;
        slot.count = items[i].count;
    }
}

namespace {
// ---- the desert pyramid (DesertPyramidPiece, 21 x 15 x 21)
void buildDesertPyramid(Build& b) {
    const int W = 21, D = 21;
    uint16_t SS = B[B_SANDSTONE], CUT = B[B_CUT_SANDSTONE], CH = B[B_CHISELED_SANDSTONE], OR = B[B_ORANGE_TERRACOTTA];
    uint16_t sN = facing(B[B_SANDSTONE_STAIRS], NORTH), sS = facing(B[B_SANDSTONE_STAIRS], SOUTH),
             sE = facing(B[B_SANDSTONE_STAIRS], EAST), sW = facing(B[B_SANDSTONE_STAIRS], WEST);
    b.box(0, -4, 0, W - 1, 0, D - 1, SS, SS);
    for (int i = 1; i <= 9; i++) {
        b.box(i, i, i, W - 1 - i, i, D - 1 - i, SS, SS);
        b.box(i + 1, i, i + 1, W - 2 - i, i, D - 2 - i, 0, 0);
    }
    for (int x = 0; x < W; x++)
        for (int z = 0; z < D; z++) b.columnDown(SS, x, -5, z);
    b.box(0, 0, 0, 4, 9, 4, SS, 0);
    b.box(1, 10, 1, 3, 10, 3, SS, SS);
    b.set(2, 10, 0, sN); b.set(2, 10, 4, sS); b.set(0, 10, 2, sE); b.set(4, 10, 2, sW);
    b.box(W - 5, 0, 0, W - 1, 9, 4, SS, 0);
    b.box(W - 4, 10, 1, W - 2, 10, 3, SS, SS);
    b.set(W - 3, 10, 0, sN); b.set(W - 3, 10, 4, sS); b.set(W - 5, 10, 2, sE); b.set(W - 1, 10, 2, sW);
    b.box(8, 0, 0, 12, 4, 4, SS, 0);
    b.air(9, 1, 0, 11, 3, 4);
    b.set(9, 1, 1, CUT); b.set(9, 2, 1, CUT); b.set(9, 3, 1, CUT); b.set(10, 3, 1, CUT);
    b.set(11, 3, 1, CUT); b.set(11, 2, 1, CUT); b.set(11, 1, 1, CUT);
    b.box(4, 1, 1, 8, 3, 3, SS, 0);
    b.air(4, 1, 2, 8, 2, 2);
    b.box(12, 1, 1, 16, 3, 3, SS, 0);
    b.air(12, 1, 2, 16, 2, 2);
    b.box(5, 4, 5, W - 6, 4, D - 6, SS, SS);
    b.air(9, 4, 9, 11, 4, 11);
    b.box(8, 1, 8, 8, 3, 8, CUT, CUT); b.box(12, 1, 8, 12, 3, 8, CUT, CUT);
    b.box(8, 1, 12, 8, 3, 12, CUT, CUT); b.box(12, 1, 12, 12, 3, 12, CUT, CUT);
    b.box(1, 1, 5, 4, 4, 11, SS, SS);
    b.box(W - 5, 1, 5, W - 2, 4, 11, SS, SS);
    b.box(6, 7, 9, 6, 7, 11, SS, SS);
    b.box(W - 7, 7, 9, W - 7, 7, 11, SS, SS);
    b.box(5, 5, 9, 5, 7, 11, CUT, CUT);
    b.box(W - 6, 5, 9, W - 6, 7, 11, CUT, CUT);
    b.set(5, 5, 10, 0); b.set(5, 6, 10, 0); b.set(6, 6, 10, 0);
    b.set(W - 6, 5, 10, 0); b.set(W - 6, 6, 10, 0); b.set(W - 7, 6, 10, 0);
    b.air(2, 4, 4, 2, 6, 4);
    b.air(W - 3, 4, 4, W - 3, 6, 4);
    b.set(2, 4, 5, sN); b.set(2, 3, 4, sN); b.set(W - 3, 4, 5, sN); b.set(W - 3, 3, 4, sN);
    b.box(1, 1, 3, 2, 2, 3, SS, SS);
    b.box(W - 3, 1, 3, W - 2, 2, 3, SS, SS);
    b.set(1, 1, 2, SS); b.set(W - 2, 1, 2, SS);
    b.set(1, 2, 2, B[B_SANDSTONE_SLAB]); b.set(W - 2, 2, 2, B[B_SANDSTONE_SLAB]);
    b.set(2, 1, 2, sW); b.set(W - 3, 1, 2, sE);
    b.box(4, 3, 5, 4, 3, 17, SS, SS);
    b.box(W - 5, 3, 5, W - 5, 3, 17, SS, SS);
    b.air(3, 1, 5, 4, 2, 16);
    b.air(W - 6, 1, 5, W - 5, 2, 16);
    for (int k = 5; k <= 17; k += 2) {
        b.set(4, 1, k, CUT); b.set(4, 2, k, CH);
        b.set(W - 5, 1, k, CUT); b.set(W - 5, 2, k, CH);
    }
    static const int8_t FLOOR[][2] = {{10, 7}, {10, 8}, {9, 9}, {11, 9}, {8, 10}, {12, 10}, {7, 10}, {13, 10}, {9, 11}, {11, 11}, {10, 12}, {10, 13}};
    for (const auto& f : FLOOR) b.set(f[0], 0, f[1], OR);
    b.set(10, 0, 10, B[B_BLUE_TERRACOTTA]);
    // the towers' fronts: bands of cut and chiseled sandstone and terracotta
    static const uint8_t COL[7][3] = {{1, 2, 1}, {1, 2, 1}, {2, 3, 2}, {1, 2, 1}, {2, 3, 2}, {2, 2, 2}, {1, 1, 1}};   // y 2..8: 1 cut, 2 orange, 3 chiseled
    auto pick = [&](int v) { return v == 1 ? CUT : v == 2 ? OR : CH; };
    for (int l = 0; l <= W - 1; l += W - 1)
        for (int y = 2; y <= 8; y++)
            for (int k = 0; k < 3; k++) b.set(l, y, 1 + k, pick(COL[y - 2][k]));
    for (int m = 2; m <= W - 3; m += W - 3 - 2)
        for (int y = 2; y <= 8; y++)
            for (int k = 0; k < 3; k++) b.set(m - 1 + k, y, 0, pick(COL[y - 2][k]));
    b.box(8, 4, 0, 12, 6, 0, CUT, CUT);
    b.set(8, 6, 0, 0); b.set(12, 6, 0, 0);
    b.set(9, 5, 0, OR); b.set(10, 5, 0, CH); b.set(11, 5, 0, OR);
    // the treasure room under the floor, the trap
    b.box(8, -14, 8, 12, -11, 12, CUT, CUT);
    b.box(8, -10, 8, 12, -10, 12, CH, CH);
    b.box(8, -9, 8, 12, -9, 12, CUT, CUT);
    b.box(8, -8, 8, 12, -1, 12, SS, SS);
    b.air(9, -11, 9, 11, -1, 11);
    b.set(10, -11, 10, B[B_STONE_PRESSURE_PLATE]);
    b.box(9, -13, 9, 11, -13, 11, B[B_TNT], 0);
    b.set(8, -11, 10, 0); b.set(8, -10, 10, 0); b.set(7, -10, 10, CH); b.set(7, -11, 10, CUT);
    b.set(12, -11, 10, 0); b.set(12, -10, 10, 0); b.set(13, -10, 10, CH); b.set(13, -11, 10, CUT);
    b.set(10, -11, 8, 0); b.set(10, -10, 8, 0); b.set(10, -10, 7, CH); b.set(10, -11, 7, CUT);
    b.set(10, -11, 12, 0); b.set(10, -10, 12, 0); b.set(10, -10, 13, CH); b.set(10, -11, 13, CUT);
    static const int DX[4] = {0, 1, 0, -1}, DZ[4] = {-1, 0, 1, 0};
    for (int d = 0; d < 4; d++) b.chest(10 + DX[d] * 2, -11, 10 + DZ[d] * 2, "desert_pyramid", d);
}

// ---- the jungle temple (JungleTemplePiece, 12 x 10 x 15)
void buildJungleTemple(Build& b, JavaRandom& r) {
    auto M = [&](int x0, int y0, int z0, int x1, int y1, int z1) { b.mossBox(r, x0, y0, z0, x1, y1, z1); };
    const int W = 12, D = 15;
    M(0, -4, 0, W - 1, 0, D - 1);
    M(2, 1, 2, 9, 2, 2); M(2, 1, 12, 9, 2, 12); M(2, 1, 3, 2, 2, 11); M(9, 1, 3, 9, 2, 11);
    M(1, 3, 1, 10, 6, 1); M(1, 3, 13, 10, 6, 13); M(1, 3, 2, 1, 6, 12); M(10, 3, 2, 10, 6, 12);
    M(2, 3, 2, 9, 3, 12); M(2, 6, 2, 9, 6, 12); M(3, 7, 3, 8, 7, 11); M(4, 8, 4, 7, 8, 10);
    b.air(3, 1, 3, 8, 2, 11); b.air(4, 3, 6, 7, 3, 9); b.air(2, 4, 2, 9, 5, 12); b.air(4, 6, 5, 7, 6, 9); b.air(5, 7, 6, 6, 7, 8);
    b.air(5, 1, 2, 6, 2, 2); b.air(5, 2, 12, 6, 2, 12); b.air(5, 5, 1, 6, 5, 1); b.air(5, 5, 13, 6, 5, 13);
    b.set(1, 5, 5, 0); b.set(10, 5, 5, 0); b.set(1, 5, 9, 0); b.set(10, 5, 9, 0);
    for (int k = 0; k <= 14; k += 14) { M(2, 4, k, 2, 5, k); M(4, 4, k, 4, 5, k); M(7, 4, k, 7, 5, k); M(9, 4, k, 9, 5, k); }
    M(5, 6, 0, 6, 6, 0);
    for (int l = 0; l <= 11; l += 11) {
        for (int m = 2; m <= 12; m += 2) M(l, 4, m, l, 5, m);
        M(l, 6, 5, l, 6, 5);
        M(l, 6, 9, l, 6, 9);
    }
    M(2, 7, 2, 2, 9, 2); M(9, 7, 2, 9, 9, 2); M(2, 7, 12, 2, 9, 12); M(9, 7, 12, 9, 9, 12);
    M(4, 9, 4, 4, 9, 4); M(7, 9, 4, 7, 9, 4); M(4, 9, 10, 4, 9, 10); M(7, 9, 10, 7, 9, 10);
    M(5, 9, 7, 6, 9, 7);
    uint16_t sE = facing(B[B_COBBLESTONE_STAIRS], EAST), sW = facing(B[B_COBBLESTONE_STAIRS], WEST),
             sS = facing(B[B_COBBLESTONE_STAIRS], SOUTH), sN = facing(B[B_COBBLESTONE_STAIRS], NORTH);
    b.set(5, 9, 6, sN); b.set(6, 9, 6, sN); b.set(5, 9, 8, sS); b.set(6, 9, 8, sS);
    b.set(4, 0, 0, sN); b.set(5, 0, 0, sN); b.set(6, 0, 0, sN); b.set(7, 0, 0, sN);
    b.set(4, 1, 8, sN); b.set(4, 2, 9, sN); b.set(4, 3, 10, sN); b.set(7, 1, 8, sN); b.set(7, 2, 9, sN); b.set(7, 3, 10, sN);
    M(4, 1, 9, 4, 1, 9); M(7, 1, 9, 7, 1, 9); M(4, 1, 10, 7, 2, 10);
    M(5, 4, 5, 6, 4, 5);
    b.set(4, 4, 5, sE); b.set(7, 4, 5, sW);
    for (int n = 0; n < 4; n++) {
        b.set(5, 0 - n, 6 + n, sS);
        b.set(6, 0 - n, 6 + n, sS);
        b.air(5, 0 - n, 7 + n, 6, 0 - n, 9 + n);
    }
    b.air(1, -3, 12, 10, -1, 13); b.air(1, -3, 1, 3, -1, 13); b.air(1, -3, 1, 9, -1, 5);
    for (int o = 1; o <= 13; o += 2) M(1, -3, o, 1, -2, o);
    for (int p = 2; p <= 12; p += 2) M(1, -1, p, 3, -1, p);
    M(2, -2, 1, 5, -2, 1); M(7, -2, 1, 9, -2, 1); M(6, -3, 1, 6, -3, 1); M(6, -1, 1, 6, -1, 1);
    // the first trap: a tripwire across the corridor, a dispenser of arrows
    uint16_t hook = setBool(B[B_TRIPWIRE_HOOK], "attached", true), wire = setBool(B[B_TRIPWIRE], "attached", true);
    uint16_t wireEW = setBool(setBool(wire, "east", true), "west", true), wireNS = setBool(setBool(wire, "north", true), "south", true);
    b.set(1, -3, 8, facing(hook, EAST)); b.set(4, -3, 8, facing(hook, WEST));
    b.set(2, -3, 8, wireEW); b.set(3, -3, 8, wireEW);
    uint16_t rs = B[B_REDSTONE_WIRE];
    uint16_t rsNS = setPropStr(setPropStr(rs, "north", "side"), "south", "side"), rsEW = setPropStr(setPropStr(rs, "east", "side"), "west", "side");
    b.set(5, -3, 7, rsNS); b.set(5, -3, 6, rsNS); b.set(5, -3, 5, rsNS); b.set(5, -3, 4, rsNS); b.set(5, -3, 3, rsNS);
    b.set(5, -3, 2, rsNS); b.set(5, -3, 1, setPropStr(setPropStr(rs, "south", "side"), "west", "side")); b.set(4, -3, 1, rsEW);
    b.set(3, -3, 1, B[B_MOSSY_COBBLESTONE]);
    b.dispenser(3, -2, 1, NORTH, "jungle_temple_dispenser");
    b.set(3, -2, 2, setBool(B[B_VINE], "south", true));
    // the second trap
    b.set(7, -3, 1, facing(hook, NORTH)); b.set(7, -3, 5, facing(hook, SOUTH));
    b.set(7, -3, 2, wireNS); b.set(7, -3, 3, wireNS); b.set(7, -3, 4, wireNS);
    b.set(8, -3, 6, rsEW); b.set(9, -3, 6, setPropStr(setPropStr(rs, "west", "side"), "north", "side"));
    b.set(9, -3, 5, setPropStr(setPropStr(rs, "south", "side"), "north", "up"));
    b.set(9, -3, 4, B[B_MOSSY_COBBLESTONE]);
    b.set(9, -2, 4, setPropStr(rs, "south", "side"));
    b.dispenser(9, -2, 3, WEST, "jungle_temple_dispenser");
    b.set(8, -1, 3, setBool(B[B_VINE], "east", true)); b.set(8, -2, 3, setBool(B[B_VINE], "east", true));
    b.chest(8, -3, 3, "jungle_temple", 0);
    b.set(9, -3, 2, B[B_MOSSY_COBBLESTONE]); b.set(8, -3, 1, B[B_MOSSY_COBBLESTONE]); b.set(4, -3, 5, B[B_MOSSY_COBBLESTONE]);
    b.set(5, -2, 5, B[B_MOSSY_COBBLESTONE]); b.set(5, -1, 5, B[B_MOSSY_COBBLESTONE]); b.set(6, -3, 5, B[B_MOSSY_COBBLESTONE]);
    b.set(7, -2, 5, B[B_MOSSY_COBBLESTONE]); b.set(7, -1, 5, B[B_MOSSY_COBBLESTONE]); b.set(8, -3, 5, B[B_MOSSY_COBBLESTONE]);
    M(9, -1, 1, 9, -1, 5);
    // the hidden room: three levers, a piston door
    b.air(8, -3, 8, 10, -1, 10);
    b.set(8, -2, 11, B[B_CHISELED_STONE_BRICKS]); b.set(9, -2, 11, B[B_CHISELED_STONE_BRICKS]); b.set(10, -2, 11, B[B_CHISELED_STONE_BRICKS]);
    uint16_t lever = setPropStr(facing(B[B_LEVER], NORTH), "face", "wall");
    b.set(8, -2, 12, lever); b.set(9, -2, 12, lever); b.set(10, -2, 12, lever);
    M(8, -3, 8, 8, -3, 10); M(10, -3, 8, 10, -3, 10);
    b.set(10, -2, 9, B[B_MOSSY_COBBLESTONE]);
    b.set(8, -2, 9, rsNS); b.set(8, -2, 10, rsNS); b.set(10, -1, 9, rsNS);
    b.set(9, -2, 8, setPropStr(B[B_STICKY_PISTON], "facing", "up"));
    b.set(10, -2, 8, facing(B[B_STICKY_PISTON], WEST)); b.set(10, -1, 8, facing(B[B_STICKY_PISTON], WEST));
    b.set(10, -2, 10, facing(B[B_REPEATER], NORTH));
    b.chest(9, -3, 10, "jungle_temple", 1);
}

// ---- the swamp hut (SwampHutPiece, 7 x 7 x 9)
void buildSwampHut(Build& b) {
    uint16_t P = B[B_SPRUCE_PLANKS], LOG = B[B_OAK_LOG], F = B[B_OAK_FENCE];
    b.box(1, 1, 1, 5, 1, 7, P, P);
    b.box(1, 4, 2, 5, 4, 7, P, P);
    b.box(2, 1, 0, 4, 1, 0, P, P);
    b.box(2, 2, 2, 3, 3, 2, P, P);
    b.box(1, 2, 3, 1, 3, 6, P, P);
    b.box(5, 2, 3, 5, 3, 6, P, P);
    b.box(2, 2, 7, 4, 3, 7, P, P);
    b.box(1, 0, 2, 1, 3, 2, LOG, LOG); b.box(5, 0, 2, 5, 3, 2, LOG, LOG);
    b.box(1, 0, 7, 1, 3, 7, LOG, LOG); b.box(5, 0, 7, 5, 3, 7, LOG, LOG);
    b.set(2, 3, 2, F); b.set(3, 3, 7, F);
    b.set(1, 3, 4, 0); b.set(5, 3, 4, 0); b.set(5, 3, 5, 0);
    b.set(1, 3, 5, B[B_POTTED_RED_MUSHROOM]);
    b.set(3, 2, 6, B[B_CRAFTING_TABLE]);
    b.set(4, 2, 6, B[B_CAULDRON]);
    b.set(1, 2, 1, F); b.set(5, 2, 1, F);
    uint16_t sN = facing(B[B_SPRUCE_STAIRS], NORTH), sE = facing(B[B_SPRUCE_STAIRS], EAST),
             sW = facing(B[B_SPRUCE_STAIRS], WEST), sS = facing(B[B_SPRUCE_STAIRS], SOUTH);
    b.box(0, 4, 1, 6, 4, 1, sN, sN);
    b.box(0, 4, 2, 0, 4, 7, sE, sE);
    b.box(6, 4, 2, 6, 4, 7, sW, sW);
    b.box(0, 4, 8, 6, 4, 8, sS, sS);
    b.set(0, 4, 1, setPropStr(sN, "shape", "outer_right")); b.set(6, 4, 1, setPropStr(sN, "shape", "outer_left"));
    b.set(0, 4, 8, setPropStr(sS, "shape", "outer_left")); b.set(6, 4, 8, setPropStr(sS, "shape", "outer_right"));
    for (int z = 2; z <= 7; z += 5)
        for (int x = 1; x <= 5; x += 4) b.columnDown(LOG, x, -1, z);
}

// ---- buried treasure (BuriedTreasurePiece): a chest under the sand where stone begins
void buildBuriedTreasure(Build& b) {
    Chunk& c = b.c;
    int X = b.p.box.x0, Z = b.p.box.z0;
    if (!b.clip.inside(X, b.clip.y0 + 1, Z)) return;
    int lx = X & 15, lz = Z & 15;
    int y = c.minY();
    for (int k = c.maxY(); k > c.minY(); k--) {   // OCEAN_FLOOR_WG: the first solid block from the top
        uint16_t s = c.get(lx, k, lz);
        if (!stateIsAir(s) && !stateIsFluid(s)) { y = k + 1; break; }
    }
    for (; y > c.minY() + 1; y--) {
        uint16_t at = c.get(lx, y, lz), below = c.get(lx, y - 1, lz);
        uint16_t bb = blockIdOf(below);
        if (bb == blockIdOf(B[B_SANDSTONE]) || bb == blockIdOf(B[B_STONE]) || bb == blockIdOf(B[B_ANDESITE]) ||
            bb == blockIdOf(B[B_GRANITE]) || bb == blockIdOf(B[B_DIORITE])) {
            uint16_t fill = !stateIsAir(at) && !stateIsFluid(at) ? at : B[B_SAND];
            static const int DX[6] = {0, 0, 0, 0, 1, -1}, DY[6] = {-1, 1, 0, 0, 0, 0}, DZ[6] = {0, 0, -1, 1, 0, 0};
            for (int d = 0; d < 6; d++) {
                int nx = lx + DX[d], ny = y + DY[d], nz = lz + DZ[d];
                if (nx < 0 || nx > 15 || nz < 0 || nz > 15) continue;
                uint16_t s = c.get(nx, ny, nz);
                if (!stateIsAir(s) && !stateIsFluid(s)) continue;
                uint16_t sb = c.get(nx, ny - 1, nz);
                c.set(nx, ny, nz, (stateIsAir(sb) || stateIsFluid(sb)) && d != 1 ? below : fill);
            }
            Piece p = b.p;
            p.box = {X, y, Z, X, y, Z};
            Build at2(c, p, b.seed);
            at2.chest(0, 0, 0, "buried_treasure", 0);
            return;
        }
    }
}

}  // namespace

// ------------------------------------------------------------------ Structures
Structures::~Structures() {
    if (cache_) {
        for (int i = 0; i < CACHE; i++) plat::bigFree(cache_[i].start);
        plat::bigFree(cache_);
    }
    plat::bigFree(rings_);
    if (lock_) plat::mutexDestroy(lock_);
}

void structuresInit() { resolveBlocks(); }

bool Structures::init(uint64_t seed, uint8_t dim, uint8_t version) {
    seed_ = seed;
    dim_ = dim;
    version_ = version;
    resolveBlocks();
    if (!lock_) lock_ = plat::mutexCreate();
    if (!cache_) {
        cache_ = (Entry*)plat::bigAlloc(sizeof(Entry) * CACHE);
        if (!cache_) return false;
        for (int i = 0; i < CACHE; i++) cache_[i] = Entry();
    }
    initRings();
    return lock_ != nullptr;
}

// ChunkGeneratorStructureState#generateRingPositions: the rings' places from the world
// seed (each later moved to a preferred biome nearby: snapRing)
void Structures::initRings() {
    if (rings_ || dim_ != DIM_OVERWORLD) return;
    for (int s = 0; s < SD_NUM_SETS; s++) {
        const StructureSetDef& sd = SD_SETS[s];
        if (!sd.rings) continue;
        uint8_t since = typeSince(SD_STRUCTURES[sd.first].type);
        if (!since || version_ < since) continue;
        ringSet_ = s;
        numRings_ = sd.ringCount;
        rings_ = (Ring*)plat::bigAlloc(sizeof(Ring) * numRings_);
        if (!rings_) {
            numRings_ = 0;
            return;
        }
        JavaRandom r((int64_t)seed_);
        double angle = r.nextDouble() * 3.141592653589793 * 2.0;
        int distance = sd.ringDistance, spread = sd.ringSpread, placed = 0, ring = 0;
        for (int i = 0; i < numRings_; i++) {
            double dist = (double)(4 * distance + distance * ring * 6) + (r.nextDouble() - 0.5) * (double)distance * 2.5;
            Ring& g = rings_[i];
            g.x = (int32_t)floor(cos(angle) * dist + 0.5);   // Math.round
            g.z = (int32_t)floor(sin(angle) * dist + 0.5);
            g.fork = r.nextLong();
            g.snapped = false;
            angle += 3.141592653589793 * 2.0 / (double)spread;
            if (++placed == spread) {
                ring++;
                placed = 0;
                spread += 2 * spread / (ring + 1);
                if (spread > numRings_ - i) spread = numRings_ - i;
                angle += r.nextDouble() * 3.141592653589793 * 2.0;
            }
        }
        return;
    }
}

// BiomeSource#findBiomeHorizontal(x * 16 + 8, 0, z * 16 + 8, 112, preferred, fork): over the
// square of quarts 112 blocks around, a matching one at random (reservoir sampling)
const Structures::Ring& Structures::snapRing(int i, TerrainProbe& probe) {
    {
        LockGuard g(lock_);
        if (rings_[i].snapped) return rings_[i];
    }
    Ring g = rings_[i];
    const StructureSetDef& sd = SD_SETS[ringSet_];
    JavaRandom r(g.fork);
    int qx0 = (g.x * 16 + 8) >> 2, qz0 = (g.z * 16 + 8) >> 2, k = 112 >> 2;
    int found = 0, bx = 0, bz = 0;
    for (int dz = -k; dz <= k; dz++)
        for (int dx = -k; dx <= k; dx++) {
            int qx = qx0 + dx, qz = qz0 + dz;
            if (!hasBit(sd.preferred, probe.biome(qx * 4, 0, qz * 4))) continue;
            if (found == 0 || r.nextInt(found + 1) == 0) {
                bx = qx * 4;
                bz = qz * 4;
            }
            found++;
        }
    g.sx = found ? bx >> 4 : g.x;
    g.sz = found ? bz >> 4 : g.z;
    g.snapped = true;
    LockGuard lk(lock_);
    rings_[i] = g;
    return rings_[i];
}

namespace {
// the dimension a set's structures belong to (by their biomes)
uint8_t biomeDim(uint8_t b) {
    using namespace biome;
    if (b == NetherWastes || b == SoulSandValley || b == CrimsonForest || b == WarpedForest || b == BasaltDeltas) return DIM_NETHER;
    if (b == TheEnd || b == EndHighlands || b == EndMidlands || b == SmallEndIslands || b == EndBarrens) return DIM_END;
    return DIM_OVERWORLD;
}
bool setInDim(int s, uint8_t dim) {
    const StructureSetDef& sd = SD_SETS[s];
    for (int k = 0; k < sd.count; k++)
        for (int b = 0; b < NUM_BIOMES; b++)
            if (hasBit(SD_STRUCTURES[sd.first + k].biomes, (uint8_t)b) && biomeDim((uint8_t)b) == dim) return true;
    return false;
}
int floorDiv(int a, int b) { return (a >= 0 ? a : a - b + 1) / b; }
}  // namespace

bool Structures::isStartChunk(int s, int cx, int cz) const {
    const StructureSetDef& sd = SD_SETS[s];
    if (sd.frequency >= 1.0f) return true;
    JavaRandom r;
    int64_t seed = (int64_t)seed_;
    switch (sd.reduction) {
        case 1: {   // legacy_type_1
            int i = cx >> 4, j = cz >> 4;
            r.setSeed((int64_t)(i ^ (j << 4)) ^ seed);
            r.nextInt();
            return r.nextInt((int)(1.0f / sd.frequency)) == 0;
        }
        case 2:   // legacy_type_2
            r.setLargeFeatureWithSalt(seed, cx, cz, 10387320);
            return r.nextFloat() < sd.frequency;
        case 3:   // legacy_type_3
            r.setLargeFeatureSeed(seed, cx, cz);
            return r.nextDouble() < (double)sd.frequency;
        default:  // the arguments in vanilla's order (salt, x, z)
            r.setLargeFeatureWithSalt(seed, sd.salt, cx, cz);
            return r.nextFloat() < sd.frequency;
    }
}

namespace {
// the starts of the single-piece structures (SinglePieceStructure#findGenerationPoint)
bool lowestCorner(TerrainProbe& probe, int x, int z, int w, int d, int& out) {
    int h[4] = {probe.height(x, z, false), probe.height(x, z + d, false), probe.height(x + w, z, false), probe.height(x + w, z + d, false)};
    out = h[0];
    for (int i = 1; i < 4; i++)
        if (h[i] < out) out = h[i];
    return true;
}
// the piece's ground over its footprint (3 x 3 columns): the lowest, or the average
int groundOver(TerrainProbe& probe, const SBox& b, bool lowest, bool oceanFloor) {
    int lo = 1 << 30, sum = 0;
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 3; k++) {
            int x = b.x0 + (b.x1 - b.x0) * i / 2, z = b.z0 + (b.z1 - b.z0) * k / 2;
            int h = probe.height(x, z, oceanFloor);
            sum += h;
            if (h < lo) lo = h;
        }
    return lowest ? lo : sum / 9;
}
}  // namespace

const Structures::Start* Structures::evaluate(int s, int scx, int scz, TerrainProbe& probe) {
    const StructureSetDef& sd = SD_SETS[s];
    // ChunkGenerator#tryGenerateStructure: by weight; one that cannot stand here gives way
    int cand[16], nc = 0, total = 0;
    for (int k = 0; k < sd.count && nc < 16; k++) {
        cand[nc++] = sd.first + k;
        total += SD_STRUCTURES[sd.first + k].weight;
    }
    JavaRandom pick;
    if (nc > 1) pick.setLargeFeatureSeed((int64_t)seed_, scx, scz);
    while (nc > 0) {
        int k = 0;
        if (nc > 1) {
            int j = pick.nextInt(total);
            for (k = 0; k < nc - 1; k++) {
                j -= SD_STRUCTURES[cand[k]].weight;
                if (j < 0) break;
            }
        }
        int si = cand[k];
        const StructureDef& st = SD_STRUCTURES[si];
        uint8_t since = typeSince(st.type);
        if (!since || version_ < since) return nullptr;   // chosen, not built yet
        JavaRandom r;   // the generation context's random
        r.setLargeFeatureSeed((int64_t)seed_, scx, scz);
        int x0 = scx * 16, z0 = scz * 16;
        Start* out = nullptr;
        switch (st.type) {
            case ST_DESERT_PYRAMID: case ST_JUNGLE_TEMPLE: case ST_SWAMP_HUT: {
                int w = st.type == ST_DESERT_PYRAMID ? 21 : st.type == ST_JUNGLE_TEMPLE ? 12 : 7;
                int d = st.type == ST_DESERT_PYRAMID ? 21 : st.type == ST_JUNGLE_TEMPLE ? 15 : 9;
                int h = st.type == ST_DESERT_PYRAMID ? 15 : st.type == ST_JUNGLE_TEMPLE ? 10 : 7;
                // the biome first (the cheaper test; neither draws a random number)
                int cy = probe.height(x0 + 8, z0 + 8, false);
                if (!hasBit(st.biomes, probe.biome(x0 + 8, cy, z0 + 8))) break;
                int low;
                lowestCorner(probe, x0, z0, w, d, low);
                if (low < SEA_LEVEL) break;
                int orient = r.nextInt(4);
                out = newStart(1);
                if (!out) break;
                Piece& p = out->pieces[0];
                p.box = makeBox(x0, 64, z0, orient, w, h, d);
                p.orient = (uint8_t)orient;
                p.kind = st.type == ST_DESERT_PYRAMID ? PK_DESERT_PYRAMID : st.type == ST_JUNGLE_TEMPLE ? PK_JUNGLE_TEMPLE : PK_SWAMP_HUT;
                // the piece's height: the lowest ground less 0 to 2 (the pyramid), the average (the others)
                int y = st.type == ST_DESERT_PYRAMID ? groundOver(probe, p.box, true, true) - r.nextInt(3) : groundOver(probe, p.box, false, false);
                p.box.moveY(y - p.box.y0);
                out->n = 1;
                break;
            }
            case ST_STRONGHOLD: {
                // StrongholdStructure: the biome at the chunk's corner, y 0; its pieces until one
                // layout has a portal room
                if (!hasBit(st.biomes, probe.biome(x0, 0, z0))) break;
                Piece* tmp = (Piece*)plat::bigAlloc(sizeof(Piece) * 400);
                if (!tmp) break;
                int n = strongholdPieces((int64_t)seed_, scx, scz, tmp, 400);
                if (n > 0 && (out = newStart(n)) != nullptr) {
                    memcpy(out->pieces, tmp, sizeof(Piece) * n);
                    out->n = (uint16_t)n;
                }
                plat::bigFree(tmp);
                break;
            }
            case ST_FORTRESS: {
                // NetherFortressStructure: the biome at the chunk's corner, y 64
                if (!hasBit(st.biomes, probe.biome(x0, 64, z0))) break;
                Piece* tmp = (Piece*)plat::bigAlloc(sizeof(Piece) * 600);
                if (!tmp) break;
                int n = fortressPieces((int64_t)seed_, scx, scz, tmp, 600);
                if (n > 0 && (out = newStart(n)) != nullptr) {
                    memcpy(out->pieces, tmp, sizeof(Piece) * n);
                    out->n = (uint16_t)n;
                }
                plat::bigFree(tmp);
                break;
            }
            case ST_MINESHAFT: {
                // MineshaftStructure: its pieces first (the height they end at decides the biome
                // asked for: the room's chunk middle at y 50 moved with them)
                Piece* tmp = (Piece*)plat::bigAlloc(sizeof(Piece) * 400);
                if (!tmp) break;
                int lift = 0;
                auto ground = [](void* ctx, int x, int z) { return ((TerrainProbe*)ctx)->height(x, z, false); };
                int n = mineshaftPieces((int64_t)seed_, scx, scz, st.param == 1, tmp, 400, lift, ground, &probe);
                if (n > 0 && hasBit(st.biomes, probe.biome(x0 + 8, 50 + lift, z0)) && (out = newStart(n)) != nullptr) {
                    memcpy(out->pieces, tmp, sizeof(Piece) * n);
                    out->n = (uint16_t)n;
                }
                plat::bigFree(tmp);
                break;
            }
            case ST_BURIED_TREASURE: {
                int cy = probe.height(x0 + 8, z0 + 8, true);
                if (!hasBit(st.biomes, probe.biome(x0 + 8, cy, z0 + 8))) break;
                out = newStart(1);
                if (!out) break;
                Piece& p = out->pieces[0];
                p.box = {x0 + 9, 90, z0 + 9, x0 + 9, 90, z0 + 9};
                p.kind = PK_BURIED_TREASURE;
                out->n = 1;
                break;
            }
            default: break;
        }
        if (out) {
            out->structure = (uint8_t)si;
            out->cx = scx;
            out->cz = scz;
            out->seed = r.nextLong();
            out->box = out->pieces[0].box;
            for (int i = 1; i < out->n; i++) {
                const SBox& b = out->pieces[i].box;
                if (b.x0 < out->box.x0) out->box.x0 = b.x0;
                if (b.y0 < out->box.y0) out->box.y0 = b.y0;
                if (b.z0 < out->box.z0) out->box.z0 = b.z0;
                if (b.x1 > out->box.x1) out->box.x1 = b.x1;
                if (b.y1 > out->box.y1) out->box.y1 = b.y1;
                if (b.z1 > out->box.z1) out->box.z1 = b.z1;
            }
            if (st.type == ST_BURIED_TREASURE) {   // its chest goes wherever the stone begins
                out->box.y0 = -2048;
                out->box.y1 = 2048;
            }
            return out;
        }
        total -= st.weight;   // ArrayList#remove(k)
        for (int i = k; i < nc - 1; i++) cand[i] = cand[i + 1];
        nc--;
    }
    return nullptr;
}

StructureList* Structures::find(int cx, int cz, TerrainProbe& probe) {
    uint64_t t0 = plat::micros();
    structureStats = {};
    if (!cache_) return nullptr;
    const SBox chunk = {cx * 16, -4096, cz * 16, cx * 16 + 15, 4096, cz * 16 + 15};
    Start* found[32];
    int n = 0;
    for (int s = 0; s < SD_NUM_SETS && n < 32; s++) {
        const StructureSetDef& sd = SD_SETS[s];
        if (!setInDim(s, dim_)) continue;
        int reach = 0;
        bool any = false;
        for (int k = 0; k < sd.count; k++) {
            uint8_t t = SD_STRUCTURES[sd.first + k].type, since = typeSince(t);
            if (since && version_ >= since) any = true;
            if (typeReach(t) > reach) reach = typeReach(t);
        }
        if (!any) continue;
        // the candidate start chunks within reach: a random spread's one per region, or the
        // rings' places (moved to their biome first)
        int cands[64][2], ncand = 0;
        if (sd.rings) {
            if (s != ringSet_) continue;
            for (int i = 0; i < numRings_ && ncand < 64; i++) {
                if (abs(rings_[i].x - cx) > reach + 8 || abs(rings_[i].z - cz) > reach + 8) continue;   // 112 blocks of search
                const Ring& g = snapRing(i, probe);
                if (abs(g.sx - cx) <= reach && abs(g.sz - cz) <= reach) {
                    cands[ncand][0] = g.sx;
                    cands[ncand++][1] = g.sz;
                }
            }
        } else {
            int sp = sd.spacing, lim = sd.spacing - sd.separation;
            for (int rz = floorDiv(cz - reach, sp); rz <= floorDiv(cz + reach, sp); rz++)
                for (int rx = floorDiv(cx - reach, sp); rx <= floorDiv(cx + reach, sp) && ncand < 64; rx++) {
                    // RandomSpreadStructurePlacement#getPotentialStructureChunk
                    JavaRandom r;
                    r.setLargeFeatureWithSalt((int64_t)seed_, rx, rz, sd.salt);
                    int ox, oz;
                    if (sd.triangular) {
                        ox = (r.nextInt(lim) + r.nextInt(lim)) / 2;
                        oz = (r.nextInt(lim) + r.nextInt(lim)) / 2;
                    } else {
                        ox = r.nextInt(lim);
                        oz = r.nextInt(lim);
                    }
                    int scx = rx * sp + ox, scz = rz * sp + oz;
                    if (abs(scx - cx) > reach || abs(scz - cz) > reach || !isStartChunk(s, scx, scz)) continue;
                    cands[ncand][0] = scx;
                    cands[ncand++][1] = scz;
                }
        }
        for (int ci = 0; ci < ncand; ci++) {
            {
                int scx = cands[ci][0], scz = cands[ci][1];
                structureStats.starts++;
                // cached: a start is evaluated once (outside the lock: the probe is slow)
                uint32_t h = ((uint32_t)scx * 73856093u ^ (uint32_t)scz * 19349663u ^ (uint32_t)s * 83492791u) % CACHE;
                Start* copy = nullptr;
                bool hit = false;
                {
                    LockGuard g(lock_);
                    Entry& e = cache_[h];
                    if (e.set == s && e.cx == scx && e.cz == scz) {
                        hit = true;
                        if (e.start && e.start->box.hits(chunk)) {
                            copy = (Start*)plat::bigAlloc(startSize(e.start));
                            if (copy) memcpy(copy, e.start, startSize(e.start));
                        }
                    }
                }
                if (!hit) {
                    structureStats.evaluated++;
                    Start* st = (Start*)evaluate(s, scx, scz, probe);
                    LockGuard g(lock_);
                    Entry& e = cache_[h];
                    plat::bigFree(e.start);
                    e.set = (int16_t)s;
                    e.cx = scx;
                    e.cz = scz;
                    e.start = st;
                    if (st && st->box.hits(chunk)) {
                        copy = (Start*)plat::bigAlloc(startSize(st));
                        if (copy) memcpy(copy, st, startSize(st));
                    }
                }
                if (copy) {
                    if (n < 32) found[n++] = copy;
                    else plat::bigFree(copy);
                }
            }
        }
    }
    structureStats.findUs = (uint32_t)(plat::micros() - t0);
    if (!n) return nullptr;
    StructureList* list = (StructureList*)plat::bigAlloc(sizeof(StructureList) + sizeof(Start*) * n);
    if (!list) {
        for (int i = 0; i < n; i++) plat::bigFree(found[i]);
        return nullptr;
    }
    list->n = n;
    for (int i = 0; i < n; i++) list->starts[i] = found[i];
    return list;
}

void Structures::place(Chunk& c, StructureList* list) {
    if (!list) return;
    uint64_t t0 = plat::micros();
    const SBox chunk = {c.cx * 16, c.minY(), c.cz * 16, c.cx * 16 + 15, c.maxY(), c.cz * 16 + 15};
    for (int i = 0; i < list->n; i++) {
        Start* s = list->starts[i];
        for (int k = 0; k < s->n; k++) {
            const Piece& p = s->pieces[k];
            if (!p.box.hits(chunk) && p.kind != PK_BURIED_TREASURE) continue;
            structureStats.pieces++;
            int64_t ps = s->seed ^ (int64_t)k * 0x5DEECE66DLL;
            Build b(c, p, ps);
            JavaRandom r(ps);
            switch (p.kind) {
                case PK_DESERT_PYRAMID: buildDesertPyramid(b); break;
                case PK_JUNGLE_TEMPLE: buildJungleTemple(b, r); break;
                case PK_SWAMP_HUT: buildSwampHut(b); break;
                case PK_BURIED_TREASURE: buildBuriedTreasure(b); break;
                case PK_SH_STRAIGHT: case PK_SH_PRISON_HALL: case PK_SH_LEFT_TURN: case PK_SH_RIGHT_TURN:
                case PK_SH_ROOM_CROSSING: case PK_SH_STRAIGHT_STAIRS_DOWN: case PK_SH_STAIRS_DOWN: case PK_SH_FIVE_CROSSING:
                case PK_SH_CHEST_CORRIDOR: case PK_SH_LIBRARY: case PK_SH_PORTAL_ROOM: case PK_SH_FILLER_CORRIDOR:
                    buildStronghold(b);
                    break;
                case PK_NF_BRIDGE_STRAIGHT: case PK_NF_BRIDGE_END_FILLER: case PK_NF_BRIDGE_CROSSING: case PK_NF_ROOM_CROSSING:
                case PK_NF_STAIRS_ROOM: case PK_NF_MONSTER_THRONE: case PK_NF_CASTLE_ENTRANCE: case PK_NF_CORRIDOR:
                case PK_NF_CORRIDOR_CROSSING: case PK_NF_CORRIDOR_RIGHT_TURN: case PK_NF_CORRIDOR_LEFT_TURN:
                case PK_NF_CORRIDOR_STAIRS: case PK_NF_CORRIDOR_T_BALCONY: case PK_NF_STALK_ROOM:
                    buildFortress(b);
                    break;
                case PK_MS_ROOM: case PK_MS_ENTRANCE: case PK_MS_CORRIDOR: case PK_MS_CROSSING: case PK_MS_STAIRS:
                    buildMineshaft(b);
                    break;
                default: break;
            }
            structureStats.blocks += b.blocks;
        }
        plat::bigFree(s);
    }
    plat::bigFree(list);
    structureStats.placeUs = (uint32_t)(plat::micros() - t0);
}

bool Structures::locate(const char* name, int x, int z, int radius, TerrainProbe& probe, int& outX, int& outZ) {
    int si = -1, set = -1;
    for (int i = 0; i < SD_NUM_STRUCTURES; i++)
        if (!strcmp(SD_STRUCTURES[i].name, name)) si = i;
    for (int s = 0; s < SD_NUM_SETS && si >= 0; s++)
        if (si >= SD_SETS[s].first && si < SD_SETS[s].first + SD_SETS[s].count) set = s;
    if (set < 0 || !cache_) return false;
    const StructureSetDef& sd = SD_SETS[set];
    if (sd.rings) {   // the nearest ring place (moved to its biome) whose stronghold stands
        if (set != ringSet_) return false;
        int cx = x >> 4, cz = z >> 4;
        int64_t best = -1;
        bool seen[256] = {};
        for (int pass = 0; pass < numRings_ && pass < 256; pass++) {
            // the nearest place not looked at yet; done when even its search square is farther
            int bi = -1;
            int64_t bd = 0;
            for (int i = 0; i < numRings_; i++) {
                if (i >= 256 || seen[i]) continue;
                int64_t d = (int64_t)(rings_[i].x - cx) * (rings_[i].x - cx) + (int64_t)(rings_[i].z - cz) * (rings_[i].z - cz);
                if (bi < 0 || d < bd) { bi = i; bd = d; }
            }
            if (bi < 0) break;
            double reachD = sqrt((double)bd) - 8;
            if (best >= 0 && reachD > 0 && reachD * reachD > (double)best) break;
            if (sqrt((double)bd) * 16 > radius + 128) break;
            const Ring& g = snapRing(bi, probe);
            int64_t d = (int64_t)(g.sx - cx) * (g.sx - cx) + (int64_t)(g.sz - cz) * (g.sz - cz);
            if (best < 0 || d < best) {
                best = d;
                outX = g.sx;
                outZ = g.sz;
            }
            seen[bi] = true;
        }
        return best >= 0;
    }
    int cx = x >> 4, cz = z >> 4, sp = sd.spacing, lim = sd.spacing - sd.separation;
    int64_t best = -1;
    // ring by ring of regions around the position
    int R = (radius >> 4) / sp + 1;   // radius in blocks
    for (int ring = 0; ring <= R; ring++) {
        for (int rz = floorDiv(cz, sp) - ring; rz <= floorDiv(cz, sp) + ring; rz++)
            for (int rx = floorDiv(cx, sp) - ring; rx <= floorDiv(cx, sp) + ring; rx++) {
                if (abs(rx - floorDiv(cx, sp)) != ring && abs(rz - floorDiv(cz, sp)) != ring) continue;
                JavaRandom r;
                r.setLargeFeatureWithSalt((int64_t)seed_, rx, rz, sd.salt);
                int ox = sd.triangular ? (r.nextInt(lim) + r.nextInt(lim)) / 2 : r.nextInt(lim);
                int oz = sd.triangular ? (r.nextInt(lim) + r.nextInt(lim)) / 2 : r.nextInt(lim);
                int scx = rx * sp + ox, scz = rz * sp + oz;
                if (!isStartChunk(set, scx, scz)) continue;
                // through the cache: what /locate finds, the chunks there need anyway
                uint32_t h = ((uint32_t)scx * 73856093u ^ (uint32_t)scz * 19349663u ^ (uint32_t)set * 83492791u) % CACHE;
                int found = -2;   // the structure there, -1 none, -2 not cached
                {
                    LockGuard g(lock_);
                    const Entry& e = cache_[h];
                    if (e.set == set && e.cx == scx && e.cz == scz) found = e.start ? e.start->structure : -1;
                }
                if (found == -2) {
                    Start* st = (Start*)evaluate(set, scx, scz, probe);
                    found = st ? st->structure : -1;
                    LockGuard g(lock_);
                    Entry& e = cache_[h];
                    plat::bigFree(e.start);
                    e.set = (int16_t)set;
                    e.cx = scx;
                    e.cz = scz;
                    e.start = st;
                }
                if (found == si) {
                    int64_t d = (int64_t)(scx - cx) * (scx - cx) + (int64_t)(scz - cz) * (scz - cz);
                    if (best < 0 || d < best) {
                        best = d;
                        outX = scx;
                        outZ = scz;
                    }
                }
            }
        if (best >= 0) return true;
    }
    return false;
}

}  // namespace vanilla
}  // namespace mc
