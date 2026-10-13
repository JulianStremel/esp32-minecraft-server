// Vanilla's structure templates, template pools and processor lists (compiled by
// tools/worldgen/compile_templates.js into template_data.cpp), and their placement
// (templates.cpp: StructureTemplate#placeInWorld with its processors) and assembly
// (jigsaw.cpp: JigsawPlacement).
#pragma once
#include <stdint.h>

namespace mc {
namespace vanilla {

struct TdSet { uint16_t first, count; };
struct TdTemplate {
    uint16_t name;
    uint8_t sx, sy, sz;
    uint32_t data;                 // in TD_BLOB: runs of (count, palette index + 1; 0 none) over y, z, x
    uint16_t palFirst, palCount;   // in TD_PALETTE (state indices)
    uint16_t jigFirst, jigCount;
    uint16_t conFirst, conCount;   // containers with a loot table
    uint16_t entFirst, entCount;
    uint16_t markFirst, markCount;   // data markers (structure blocks in data mode)
    uint8_t palettes;                // palettes of palCount states each, one after the other
};
// a jigsaw block: orientation's index in "down_east" .. "south_up" (JigsawBlock's FrontAndTop)
struct TdJigsaw { uint8_t x, y, z, orient, joint; int8_t selection, placement; uint16_t name, target, pool, finalState; };
struct TdContainer { uint8_t x, y, z; uint16_t loot; };
struct TdEntity { int8_t x, y, z; uint16_t type; };   // may stand just outside the template
struct TdMarker { uint8_t x, y, z; uint16_t name; };
struct TdRule {
    uint8_t inType, locType, posType;
    uint16_t inArg, locArg;
    float inProb, locProb;
    float posMinChance, posMaxChance;
    int16_t posMinDist, posMaxDist;
    uint8_t posAxis;
    uint16_t output, loot;
};
struct TdProcessor { uint8_t type; uint16_t firstRule, rules, set; float integrity; };
struct TdProcList { uint16_t first, count; };
enum TdElementType : uint8_t { TE_EMPTY, TE_SINGLE, TE_LEGACY, TE_LIST, TE_FEATURE };
struct TdElement { uint8_t type, projection; uint16_t ref, count, processors, feature, weight; };
struct TdPool { uint16_t name; int16_t fallback; uint16_t first, count; };

extern const char* const TD_STRINGS[];
extern const int TD_NUM_STRINGS;
extern const char* const TD_STATES[];
extern const int TD_NUM_STATES;
extern const TdSet TD_SETS[];
extern const uint16_t TD_SET_NAMES[];
extern const TdTemplate TD_TEMPLATES[];
extern const int TD_NUM_TEMPLATES;
extern const uint16_t TD_PALETTE[];
extern const TdJigsaw TD_JIGSAWS[];
extern const TdContainer TD_CONTAINERS[];
extern const TdEntity TD_ENTITIES[];
extern const TdMarker TD_MARKERS[];
extern const TdRule TD_RULES[];
extern const TdProcessor TD_PROCESSORS[];
extern const TdProcList TD_PROC_LISTS[];
extern const TdElement TD_ELEMENTS[];
extern const TdElement TD_LIST_ITEMS[];
extern const TdPool TD_POOLS[];
extern const int TD_NUM_POOLS;
extern const uint8_t TD_BLOB[];

}  // namespace vanilla
}  // namespace mc
