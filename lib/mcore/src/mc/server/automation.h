#pragma once
#include "mc/server/piston.h"
#include "mc/item.h"

namespace mc {
class Server;
struct TileEntity;
class Automation {
  public:
    static uint8_t tileType(uint16_t block);
    static void hopper(Server& s, PistonPos pos);
    static void dispense(Server& s, PistonPos pos);
    static bool insertOne(Server& s, PistonPos pos, ItemStack& item, int face, TileEntity* source = nullptr);
};
int furnaceFuelTicks(uint16_t item);
} // namespace mc
