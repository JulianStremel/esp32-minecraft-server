// Item stacks and their network (slot) encoding.
#pragma once
#include <stdint.h>
#include "mc/io.h"

namespace mc {

struct ItemTag;

struct ItemStack {
    ItemStack() = default;
    ItemStack(const ItemStack& other);
    ItemStack(ItemStack&& other) noexcept;
    ItemStack& operator=(const ItemStack& other);
    ItemStack& operator=(ItemStack&& other) noexcept;
    ~ItemStack();
    uint16_t id = 0;
    uint8_t count = 0;
    uint16_t damage = 0;

    bool empty() const { return id == 0 || count == 0; }
    void clear();
    bool sameItem(const ItemStack& o) const;
    // Immutable, shared canonical NBT. Compound key order is insignificant;
    // list order is significant. Root Damage remains in the field above.
    const uint8_t* tagData() const;
    size_t tagSize() const;
    bool readTag(Reader& r); // consumes a network root, including Damage
    bool setTag(const uint8_t* data, size_t len);
    void writeTag(Writer& w) const;
    static constexpr size_t MAX_TAG_BYTES = 65536;
    static ItemStack of(uint16_t id, int count = 1) {
        ItemStack s;
        s.id = id;
        s.count = (uint8_t)count;
        return s;
    }

  private:
    ItemTag* tag_ = nullptr;
};

// Slot encoding (1.21.8): varint count, then varint id and the data components
// (damage, custom name, lore, enchantments, book contents from the stack's NBT).
void writeSlot(Writer& w, const ItemStack& s);
// An item stack sent by the client (length-prefixed components), back into NBT.
bool readSlot(Reader& r, ItemStack& s);
// An optional hashed item stack (window clicks); the server ignores the client's prediction.
bool skipHashedSlot(Reader& r);

} // namespace mc
