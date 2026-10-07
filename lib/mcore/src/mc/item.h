// Item stacks and their network (slot) encoding.
#pragma once
#include <stdint.h>
#include "mc/io.h"

namespace mc {

struct ItemStack {
    uint16_t id = 0;
    uint8_t count = 0;
    uint16_t damage = 0;

    bool empty() const { return id == 0 || count == 0; }
    void clear() { id = 0; count = 0; damage = 0; }
    bool sameItem(const ItemStack& o) const { return id == o.id && damage == o.damage; }
    static ItemStack of(uint16_t id, int count = 1) { ItemStack s; s.id = id; s.count = (uint8_t)count; return s; }
};

// Slot encoding (1.16.5): bool present, varint id, byte count, NBT (TAG_End or {Damage:int}).
void writeSlot(Writer& w, const ItemStack& s);
bool readSlot(Reader& r, ItemStack& s);

}  // namespace mc
