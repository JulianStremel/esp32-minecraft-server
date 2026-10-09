#include "mc/item.h"
#include "mc/bytebuf.h"
#include "mc/nbt.h"
#include "mc/registry.h"
#include "mc/text.h"
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
// ---------------------------------------------------------------- slots (1.21.8)
// On the wire an item stack is its count, id and data components. The server keeps the
// item NBT of 1.16.5 (books, names, enchantments survive in storage as before) and
// translates the parts the client shows: Damage, display.Name and Lore, Enchantments,
// StoredEnchantments and the book contents.
namespace {

int enchantmentId(const uint8_t* name, size_t len) {
    for (int i = 0; i < NUM_ENCHANTMENTS; i++) {
        const char* n = ENCHANTMENT_NAME[i];
        size_t l = strlen(n);
        if (l == len && !memcmp(n, name, len)) return i;
        if (len + 10 == l && !memcmp(n + 10, name, len)) return i;   // without "minecraft:"
    }
    return -1;
}

// Text stored as a JSON string in the tag, written as an NBT text component.
void writeJsonText(Writer& w, const uint8_t* json, size_t len) {
    char* z = (char*)plat::bigAlloc(len + 1);
    if (!z) {
        w.u8(NBT_STRING);
        w.u16(0);
        return;
    }
    memcpy(z, json, len);
    z[len] = 0;
    writeTextNbt(w, z);
    plat::bigFree(z);
}

struct TagView {   // spans into the canonical tag of a stack
    const uint8_t* name = nullptr;      // display.Name (JSON)
    size_t nameLen = 0;
    const uint8_t* lore = nullptr;      // display.Lore list payload (after type and count)
    int32_t loreCount = 0;
    const uint8_t* ench[2] = {};        // Enchantments, StoredEnchantments list payloads
    int32_t enchCount[2] = {};
    const uint8_t* pages = nullptr;     // pages list payload (strings)
    int32_t pageCount = 0;
    const uint8_t* title = nullptr;
    size_t titleLen = 0;
    const uint8_t* author = nullptr;
    size_t authorLen = 0;
    int32_t generation = 0;
};

bool keyIs(const uint8_t* k, uint16_t l, const char* s) { return strlen(s) == l && !memcmp(k, s, l); }

void viewCompound(Reader& r, TagView& v, bool display) {
    while (r.ok()) {
        uint8_t t = r.u8();
        if (t == NBT_END) return;
        uint16_t l = r.u16();
        const uint8_t* k = r.take(l);
        if (!r.ok()) return;
        if (display && t == NBT_STRING && keyIs(k, l, "Name")) {
            v.nameLen = r.u16();
            v.name = r.take(v.nameLen);
        } else if (display && t == NBT_LIST && keyIs(k, l, "Lore")) {
            uint8_t e = r.u8();
            int32_t n = r.i32();
            if (e == NBT_STRING || n == 0) { v.lore = r.cursor(); v.loreCount = n; }
            for (int32_t i = 0; i < n && r.ok(); i++) nbtSkipPayload(r, e);
        } else if (!display && t == NBT_COMPOUND && keyIs(k, l, "display")) {
            viewCompound(r, v, true);
        } else if (!display && t == NBT_LIST && (keyIs(k, l, "Enchantments") || keyIs(k, l, "StoredEnchantments"))) {
            int which = keyIs(k, l, "StoredEnchantments");
            uint8_t e = r.u8();
            int32_t n = r.i32();
            if (e == NBT_COMPOUND) { v.ench[which] = r.cursor(); v.enchCount[which] = n; }
            for (int32_t i = 0; i < n && r.ok(); i++) nbtSkipPayload(r, e);
        } else if (!display && t == NBT_LIST && keyIs(k, l, "pages")) {
            uint8_t e = r.u8();
            int32_t n = r.i32();
            if (e == NBT_STRING || n == 0) { v.pages = r.cursor(); v.pageCount = n; }
            for (int32_t i = 0; i < n && r.ok(); i++) nbtSkipPayload(r, e);
        } else if (!display && t == NBT_STRING && keyIs(k, l, "title")) {
            v.titleLen = r.u16();
            v.title = r.take(v.titleLen);
        } else if (!display && t == NBT_STRING && keyIs(k, l, "author")) {
            v.authorLen = r.u16();
            v.author = r.take(v.authorLen);
        } else if (!display && t == NBT_INT && keyIs(k, l, "generation")) {
            v.generation = r.i32();
        } else {
            nbtSkipPayload(r, t);
        }
    }
}

// One enchantment list as the enchantments component: count, then (id, level) pairs.
void writeEnchantments(Writer& w, const uint8_t* list, int32_t n, size_t end) {
    Reader count(list, end), r(list, end);
    int known = 0;
    for (int pass = 0; pass < 2; pass++) {
        Reader& rr = pass ? r : count;
        if (pass) w.varint(known);
        for (int32_t i = 0; i < n && rr.ok(); i++) {
            int id = -1, lvl = 1;
            while (rr.ok()) {
                uint8_t t = rr.u8();
                if (t == NBT_END) break;
                uint16_t l = rr.u16();
                const uint8_t* k = rr.take(l);
                if (t == NBT_STRING && keyIs(k, l, "id")) {
                    uint16_t sl = rr.u16();
                    id = enchantmentId(rr.take(sl), sl);
                } else if (keyIs(k, l, "lvl") && (t == NBT_SHORT || t == NBT_INT || t == NBT_BYTE)) {
                    lvl = t == NBT_SHORT ? rr.i16() : t == NBT_INT ? rr.i32() : rr.i8();
                } else {
                    nbtSkipPayload(rr, t);
                }
            }
            if (id < 0) continue;
            if (!pass) known++;
            else { w.varint(id); w.varint(lvl < 1 ? 1 : lvl > 255 ? 255 : lvl); }
        }
    }
}

}  // namespace

void writeSlot(Writer& w, const ItemStack& s) {
    if (s.empty()) {
        w.varint(0);
        return;
    }
    w.varint(s.count);
    w.varint(s.id);
    TagView v;
    size_t tagEnd = 0;
    if (s.tagSize() > 3) {
        Reader r(s.tagData() + 3, s.tagSize() - 3);   // skip the root's type and empty name
        viewCompound(r, v, false);
        tagEnd = s.tagSize();
    }
    const uint8_t* base = s.tagData();
    auto left = [&](const uint8_t* p) { return (size_t)(base + tagEnd - p); };
    bool writable = s.id == itm::WritableBook && v.pages, written = s.id == itm::WrittenBook && v.pages && v.title;
    int n = (s.damage > 0) + (v.name != nullptr) + (v.lore != nullptr) + (v.ench[0] != nullptr) + (v.ench[1] != nullptr) +
            writable + written;
    w.varint(n);
    w.varint(0);   // no removed components
    if (s.damage > 0) { w.varint(comp::Damage); w.varint(s.damage); }
    if (v.name) { w.varint(comp::CustomName); writeJsonText(w, v.name, v.nameLen); }
    if (v.lore) {
        w.varint(comp::Lore);
        w.varint(v.loreCount);
        Reader r(v.lore, left(v.lore));
        for (int32_t i = 0; i < v.loreCount && r.ok(); i++) {
            uint16_t l = r.u16();
            writeJsonText(w, r.take(l), l);
        }
    }
    if (v.ench[0]) { w.varint(comp::Enchantments); writeEnchantments(w, v.ench[0], v.enchCount[0], left(v.ench[0])); }
    if (v.ench[1]) { w.varint(comp::StoredEnchantments); writeEnchantments(w, v.ench[1], v.enchCount[1], left(v.ench[1])); }
    if (writable || written) {
        w.varint(writable ? comp::WritableBookContent : comp::WrittenBookContent);
        if (written) {
            w.string((const char*)v.title, v.titleLen);
            w.boolean(false);
            w.string(v.author ? (const char*)v.author : "", v.author ? v.authorLen : 0);
            w.varint(v.generation);
        }
        w.varint(v.pageCount);
        Reader r(v.pages, left(v.pages));
        for (int32_t i = 0; i < v.pageCount && r.ok(); i++) {
            uint16_t l = r.u16();
            const uint8_t* page = r.take(l);
            if (written) {
                writeJsonText(w, page, l);
                w.u8(NBT_END);   // no filtered text
            } else {
                w.string((const char*)page, l);
                w.boolean(false);
            }
        }
        if (written) w.boolean(true);   // resolved
    }
}

namespace {

void nbtKey(Writer& w, uint8_t type, const char* k) {
    w.u8(type);
    w.u16((uint16_t)strlen(k));
    w.bytes((const uint8_t*)k, strlen(k));
}
void nbtValue(Writer& w, const char* s, size_t l) {
    w.u16((uint16_t)l);
    w.bytes((const uint8_t*)s, l);
}

// A text component from the client as a JSON NBT string value.
bool textAsJsonString(Reader& r, Writer& w) {
    char* json = (char*)plat::bigAlloc(8192);
    if (!json) return false;
    bool ok = readTextNbtAsJson(r, json, 8192);
    if (ok) nbtValue(w, json, strlen(json));
    plat::bigFree(json);
    return ok;
}

}  // namespace

// An item stack from the client (creative inventory: each component's data is prefixed
// by its length). Components the server keeps are converted to the 1.16.5 item NBT.
bool readSlot(Reader& r, ItemStack& s) {
    s.clear();
    int32_t count = r.varint();
    if (count <= 0) return r.ok();
    int32_t id = r.varint();
    int32_t added = r.varint(), removed = r.varint();
    if (!r.ok() || added < 0 || removed < 0 || added > 256 || removed > 256) { r.fail(); return false; }
    ByteBuf tag;
    Writer w(tag);
    w.u8(NBT_COMPOUND);
    w.u16(0);
    int damage = 0;
    bool any = false;
    for (int32_t i = 0; i < added && r.ok(); i++) {
        int type = r.varint();
        int32_t len = r.varint();
        const uint8_t* data = r.take((size_t)(len < 0 ? 0 : len));
        if (!r.ok() || len < 0) return false;
        Reader c(data, (size_t)len);
        switch (type) {
            case comp::Damage: damage = c.varint(); break;
            case comp::CustomName: {
                nbtKey(w, NBT_COMPOUND, "display");
                nbtKey(w, NBT_STRING, "Name");
                if (!textAsJsonString(c, w)) nbtValue(w, "\"\"", 2);
                w.u8(NBT_END);
                any = true;
                break;
            }
            case comp::Enchantments:
            case comp::StoredEnchantments: {
                int32_t n = c.varint();
                if (n < 0 || n > 64) break;
                nbtKey(w, NBT_LIST, type == comp::Enchantments ? "Enchantments" : "StoredEnchantments");
                w.u8(NBT_COMPOUND);
                w.i32(n);
                for (int32_t k = 0; k < n; k++) {
                    int e = c.varint(), lvl = c.varint();
                    const char* name = e >= 0 && e < NUM_ENCHANTMENTS ? ENCHANTMENT_NAME[e] : "minecraft:unbreaking";
                    nbtKey(w, NBT_STRING, "id");
                    nbtValue(w, name, strlen(name));
                    nbtKey(w, NBT_SHORT, "lvl");
                    w.i16((int16_t)lvl);
                    w.u8(NBT_END);
                }
                any = true;
                break;
            }
            case comp::WritableBookContent:
            case comp::WrittenBookContent: {
                bool written = type == comp::WrittenBookContent;
                char title[64] = "", author[64] = "";
                int generation = 0;
                if (written) {
                    c.string(title, sizeof(title));
                    if (c.boolean()) { char skip[64]; c.string(skip, sizeof(skip)); }
                    c.string(author, sizeof(author));
                    generation = c.varint();
                }
                int32_t n = c.varint();
                if (n < 0 || n > 100 || !c.ok()) break;
                nbtKey(w, NBT_LIST, "pages");
                w.u8(NBT_STRING);
                w.i32(n);
                for (int32_t k = 0; k < n && c.ok(); k++) {
                    if (written) {
                        if (!textAsJsonString(c, w)) nbtValue(w, "\"\"", 2);
                        nbtSkipNetwork(c);   // the filtered page
                    } else {
                        int32_t l = c.varint();
                        const uint8_t* text = c.take((size_t)(l < 0 ? 0 : l));
                        nbtValue(w, (const char*)text, (size_t)l);
                        if (c.boolean()) { int32_t fl = c.varint(); c.take((size_t)(fl < 0 ? 0 : fl)); }
                    }
                }
                if (written) {
                    nbtKey(w, NBT_STRING, "title");
                    nbtValue(w, title, strlen(title));
                    nbtKey(w, NBT_STRING, "author");
                    nbtValue(w, author, strlen(author));
                    nbtKey(w, NBT_INT, "generation");
                    w.i32(generation);
                    nbtKey(w, NBT_BYTE, "resolved");
                    w.u8(1);
                }
                any = true;
                break;
            }
            default: break;
        }
    }
    for (int32_t i = 0; i < removed && r.ok(); i++) r.varint();
    w.u8(NBT_END);
    if (!r.ok()) return false;
    if (id <= 0 || id >= NUM_ITEMS) return true;
    s.id = (uint16_t)id;
    s.count = (uint8_t)std::min(99, (int)count);
    if (any && !tag.failed()) s.setTag(tag.data(), tag.size());
    s.damage = (uint16_t)std::max(0, std::min(65535, damage));
    return true;
}

// The slots of a window click (1.21.5+): the client's prediction as hashes. The server
// recomputes the click itself and resends the window, so they are skipped.
bool skipHashedSlot(Reader& r) {
    if (!r.boolean()) return r.ok();
    r.varint();   // item
    r.varint();   // count
    int32_t n = r.varint();
    for (int32_t i = 0; i < n && r.ok(); i++) { r.varint(); r.i32(); }
    n = r.varint();
    for (int32_t i = 0; i < n && r.ok(); i++) r.varint();
    return r.ok();
}
} // namespace mc
