// Vanilla's structures (generator version 10 and up): where they stand (the structure
// sets' placements from the data pack, compiled by tools/worldgen/compile_structures.js
// into structure_data.cpp), whether one is built there (its biomes, the terrain's height),
// and its pieces, placed into each chunk they reach (vanilla's StructurePiece#postProcess
// with the chunk as the box).
//
// A structure's start and pieces are a pure function of the seed and its start chunk:
// computed once with the terrain probe (the router's height and biomes, never a generated
// chunk's blocks) and cached, so every chunk it reaches places the same structure, in
// whatever order chunks are generated.
#pragma once
#include <stdint.h>

namespace mc {

class Chunk;

namespace vanilla {

enum StructType : uint8_t {
    ST_DESERT_PYRAMID, ST_JUNGLE_TEMPLE, ST_SWAMP_HUT, ST_BURIED_TREASURE, ST_IGLOO, ST_STRONGHOLD, ST_FORTRESS,
    ST_MINESHAFT, ST_OCEAN_MONUMENT, ST_JIGSAW, ST_RUINED_PORTAL, ST_SHIPWRECK, ST_OCEAN_RUIN, ST_NETHER_FOSSIL,
    ST_END_CITY, ST_WOODLAND_MANSION, ST_NUM_TYPES
};

struct StructureSetDef {
    uint8_t rings, triangular, reduction;
    int16_t spacing, separation;
    int32_t salt;
    float frequency;
    int8_t exclusionSet;
    uint8_t exclusionChunks;
    int16_t ringDistance, ringCount, ringSpread;
    uint8_t preferred[16];   // the rings' preferred biomes: a bit per biome id
    uint8_t first, count;    // in SD_STRUCTURES
};
struct JigsawDef {
    const char* startPool;     // without "minecraft:"
    const char* startJigsaw;   // the start's jigsaw put at the start position (or nullptr)
    int16_t heightMin, heightMax;
    uint8_t project, size;     // project: to WORLD_SURFACE_WG; size: the maximum depth
    uint16_t maxDistance;
    uint8_t expansionHack, padding;
    uint8_t noWaterlogging;               // liquid_settings: ignore_waterlogging
    uint8_t aliasFirst, aliasCount;       // in SD_ALIAS_BINDINGS
};
struct AliasBinding { uint8_t type; uint16_t first, count; };   // 0 direct, 1 random, 2 random group
struct AliasOption { uint16_t weight, first, count; };
struct AliasPair { const char* alias; const char* target; };   // the alias with "minecraft:", the target without
extern const AliasBinding SD_ALIAS_BINDINGS[];
extern const AliasOption SD_ALIAS_OPTIONS[];
extern const AliasPair SD_ALIAS_PAIRS[];
struct StructureDef {
    uint8_t type, step, adaptation, param, weight;
    uint8_t biomes[16];
    const char* name;
    JigsawDef jigsaw;
};
struct LootEntry { int16_t item; uint8_t weight, min, max, flags; };
struct LootPool { uint8_t rollsMin, rollsMax; uint16_t first, count; };
struct LootTable { const char* name; uint16_t firstPool, pools; };

extern const StructureSetDef SD_SETS[];
extern const int SD_NUM_SETS;
extern const StructureDef SD_STRUCTURES[];
extern const int SD_NUM_STRUCTURES;
extern const char* const SD_LOOT_ITEMS[];
extern const int SD_NUM_LOOT_ITEMS;
extern const LootEntry SD_LOOT_ENTRIES[];
extern const LootPool SD_LOOT_POOLS[];
extern const LootTable SD_LOOT_TABLES[];
extern const int SD_NUM_LOOT_TABLES;

// What placing a structure asks of the terrain; the generator answers from its router.
class TerrainProbe {
public:
    virtual ~TerrainProbe() = default;
    // The first free y above the column (vanilla's WORLD_SURFACE_WG: above the sea's
    // surface; oceanFloor: OCEAN_FLOOR_WG, above the ground under the water).
    virtual int height(int x, int z, bool oceanFloor) = 0;
    virtual uint8_t biome(int x, int y, int z) = 0;   // vanilla's getNoiseBiome at the block's quart
};

struct StructureList;   // the structures reaching one chunk (a copy: no lock while placing)

struct StructureStats { uint32_t findUs, placeUs, starts, evaluated, pieces, blocks; };
extern StructureStats structureStats;   // the last chunk's (the benchmark)

class Structures {
public:
    Structures() = default;
    ~Structures();
    Structures(const Structures&) = delete;
    Structures& operator=(const Structures&) = delete;
    // version: the generator's (the types each version builds)
    bool init(uint64_t seed, uint8_t dim, uint8_t version);
    // The structures whose pieces reach chunk (cx, cz); nullptr if none. Thread-safe.
    StructureList* find(int cx, int cz, TerrainProbe& probe);
    // Places them into the chunk (its blocks written, before the features) and frees the list.
    void place(Chunk& c, StructureList* list);
    // What of them shapes the chunk's terrain (Beardifier#forStructuresInChunk): their rigid
    // pieces and junctions within 12 blocks, with their structure's terrain adaptation; the
    // count (at most max). From version 14.
    int beard(const StructureList* list, int cx, int cz, struct BeardPiece* out, int max) const;
    // The start chunk of the nearest structure of this name (vanilla's /locate), searching
    // `radius` chunks around (x, z); false if none.
    bool locate(const char* name, int x, int z, int radius, TerrainProbe& probe, int& outX, int& outZ);

    struct Start;   // a structure's start and pieces (structures.cpp)
    struct Entry;

private:
    const Start* evaluate(int set, int scx, int scz, TerrainProbe& probe);   // cached; lock held
    bool isStartChunk(int set, int cx, int cz) const;
    uint64_t seed_ = 0;
    uint8_t dim_ = 0, version_ = 0;
    void* lock_ = nullptr;
    Entry* cache_ = nullptr;
    uint32_t tick_ = 0;
    // the strongholds' rings (ConcentricRingsStructurePlacement): positions before and
    // after the biome search, which is made when a chunk near one is first generated
    struct Ring { int32_t x, z, sx, sz; int64_t fork; bool snapped; };
    Ring* rings_ = nullptr;
    int numRings_ = 0, ringSet_ = -1;
    void initRings();
    const Ring& snapRing(int i, TerrainProbe& probe);
};

// Loot: the chest's items rolled from a vanilla loot table (by its name, "desert_pyramid").
void structuresInit();   // the item names resolved, once (Generator::init)

}  // namespace vanilla
}  // namespace mc
