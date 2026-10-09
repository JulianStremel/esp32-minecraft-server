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
    static bool isShelfBook(uint16_t item);
    // the container block entity at pos (created when missing), or nullptr
    static TileEntity* container(Server& s, PistonPos pos);
    // the crafter at pos crafts once (its scheduled tick after a rising edge)
    static void craft(Server& s, PistonPos pos);
    static bool crafterAccepts(const TileEntity& t, int slot, const ItemStack& item);
};
int furnaceFuelTicks(uint16_t item);
// crafting (inventory.cpp): the result of a size x size grid, and using it up
ItemStack matchCraftingRecipe(const ItemStack* grid, int size);
void consumeCraftingGrid(ItemStack* grid, int n);
} // namespace mc
