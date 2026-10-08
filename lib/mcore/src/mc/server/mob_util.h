// Movement and targeting helpers shared by the mob code (entities.cpp, nether_mobs.cpp,
// dragon.cpp). All work in the server's current dimension (Server::curDim).
#pragma once
#include "mc/server/entity.h"
#include "mc/world/noise.h"

namespace mc {

class Server;
class Player;
class Writer;

namespace mobs {

bool solidAt(Server& s, int x, int y, int z);   // unloaded chunks count as solid
bool boxCollides(Server& s, double x, double y, double z, float w, float h);
bool inFluid(Server& s, const Entity& e, uint16_t blockId);
// Moves e by its velocity with block collisions; true if blocked horizontally.
bool moveEntity(Server& s, Entity& e);
bool isDay(const Server& s);
void lookAt(Entity& e, double tx, double tz);
// the nearest survival or adventure player in e's dimension within range
Player* nearestTarget(Server& s, const Entity& e, double range);
// no colliding block on the straight line between the two points
bool lineOfSight(Server& s, double x0, double y0, double z0, double x1, double y1, double z1);
void writeVelocity(Writer& w, double vx, double vy, double vz);
Rng& rng();

}  // namespace mobs
}  // namespace mc
