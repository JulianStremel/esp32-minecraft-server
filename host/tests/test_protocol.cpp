// The 1.21.8 wire formats: NBT text components, item stacks with data components, the
// configuration payloads, and a chunk packet parsed the way a client reads it.
#include <string>
#include <vector>
#include "testing.h"
#include "mc/bytebuf.h"
#include "mc/item.h"
#include "mc/net/deflate.h"
#include "mc/nbt.h"
#include "mc/registry.h"
#include "mc/server/chunk_codec.h"
#include "mc/text.h"
#include "mc/world/generator.h"
#include "mc/world/light.h"

using namespace mc;

namespace {

std::string nbtJson(const ByteBuf& b) {
    Reader r(b.data(), b.size());
    char out[1024];
    if (!readTextNbtAsJson(r, out, sizeof(out)) || r.remaining()) return "<invalid>";
    return out;
}

std::string text(const char* json) {
    ByteBuf b;
    Writer w(b);
    writeTextNbt(w, json);
    return nbtJson(b);
}

std::string readString(Reader& r) {
    int32_t n = r.varint();
    const uint8_t* p = r.take((size_t)(n < 0 ? 0 : n));
    return r.ok() ? std::string((const char*)p, (size_t)n) : std::string();
}

}  // namespace

TEST(text_json_becomes_nbt) {
    CHECK_STR(text("{\"text\":\"hi\",\"color\":\"gold\",\"bold\":true}").c_str(),
              "{\"text\":\"hi\",\"color\":\"gold\",\"bold\":true}");
    CHECK_STR(text("\"plain\"").c_str(), "\"plain\"");
    // clickEvent becomes click_event, its value the command (1.21.5)
    CHECK_STR(text("{\"text\":\"a\",\"clickEvent\":{\"action\":\"suggest_command\",\"value\":\"/msg a \"}}").c_str(),
              "{\"text\":\"a\",\"click_event\":{\"action\":\"suggest_command\",\"command\":\"/msg a \"}}");
    // translate with mixed arguments: strings become text compounds (an NBT list has one type)
    CHECK_STR(text("{\"translate\":\"chat.type.text\",\"with\":[{\"text\":\"Steve\"},\"hello\"]}").c_str(),
              "{\"translate\":\"chat.type.text\",\"with\":[{\"text\":\"Steve\"},{\"text\":\"hello\"}]}");
    // an array: the first with the rest as extra
    CHECK_STR(text("[\"a\",{\"text\":\"b\"}]").c_str(), "{\"text\":\"\",\"extra\":[{\"text\":\"a\"},{\"text\":\"b\"}]}");
    // escapes survive; broken JSON is sent as plain text
    CHECK_STR(text("{\"text\":\"q\\\"u\\\\o\\nte\"}").c_str(), "{\"text\":\"q\\\"u\\\\o\\nte\"}");
    CHECK_STR(text("{\"text\":").c_str(), "\"{\\\"text\\\":\"");
}

TEST(slot_writes_components_for_damage_name_and_book) {
    ByteBuf tag;
    Writer tw(tag);
    NbtWriter n(tw);
    n.beginRoot();
    n.beginCompound("display");
    n.str("Name", "{\"text\":\"Manual\"}");
    n.end();
    n.listHeader("pages", NBT_STRING, 2);
    tw.u16(14); tw.bytes((const uint8_t*)"{\"text\":\"p1\"}", 14);
    tw.u16(14); tw.bytes((const uint8_t*)"{\"text\":\"p2\"}", 14);
    n.str("title", "T");
    n.str("author", "A");
    n.listHeader("Enchantments", NBT_COMPOUND, 1);
    n.str("id", "minecraft:unbreaking");
    n.i16("lvl", 3);
    n.end();
    n.end();
    ItemStack book = ItemStack::of(itm::WrittenBook, 2);
    CHECK(book.setTag(tag.data(), tag.size()));
    book.damage = 4;
    ByteBuf b;
    Writer w(b);
    writeSlot(w, book);
    Reader r(b.data(), b.size());
    CHECK_EQ(r.varint(), 2);
    CHECK_EQ(r.varint(), itm::WrittenBook);
    CHECK_EQ(r.varint(), 4);   // damage, name, enchantments, book
    CHECK_EQ(r.varint(), 0);
    CHECK_EQ(r.varint(), comp::Damage);
    CHECK_EQ(r.varint(), 4);
    CHECK_EQ(r.varint(), comp::CustomName);
    char json[256];
    CHECK(readTextNbtAsJson(r, json, sizeof(json)));
    CHECK_STR(json, "{\"text\":\"Manual\"}");
    CHECK_EQ(r.varint(), comp::Enchantments);
    CHECK_EQ(r.varint(), 1);
    int unbreaking = -1;
    for (int i = 0; i < NUM_ENCHANTMENTS; i++)
        if (!strcmp(ENCHANTMENT_NAME[i], "minecraft:unbreaking")) unbreaking = i;
    CHECK_EQ(r.varint(), unbreaking);
    CHECK_EQ(r.varint(), 3);
    CHECK_EQ(r.varint(), comp::WrittenBookContent);
    CHECK_STR(readString(r).c_str(), "T");
    CHECK(!r.boolean());
    CHECK_STR(readString(r).c_str(), "A");
    CHECK_EQ(r.varint(), 0);
    CHECK_EQ(r.varint(), 2);
    CHECK(readTextNbtAsJson(r, json, sizeof(json)));
    CHECK_STR(json, "{\"text\":\"p1\"}");
    CHECK_EQ(r.u8(), NBT_END);
    CHECK(readTextNbtAsJson(r, json, sizeof(json)));
    CHECK_STR(json, "{\"text\":\"p2\"}");
    CHECK_EQ(r.u8(), NBT_END);
    CHECK(r.boolean());
    CHECK(r.ok());
    CHECK_EQ(r.remaining(), 0u);
    // empty and plain stacks
    ByteBuf e;
    Writer ew(e);
    writeSlot(ew, ItemStack());
    writeSlot(ew, ItemStack::of(itm::Stone, 64));
    ByteBuf want;
    Writer ww(want);
    ww.varint(0);   // empty
    ww.varint(64);
    ww.varint(itm::Stone);
    ww.varint(0);   // no components added
    ww.varint(0);   // or removed
    CHECK_EQ(e.size(), want.size());
    CHECK(e.size() == want.size() && !memcmp(e.data(), want.data(), want.size()));
}

TEST(slot_from_the_client_keeps_damage_enchantments_and_pages) {
    // an untrusted stack: each component's data is prefixed with its length
    ByteBuf b;
    Writer w(b);
    w.varint(1);
    w.varint(itm::WritableBook);
    w.varint(3);
    w.varint(1);
    auto component = [&](int type, void (*body)(Writer&)) {
        ByteBuf c;
        Writer cw(c);
        body(cw);
        w.varint(type);
        w.varint((int32_t)c.size());
        w.bytes(c.data(), c.size());
    };
    component(comp::Damage, [](Writer& c) { c.varint(11); });
    component(comp::Enchantments, [](Writer& c) { c.varint(1); c.varint(0); c.varint(2); });
    component(comp::WritableBookContent, [](Writer& c) {
        c.varint(2);
        c.string("one"); c.boolean(false);
        c.string("two"); c.boolean(false);
    });
    w.varint(comp::Lore);   // removed: ignored
    w.varint(itm::Stone);   // the next stack in the packet stays readable
    Reader r(b.data(), b.size());
    ItemStack s;
    CHECK(readSlot(r, s));
    CHECK_EQ(r.varint(), itm::Stone);
    CHECK_EQ(s.id, itm::WritableBook);
    CHECK_EQ(s.count, 1);
    CHECK_EQ(s.damage, 11);
    // back on the wire it has the same components
    ByteBuf out;
    Writer ow(out);
    writeSlot(ow, s);
    Reader o(out.data(), out.size());
    o.varint();
    o.varint();
    CHECK_EQ(o.varint(), 3);   // damage, enchantments, writable book
    o.varint();
    CHECK_EQ(o.varint(), comp::Damage);
    CHECK_EQ(o.varint(), 11);
    CHECK_EQ(o.varint(), comp::Enchantments);
    CHECK_EQ(o.varint(), 1);
    CHECK_EQ(o.varint(), 0);
    CHECK_EQ(o.varint(), 2);
    CHECK_EQ(o.varint(), comp::WritableBookContent);
    CHECK_EQ(o.varint(), 2);
    CHECK_STR(readString(o).c_str(), "one");
    CHECK(!o.boolean());
    CHECK_STR(readString(o).c_str(), "two");
    CHECK(!o.boolean());
    CHECK_EQ(o.remaining(), 0u);
}

TEST(configuration_payloads_parse) {
    int biomes = -1, dimensions = -1;
    for (int i = 0; i < 2 * NUM_SYNCED_REGISTRIES; i++) {
        bool full = i >= NUM_SYNCED_REGISTRIES;   // the payloads for clients without minecraft:core
        int k0 = i % NUM_SYNCED_REGISTRIES;
        Reader r(full ? REGISTRY_PAYLOAD_FULL[k0] : REGISTRY_PAYLOAD[k0], full ? REGISTRY_PAYLOAD_FULL_LEN[k0] : REGISTRY_PAYLOAD_LEN[k0]);
        std::string id = readString(r);
        int32_t n = r.varint();
        CHECK(n > 0);
        for (int32_t k = 0; k < n && r.ok(); k++) {
            CHECK(readString(r).rfind("minecraft:", 0) == 0);
            bool data = r.boolean();
            if (full) CHECK(data);
            if (data) CHECK(nbtSkipNetwork(r));
        }
        CHECK(r.ok());
        CHECK_EQ(r.remaining(), 0u);
        if (id == "minecraft:worldgen/biome") biomes = n;
        if (id == "minecraft:dimension_type") dimensions = n;
    }
    CHECK_EQ(biomes, NUM_BIOMES);
    CHECK_EQ(dimensions, 4);
    Reader t(TAGS_PAYLOAD, TAGS_PAYLOAD_LEN);
    int32_t registries = t.varint();
    CHECK(registries >= 4);
    for (int32_t i = 0; i < registries && t.ok(); i++) {
        readString(t);
        int32_t tags = t.varint();
        for (int32_t k = 0; k < tags && t.ok(); k++) {
            readString(t);
            int32_t n = t.varint();
            for (int32_t e = 0; e < n; e++) t.varint();
        }
    }
    CHECK(t.ok());
    CHECK_EQ(t.remaining(), 0u);
}

TEST(chunk_packet_parses_like_a_client) {
    Generator g;
    g.init(9, WORLD_NORMAL);
    Chunk c(2, -5);
    g.generate(c);
    c.setBiomeCell(0, 0, biome::Desert);   // two biomes: a palette
    ChunkLight light;
    CHECK(light.compute(c, (World*)nullptr));
    ByteBuf b;
    Writer w(b);
    writeChunkPacket(w, c, light);
    Reader r(b.data(), b.size());
    CHECK_EQ(r.varint(), pkt::s2c::MapChunk);
    CHECK_EQ(r.i32(), 2);
    CHECK_EQ(r.i32(), -5);
    int32_t maps = r.varint();
    for (int32_t i = 0; i < maps; i++) {
        r.varint();
        int32_t longs = r.varint();
        CHECK_EQ(longs, 37);
        for (int32_t k = 0; k < longs; k++) r.u64();
    }
    int32_t size = r.varint();
    const uint8_t* data = r.take((size_t)size);
    CHECK(r.ok());
    // the sections: blocks, then biomes, each a paletted container without a data length
    Reader d(data, (size_t)size);
    auto container = [&](int entries, int minBits, int maxBits, int direct) {
        int bits = d.u8();
        if (bits == 0) { d.varint(); return 0; }
        CHECK(bits == direct || (bits >= minBits && bits <= maxBits));
        if (bits != direct) {
            int32_t n = d.varint();
            for (int32_t i = 0; i < n; i++) d.varint();
        }
        int per = 64 / bits;
        for (int i = 0; i < (entries + per - 1) / per; i++) d.u64();
        return bits;
    };
    int biomeBits = 0;
    for (int s = 0; s < NUM_SECTIONS; s++) {
        int16_t count = d.i16();
        CHECK(count >= 0 && count <= 4096);
        container(4096, 4, 8, 15);
        biomeBits = container(64, 1, 3, 7);
    }
    CHECK_EQ(biomeBits, 1);
    CHECK(d.ok());
    CHECK_EQ(d.remaining(), 0u);
    int32_t tiles = r.varint();
    CHECK_EQ(tiles, 0);
    // light: four bit sets, then the arrays
    int sky = 0, block = 0;
    for (int m = 0; m < 4; m++) {
        int32_t longs = r.varint();
        uint64_t bits = longs ? r.u64() : 0;
        if (m == 0) sky = __builtin_popcountll(bits);
        if (m == 1) block = __builtin_popcountll(bits);
    }
    CHECK_EQ(r.varint(), sky);
    for (int i = 0; i < sky; i++) { CHECK_EQ(r.varint(), 2048); r.take(2048); }
    CHECK_EQ(r.varint(), block);
    for (int i = 0; i < block; i++) { CHECK_EQ(r.varint(), 2048); r.take(2048); }
    CHECK(sky > 0);
    CHECK(r.ok());
    CHECK_EQ(r.remaining(), 0u);
}

TEST(prebuilt_configuration_packets_inflate_to_their_payloads) {
    // the deflated copies (sent on compressed connections) hold the id and payload
    std::vector<uint8_t> out(1 << 17);
    auto check = [&](int id, const uint8_t* payload, size_t len, const uint8_t* z, size_t zLen) {
        size_t n = 0;
        CHECK(inflateZlib(z, zLen, out.data(), out.size(), n));
        ByteBuf want;
        Writer w(want);
        w.varint(id);
        w.bytes(payload, len);
        CHECK_EQ(n, want.size());
        CHECK(n == want.size() && !memcmp(out.data(), want.data(), n));
    };
    for (int i = 0; i < NUM_SYNCED_REGISTRIES; i++) {
        check(pkt::cfg_s2c::RegistryData, REGISTRY_PAYLOAD[i], REGISTRY_PAYLOAD_LEN[i], REGISTRY_PAYLOAD_Z[i], REGISTRY_PAYLOAD_Z_LEN[i]);
        check(pkt::cfg_s2c::RegistryData, REGISTRY_PAYLOAD_FULL[i], REGISTRY_PAYLOAD_FULL_LEN[i], REGISTRY_PAYLOAD_FULL_Z[i],
              REGISTRY_PAYLOAD_FULL_Z_LEN[i]);
    }
    check(pkt::cfg_s2c::Tags, TAGS_PAYLOAD, TAGS_PAYLOAD_LEN, TAGS_PAYLOAD_Z, TAGS_PAYLOAD_Z_LEN);
}
