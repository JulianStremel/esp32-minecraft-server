// Rails: shapes, power along powered and activator rails, support (rails.cpp).
#pragma once
#include <stdint.h>

namespace mc {
class Server;

namespace rails {

// RailShape, in the order of the block states' "shape" values
enum Shape {
    NORTH_SOUTH = 0, EAST_WEST, ASC_EAST, ASC_WEST, ASC_NORTH, ASC_SOUTH, SOUTH_EAST, SOUTH_WEST, NORTH_WEST, NORTH_EAST
};

bool isRail(uint16_t st);                 // rail, powered, detector or activator rail
bool straightOnly(uint16_t st);           // all but the plain rail: no curves
int shapeOf(uint16_t st);                 // -1: not a rail
uint16_t withShape(uint16_t st, int shape);
bool ascending(int shape);
void exits(int shape, int a[3], int b[3]);   // the two ends, as minecarts follow them

uint16_t placementShape(uint16_t st, bool eastWest);
void placed(Server& s, int x, int y, int z);                       // joins its neighbours
bool supported(Server& s, uint16_t st, int x, int y, int z);
void neighbourChanged(Server& s, int x, int y, int z, uint16_t st);
void updatePower(Server& s, int x, int y, int z);

}  // namespace rails
}  // namespace mc
