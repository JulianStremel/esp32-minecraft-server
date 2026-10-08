// The write-back cache for SD cards: same contents as without it, fewer and larger
// writes, oldest-first order, and the world store working on top of it.
#include <string.h>
#include <vector>
#include "testing.h"
#include "mc/registry.h"
#include "mc/storage/block_device.h"
#include "mc/storage/world_store.h"
#include "mc/storage/write_cache.h"
#include "mc/world/noise.h"

using namespace mc;

namespace {

// a RAM device that records the writes reaching it
class LogDevice : public MemDevice {
public:
    explicit LogDevice(size_t n) : MemDevice(n) {}
    struct W { uint64_t off; uint32_t len; };
    std::vector<W> log;
    bool write(uint64_t off, const void* buf, uint32_t len) override {
        log.push_back({off, len});
        return MemDevice::write(off, buf, len);
    }
};

}  // namespace

TEST(write_cache_keeps_contents_under_random_io) {
    const size_t SIZE = 1 << 20;   // 1 MiB, 64 lines of 16 KiB; the cache has 8
    MemDevice ref(SIZE);
    LogDevice dev(SIZE);
    WriteBackCache c(&dev, 16 * 1024, 8);
    CHECK(c.ok());
    Rng r(7);
    std::vector<uint8_t> a(70000), b(70000);
    for (int op = 0; op < 4000; op++) {
        uint32_t len = 1 + r.range(r.range(4) == 0 ? 66000 : 900);
        uint64_t off = r.range((int)(SIZE - len));
        if (r.range(3)) {
            for (uint32_t i = 0; i < len; i++) a[i] = (uint8_t)r.u32();
            CHECK(c.write(off, a.data(), len));
            ref.write(off, a.data(), len);
        } else {
            CHECK(c.read(off, a.data(), len));
            ref.read(off, b.data(), len);
            if (memcmp(a.data(), b.data(), len) != 0) {
                CHECK(false);
                break;
            }
        }
        if (r.range(500) == 0) CHECK(c.flush());
    }
    CHECK(c.flush());
    CHECK_EQ(c.dirtyLines(), 0);
    // after a flush the device itself holds everything
    std::vector<uint8_t> x(SIZE), y(SIZE);
    dev.read(0, x.data(), SIZE);
    ref.read(0, y.data(), SIZE);
    CHECK(memcmp(x.data(), y.data(), SIZE) == 0);
    printf("    4000 random operations: %u writes reached the device for %u written\n", (unsigned)dev.log.size(),
           (unsigned)c.stats().writes);
}

TEST(write_cache_gathers_small_writes_and_keeps_their_order) {
    LogDevice dev(1 << 20);
    WriteBackCache c(&dev, 16 * 1024, 8);
    uint8_t b[512];
    memset(b, 0xAB, sizeof(b));
    // 32 small writes into one line, then one into line 5, then line 0 again
    for (int i = 0; i < 32; i++) c.write((uint64_t)i * 512, b, 512);
    c.write(5 * 16384 + 100, b, 10);
    c.write(1000, b, 10);
    CHECK_EQ(dev.log.size(), 0u);   // nothing reached the device yet
    CHECK(c.flush());
    CHECK_EQ(dev.log.size(), 2u);
    CHECK_EQ(dev.log[0].off, 0u);                // line 0 was changed first
    CHECK_EQ(dev.log[1].off, 5u * 16384);
    CHECK_EQ(dev.log[0].len, 16384u);
    // a 64 KiB record over 4 lines: one write
    dev.log.clear();
    std::vector<uint8_t> rec(65536, 7);
    c.write(8 * 16384, rec.data(), 65536);
    CHECK(c.flush());
    CHECK_EQ(dev.log.size(), 1u);
    CHECK_EQ(dev.log[0].len, 65536u);
    // more dirty lines than the cache has (not neighbours, so not merged): the oldest
    // go out first
    dev.log.clear();
    for (int i = 0; i < 12; i++) c.write((uint64_t)(20 + 2 * i) * 16384 + 7, b, 1);
    CHECK(dev.log.size() >= 2);
    if (dev.log.size() >= 2) {
        CHECK_EQ(dev.log[0].off, 20u * 16384);
        CHECK_EQ(dev.log[1].off, 22u * 16384);
    }
    CHECK(c.flush());
}

TEST(write_cache_handles_the_devices_last_partial_line) {
    MemDevice dev(100000);   // not a multiple of 16 KiB
    WriteBackCache c(&dev, 16 * 1024, 4);
    uint8_t b[1000];
    memset(b, 0x5A, sizeof(b));
    CHECK(c.write(99000, b, 1000));
    CHECK(!c.write(99500, b, 1000));   // past the end
    CHECK(c.flush());
    uint8_t r[1000];
    CHECK(dev.read(99000, r, 1000));
    CHECK(memcmp(r, b, 1000) == 0);
}

// what the world store's own write pattern costs, with and without the cache
static void storeWork(BlockDevice* d) {
    StoreParams sp;
    sp.radius = 64;
    WorldStore ws(d);
    ws.open(sp, true);
    WorldMeta m;
    for (int round = 0; round < 3; round++) {   // three autosaves
        ws.saveMeta(m);
        for (int i = 0; i < 40; i++) {
            Chunk ch(i % 7 - 3 + round * 40, i / 7 - 3);
            ch.set(i % 16, 64, 3, bs::GoldBlock);
            ws.saveChunk(ch);
        }
        for (int k = 0; k < 4; k++) {
            PlayerData p;
            p.uuid[0] = (uint8_t)(k + 1);
            ws.savePlayer(p);
        }
        ws.flush();
    }
}

TEST(write_cache_turns_the_stores_small_writes_into_few_large_ones) {
    LogDevice plain(64u << 20), under(64u << 20);
    storeWork(&plain);
    {
        WriteBackCache c(&under, 16 * 1024, 32);
        storeWork(&c);
    }
    auto smallWrites = [](const LogDevice& d) {
        int n = 0;
        for (const auto& w : d.log) n += w.len < 4096;
        return n;
    };
    auto bytes = [](const LogDevice& d) {
        uint64_t n = 0;
        for (const auto& w : d.log) n += w.len;
        return (unsigned)(n / 1024);
    };
    printf("    bytes written: %u KiB without the cache, %u KiB with it\n", bytes(plain), bytes(under));
    printf("    120 chunks, 12 player and 3 meta saves: %u device writes (%d under 4 KiB) without the cache, "
           "%u (%d) with it\n",
           (unsigned)plain.log.size(), smallWrites(plain), (unsigned)under.log.size(), smallWrites(under));
    CHECK(under.log.size() < plain.log.size());
    CHECK_EQ(smallWrites(under), 0);   // the card sees no writes smaller than a line
}

TEST(world_store_works_through_the_write_cache) {
    MemDevice dev(64u << 20);
    StoreParams sp;
    sp.radius = 64;
    {
        WriteBackCache c(&dev, 16 * 1024, 32);
        WorldStore ws(&c);
        CHECK(ws.open(sp, true));
        WorldMeta m;
        m.seed = 4242;
        CHECK(ws.saveMeta(m));
        for (int i = 0; i < 40; i++) {
            Chunk ch(i % 7 - 3, i / 7 - 3);
            ch.set(i % 16, 64, 3, bs::GoldBlock);
            CHECK(ws.saveChunk(ch));
        }
        PlayerData p;
        p.uuid[0] = 9;
        p.x = 1.5;
        CHECK(ws.savePlayer(p));
        CHECK(ws.flush());
    }
    // reopened straight on the device: everything arrived
    WorldStore ws(&dev);
    CHECK(ws.open(sp, false));
    WorldMeta m;
    CHECK(ws.loadMeta(m));
    CHECK_EQ(m.seed, 4242u);
    for (int i = 0; i < 40; i++) {
        Chunk ch(i % 7 - 3, i / 7 - 3);
        CHECK_EQ(ws.loadChunk(ch), LOAD_OK);
        CHECK_EQ(ch.get(i % 16, 64, 3), bs::GoldBlock);
    }
    PlayerData q;
    uint8_t u[16] = {9};
    CHECK(ws.loadPlayer(u, q));
    CHECK(q.x == 1.5);
}
