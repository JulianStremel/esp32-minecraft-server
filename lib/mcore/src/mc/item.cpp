#include "mc/item.h"
#include "mc/bytebuf.h"
#include "mc/nbt.h"
#include "mc/registry.h"
#include <algorithm>
#include <atomic>
#include <new>
#include <string.h>
#include <utility>

namespace mc {
struct ItemTag {
    std::atomic<uint32_t> refs{1};
    size_t size;
    const uint8_t* data() const { return (const uint8_t*)(this + 1); }
    void retain() { refs.fetch_add(1, std::memory_order_relaxed); }
    void release() {
        if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            this->~ItemTag();
            plat::bigFree(this);
        }
    }
};
namespace {
struct Entry {
    const uint8_t* name;
    const uint8_t* payload;
    size_t length;
    uint32_t sequence;
    uint16_t nameLength;
    uint8_t type;
};
int compareName(const Entry& a, const Entry& b) {
    int c = memcmp(a.name, b.name, std::min(a.nameLength, b.nameLength));
    return c ? c : (int)a.nameLength - b.nameLength;
}
// Canonicalize recursively, using spans into the validated input. Temporary entry
// arrays and output live in PSRAM; copying an inventory never copies tag bytes.
bool canonical(Reader& r, Writer& w, uint8_t type, int depth, int32_t* damage = nullptr) {
    if (depth > 24) {
        r.fail();
        return false;
    }
    if (type == NBT_COMPOUND) {
        ByteBuf entries;
        uint32_t sequence = 0;
        while (r.ok()) {
            uint8_t t = r.u8();
            if (t == NBT_END) break;
            uint16_t len = r.u16();
            const uint8_t* name = r.take(len);
            const uint8_t* payload = r.cursor();
            if (!nbtSkipPayload(r, t, depth + 1)) return false;
            Entry e{name, payload, (size_t)(r.cursor() - payload), sequence++, len, t};
            entries.put((const uint8_t*)&e, sizeof(e));
            if (entries.failed()) {
                r.fail();
                return false;
            }
        }
        if (!r.ok()) return false;
        size_t count = entries.size() / sizeof(Entry);
        Entry* es = (Entry*)entries.data();
        if (count > 1)
            std::sort(es, es + count, [](const Entry& a, const Entry& b) {
                int c = compareName(a, b);
                return c ? c < 0 : a.sequence < b.sequence;
            });
        for (size_t i = 0; i < count; ++i) {
            const Entry& e = es[i];
            // CompoundTag.load retains the last value for duplicate names.
            if (i + 1 < count && !compareName(e, es[i + 1])) continue;
            Reader sub(e.payload, e.length);
            if (damage && e.nameLength == 6 && !memcmp(e.name, "Damage", 6)) {
                switch (e.type) {
                case NBT_BYTE:
                    *damage = sub.i8();
                    break;
                case NBT_SHORT:
                    *damage = sub.i16();
                    break;
                case NBT_INT:
                    *damage = sub.i32();
                    break;
                default:
                    *damage = 0;
                    break;
                }
                continue;
            }
            w.u8(e.type);
            w.u16(e.nameLength);
            w.bytes(e.name, e.nameLength);
            if (!canonical(sub, w, e.type, depth + 1)) return false;
        }
        w.u8(NBT_END);
    } else if (type == NBT_LIST) {
        uint8_t element = r.u8();
        int32_t n = r.i32();
        if (n < 0 || element > NBT_LONG_ARRAY || (n && element == NBT_END)) {
            r.fail();
            return false;
        }
        // Empty list equality does not depend on its stored element type.
        w.u8(n ? element : (uint8_t)NBT_END);
        w.i32(n);
        for (int32_t i = 0; i < n && r.ok(); ++i)
            if (!canonical(r, w, element, depth + 1)) return false;
    } else {
        const uint8_t* start = r.cursor();
        if (!nbtSkipPayload(r, type, depth)) return false;
        w.bytes(start, (size_t)(r.cursor() - start));
    }
    return r.ok();
}
} // namespace

ItemStack::ItemStack(const ItemStack& o) : id(o.id), count(o.count), damage(o.damage), tag_(o.tag_) {
    if (tag_) tag_->retain();
}
ItemStack::ItemStack(ItemStack&& o) noexcept : id(o.id), count(o.count), damage(o.damage), tag_(o.tag_) {
    o.tag_ = nullptr;
    o.id = 0;
    o.count = 0;
    o.damage = 0;
}
ItemStack& ItemStack::operator=(const ItemStack& o) {
    if (this != &o) {
        ItemStack copy(o);
        *this = std::move(copy);
    }
    return *this;
}
ItemStack& ItemStack::operator=(ItemStack&& o) noexcept {
    if (this != &o) {
        clear();
        id = o.id;
        count = o.count;
        damage = o.damage;
        tag_ = o.tag_;
        o.tag_ = nullptr;
        o.id = 0;
        o.count = 0;
        o.damage = 0;
    }
    return *this;
}
ItemStack::~ItemStack() {
    if (tag_) tag_->release();
}
void ItemStack::clear() {
    if (tag_) tag_->release();
    tag_ = nullptr;
    id = 0;
    count = 0;
    damage = 0;
}
const uint8_t* ItemStack::tagData() const {
    return tag_ ? tag_->data() : nullptr;
}
size_t ItemStack::tagSize() const {
    return tag_ ? tag_->size : 0;
}
bool ItemStack::sameItem(const ItemStack& o) const {
    return id == o.id && damage == o.damage &&
           (tag_ == o.tag_ || (tagSize() == o.tagSize() && !memcmp(tagData(), o.tagData(), tagSize())));
}
bool ItemStack::readTag(Reader& r) {
    Reader scan = r;
    uint8_t type = scan.u8();
    if (type != NBT_END && type != NBT_COMPOUND) {
        r.fail();
        return false;
    }
    if (type == NBT_COMPOUND) {
        uint16_t name = scan.u16();
        scan.skip(name);
    }
    const uint8_t* start = scan.cursor();
    if (!nbtSkipPayload(scan, type) || (size_t)(scan.cursor() - r.cursor()) > MAX_TAG_BYTES) {
        r.fail();
        return false;
    }
    ByteBuf buf;
    Writer w(buf);
    int32_t parsedDamage = 0;
    if (type == NBT_COMPOUND) {
        w.u8(NBT_COMPOUND);
        w.u16(0);
        Reader content(start, (size_t)(scan.cursor() - start));
        if (!canonical(content, w, NBT_COMPOUND, 0, &parsedDamage)) {
            r.fail();
            return false;
        }
    }
    if (buf.failed()) {
        r.fail();
        return false;
    }
    ItemTag* tag = nullptr;
    if (buf.size() > 4) {
        void* mem = plat::bigAlloc(sizeof(ItemTag) + buf.size());
        if (!mem) {
            r.fail();
            return false;
        }
        tag = new (mem) ItemTag;
        tag->size = buf.size();
        memcpy((uint8_t*)(tag + 1), buf.data(), buf.size());
    }
    if (tag_) tag_->release();
    tag_ = tag;
    damage = (uint16_t)std::max<int32_t>(0, std::min<int32_t>(65535, parsedDamage));
    r = scan;
    return true;
}
bool ItemStack::setTag(const uint8_t* data, size_t len) {
    if (!data || !len || len > MAX_TAG_BYTES) return false;
    Reader r(data, len);
    ItemStack copy(*this);
    if (!copy.readTag(r) || r.remaining()) return false;
    *this = std::move(copy);
    return true;
}
void ItemStack::writeTag(Writer& w) const {
    if (!tag_ && !damage) {
        w.u8(NBT_END);
        return;
    }
    if (tag_)
        w.bytes(tag_->data(), tag_->size - 1);
    else {
        w.u8(NBT_COMPOUND);
        w.u16(0);
    }
    if (damage) {
        NbtWriter n(w);
        n.i32("Damage", damage);
    }
    w.u8(NBT_END);
}
void writeSlot(Writer& w, const ItemStack& s) {
    w.boolean(!s.empty());
    if (s.empty()) return;
    w.varint(s.id);
    w.i8((int8_t)s.count);
    s.writeTag(w);
}
bool readSlot(Reader& r, ItemStack& s) {
    s.clear();
    if (!r.boolean()) return r.ok();
    int32_t id = r.varint();
    int8_t count = r.i8();
    if (!s.readTag(r)) return false;
    if (id <= 0 || id >= NUM_ITEMS || count <= 0) {
        s.clear();
        return true;
    }
    s.id = (uint16_t)id;
    s.count = (uint8_t)std::min(64, (int)count);
    return true;
}
} // namespace mc
