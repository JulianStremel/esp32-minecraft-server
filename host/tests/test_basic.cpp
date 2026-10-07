#include "testing.h"
#include "mc/io.h"
#include "mc/md5.h"
#include "mc/nbt.h"

using namespace mc;

TEST(varint_roundtrip) {
    int32_t vals[] = {0, 1, 127, 128, 255, 25565, 2097151, 2147483647, -1, -2147483648};
    for (int32_t v : vals) {
        uint8_t buf[8];
        BufSink s(buf, sizeof(buf));
        Writer w(s);
        w.varint(v);
        CHECK_EQ((int)s.size(), varintSize((uint32_t)v));
        Reader r(buf, s.size());
        CHECK_EQ(r.varint(), v);
        CHECK(r.ok());
        CHECK_EQ(r.remaining(), 0);
    }
}

TEST(varlong_roundtrip) {
    int64_t vals[] = {0, 1, 300, 4294967296LL, -1, INT64_MIN, INT64_MAX};
    for (int64_t v : vals) {
        uint8_t buf[12];
        BufSink s(buf, sizeof(buf));
        Writer w(s);
        w.varlong(v);
        Reader r(buf, s.size());
        CHECK(r.varlong() == v);
        CHECK(r.ok());
    }
}

TEST(reader_underflow_sets_error) {
    uint8_t b[2] = {1, 2};
    Reader r(b, 2);
    r.u32();
    CHECK(!r.ok());
    uint8_t bad[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    Reader r2(bad, 6);
    r2.varint();
    CHECK(!r2.ok());
}

TEST(primitive_roundtrip) {
    uint8_t buf[64];
    BufSink s(buf, sizeof(buf));
    Writer w(s);
    w.i16(-2); w.i32(-123456); w.i64(-9876543210LL); w.f32(1.5f); w.f64(-2.25); w.string("héllo");
    Reader r(buf, s.size());
    CHECK_EQ(r.i16(), -2);
    CHECK_EQ(r.i32(), -123456);
    CHECK(r.i64() == -9876543210LL);
    CHECK(r.f32() == 1.5f);
    CHECK(r.f64() == -2.25);
    char str[16];
    r.string(str, sizeof(str));
    CHECK_STR(str, "héllo");
    CHECK(r.ok());
}

TEST(buf_sink_overflow) {
    uint8_t b[4];
    BufSink s(b, 4);
    Writer w(s);
    w.i32(1);
    CHECK(!s.overflowed());
    w.u8(1);
    CHECK(s.overflowed());
}

TEST(md5_known_vectors) {
    uint8_t out[16];
    md5((const uint8_t*)"", 0, out);
    char hex[33];
    auto tohex = [&]() { for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", out[i]); };
    tohex();
    CHECK_STR(hex, "d41d8cd98f00b204e9800998ecf8427e");
    md5((const uint8_t*)"The quick brown fox jumps over the lazy dog", 43, out);
    tohex();
    CHECK_STR(hex, "9e107d9d372bb6826bd81d3542a419d6");
    // 56 bytes forces a second padding block
    const char* s56 = "12345678901234567890123456789012345678901234567890123456";
    md5((const uint8_t*)s56, 56, out);
    tohex();
    CHECK_STR(hex, "49f193adce178490e34d1b3a4ec0064c");
}

TEST(offline_uuid_matches_vanilla) {
    // Notch's offline UUID, as printed by a vanilla server in offline mode.
    uint8_t u[16];
    offlineUuid("Notch", u);
    char hex[33];
    for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", u[i]);
    CHECK_STR(hex, "b50ad385829d3141a2167e7d7539ba7f");
}

TEST(nbt_skip_and_visit) {
    uint8_t buf[256];
    BufSink s(buf, sizeof(buf));
    Writer w(s);
    NbtWriter n(w);
    n.beginRoot();
    n.i32("Damage", 42);
    n.beginCompound("display");
    n.str("Name", "abc");
    n.end();
    n.listHeader("List", NBT_INT, 2);
    w.i32(1); w.i32(2);
    n.longArrayHeader("Longs", 1);
    w.i64(7);
    n.end();
    Reader r(buf, s.size());
    struct Ctx { int damage = -1; } ctx;
    bool ok = nbtVisitRoot(r, [](void* c, uint8_t t, const char* name, Reader& rr) {
        if (t == NBT_INT && !strcmp(name, "Damage")) { ((Ctx*)c)->damage = rr.i32(); return true; }
        return false;
    }, &ctx);
    CHECK(ok);
    CHECK_EQ(ctx.damage, 42);
    CHECK_EQ(r.remaining(), 0);
    Reader r2(buf, s.size());
    CHECK(nbtSkipRoot(r2));
    CHECK_EQ(r2.remaining(), 0);
    uint8_t endOnly = 0;
    Reader r3(&endOnly, 1);
    CHECK(nbtSkipRoot(r3));
}
