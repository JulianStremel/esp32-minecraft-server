#pragma once
#include <stdint.h>
namespace mc {
class Server;
class Player;
struct ItemStack;
struct TileEntity;
namespace Books {
bool isBook(const ItemStack& item);
int pages(const ItemStack& item);
bool place(Server& s, Player& p, int x, int y, int z, ItemStack& item);
void turnPage(Server& s, int x, int y, int z, int page);
void removedBook(Server& s, int x, int y, int z);
int comparator(const TileEntity& tile);
void properties(Player& p, const TileEntity& tile);
} // namespace Books
} // namespace mc
