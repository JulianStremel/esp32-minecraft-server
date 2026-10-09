#include "testing.h"
#include "mc/item.h"
#include "mc/bytebuf.h"
#include "mc/nbt.h"
#include "mc/registry.h"
#include <thread>
#include <utility>
using namespace mc;
namespace {
ItemStack namedBook(bool reverse, const char* page = "hello") {
    ByteBuf b;
    Writer w(b);
    NbtWriter n(w);
    n.beginRoot();
    if (!reverse) n.str("author", "redstone");
    n.beginCompound("display");
    if (!reverse) n.i32("color", 123);
    n.str("Name", "{\"text\":\"Circuit manual\"}");
    if (reverse) n.i32("color", 123);
    n.end();
    n.listHeader("pages", NBT_STRING, 1);
    w.u16(strlen(page));
    w.bytes((const uint8_t*)page, strlen(page));
    if (reverse) n.str("author", "redstone");
    n.i32("Damage", 7);
    n.end();
    ItemStack item = ItemStack::of(itm::WrittenBook);
    CHECK(item.setTag(b.data(), b.size()));
    return item;
}
} // namespace
TEST(item_tags_compare_compounds_independent_of_key_order) {
    ItemStack a = namedBook(false), b = namedBook(true), other = namedBook(false, "different");
    CHECK(a.sameItem(b));
    CHECK(!a.sameItem(other));
    CHECK(a.tagData() != b.tagData());
    CHECK_EQ(a.damage, 7);
    ItemStack copy = a;
    CHECK(copy.tagData() == a.tagData());
    a.clear();
    CHECK(copy.sameItem(b));
    ItemStack moved = std::move(copy);
    CHECK(copy.empty());
    CHECK_EQ(copy.tagSize(), 0u);
    CHECK(moved.sameItem(b));
    moved.damage = 9;
    CHECK(!moved.sameItem(b));
}
TEST(item_tags_snapshot_copies_are_safe_across_workers) {
    ItemStack original = namedBook(false);
    std::thread workers[4];
    for (auto& worker : workers)
        worker = std::thread([original] {
            for (int i = 0; i < 10000; ++i) {
                ItemStack copy = original;
                ItemStack moved = std::move(copy);
                copy = moved;
            }
        });
    for (auto& worker : workers)
        worker.join();
    CHECK(original.sameItem(namedBook(true)));
}
TEST(item_tags_reject_malformed_and_oversized_values_without_modification) {
    ItemStack item = namedBook(false), before = item;
    // Nonempty lists cannot use TAG_End (zero-byte elements).
    const uint8_t bad[] = {NBT_COMPOUND, 0, 0, NBT_LIST, 0, 1, 'x', NBT_END, 0x7f, 0xff, 0xff, 0xff, 0};
    CHECK(!item.setTag(bad, sizeof(bad)));
    CHECK(item.sameItem(before));
    const uint8_t badArray[] = {NBT_COMPOUND, 0, 0, NBT_LONG_ARRAY, 0, 1, 'x', 0x40, 0, 0, 0, 0};
    CHECK(!item.setTag(badArray, sizeof(badArray)));
    CHECK(item.sameItem(before));
    ByteBuf big;
    Writer w(big);
    NbtWriter n(w);
    n.beginRoot();
    w.u8(NBT_BYTE_ARRAY);
    w.u16(1);
    w.u8('x');
    w.i32(ItemStack::MAX_TAG_BYTES);
    w.zeros(ItemStack::MAX_TAG_BYTES);
    n.end();
    CHECK(!item.setTag(big.data(), big.size()));
    CHECK(item.sameItem(before));
    ByteBuf deep;
    Writer dw(deep);
    NbtWriter dn(dw);
    dn.beginRoot();
    for (int i = 0; i < 26; ++i)
        dn.beginCompound("x");
    for (int i = 0; i < 27; ++i)
        dn.end();
    CHECK(!item.setTag(deep.data(), deep.size()));
    CHECK(item.sameItem(before));
}
TEST(item_tags_duplicate_keys_keep_last_and_empty_root_collapses) {
    ByteBuf b;
    Writer w(b);
    NbtWriter n(w);
    n.beginRoot();
    n.str("x", "old");
    n.str("x", "new");
    n.end();
    ItemStack a = ItemStack::of(itm::Stone);
    CHECK(a.setTag(b.data(), b.size()));
    b.clear();
    n.beginRoot();
    n.str("x", "new");
    n.end();
    ItemStack other = ItemStack::of(itm::Stone);
    CHECK(other.setTag(b.data(), b.size()));
    CHECK(a.sameItem(other));
    b.clear();
    n.beginRoot();
    n.i32("Damage", 0);
    n.end();
    CHECK(a.setTag(b.data(), b.size()));
    CHECK_EQ(a.tagSize(), 0u);
    CHECK(a.sameItem(ItemStack::of(itm::Stone)));
}
