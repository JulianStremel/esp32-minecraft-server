// The classic generator's structures that reach over chunks (villages, strongholds, the
// Nether's and the End's buildings): starts on a grid of regions, each a pure function of
// the world seed and its start chunk (the terrain from Generator::column, never from
// generated chunks), cached, their pieces placed clipped to each chunk.
#pragma once
#include <stdint.h>
#include "mc/world/chunk.h"
#include "mc/world/vanilla/structure_build.h"

namespace mc {
class Generator;
struct ColumnInfo;
namespace classic {

enum Type : uint8_t { T_VILLAGE, T_STRONGHOLD, T_NUM };

// a classic piece kind (Piece::kind; PK_TEMPLATE pieces use vanilla's placement)
enum ClassicPiece : uint8_t {
    CP_ROAD = 200,   // a straight road: box over it (y0: unused), a = width, b = material
    CP_LAMP,         // a lamp post at box.x0, z0 (its y from the ground)
    CP_BELL,         // the meeting point's bell at box.x0, y0, z0
    CP_VILLAGERS,    // a = how many, around box.x0, y0, z0
    CP_ENTRANCE,     // a stronghold's shaft: box over its opening, from the top step (y0) to the ground
                     // (y1); orient: the way the stairs go down; a ladder, a broken arch
};

class Structures {
public:
    Structures() = default;
    ~Structures();
    Structures(const Structures&) = delete;
    Structures& operator=(const Structures&) = delete;
    bool init(uint64_t seed, uint8_t dim, const Generator* gen);
    // places the pieces reaching chunk c (after the decoration)
    void place(Chunk& c, const ColumnInfo* cols);
    // the start chunk of the nearest structure of this name ("village") within radius blocks
    bool locate(const char* name, int x, int z, int radius, int& cx, int& cz);

    struct Start {
        uint8_t type;
        int32_t cx, cz;
        vanilla::sb::SBox box;
        uint16_t n;
        vanilla::sb::Piece pieces[1];
    };

private:
    struct Entry { int32_t cx, cz; int16_t type; Start* start; };
    const Start* start(int type, int cx, int cz);   // cached; nullptr: none there
    bool candidate(int type, int rx, int rz, int& cx, int& cz) const;   // the region's start chunk
    Start* evaluate(int type, int cx, int cz) const;
    uint64_t seed_ = 0;
    uint8_t dim_ = 0;
    const Generator* gen_ = nullptr;
    void* lock_ = nullptr;
    Entry* cache_ = nullptr;
};

// villages (village.cpp): the pieces of a village starting in chunk (cx, cz), at most max
int villagePieces(const Generator& g, uint64_t seed, int cx, int cz, vanilla::sb::Piece* out, int max);
// strongholds (stronghold.cpp): the End portal's room among vanilla's stronghold rooms
int strongholdPieces(const Generator& g, uint64_t seed, int cx, int cz, vanilla::sb::Piece* out, int max);

}  // namespace classic
}  // namespace mc
