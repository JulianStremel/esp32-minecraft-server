// The cache of ready-to-send chunk packets (storage/packet_cache.h).
#include <string.h>
#include <vector>
#include "testing.h"
#include "mc/storage/block_device.h"
#include "mc/storage/packet_cache.h"

using namespace mc;

static void fill(std::vector<uint8_t>& v, uint64_t key, uint32_t len) {
    v.resize(len);
    uint64_t s = key * 0x9E3779B97F4A7C15ull + len;
    for (uint32_t i = 0; i < len; i++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        v[i] = (uint8_t)s;
    }
}

TEST(packet_cache_round_trip_reopen_and_epochs) {
    MemDevice dev(4 << 20);   // 4 MiB
    CHECK(dev.ok());
    PacketCache pc;
    CHECK(pc.open(&dev, 1234));
    std::vector<uint8_t> want, got(PacketCache::MAX_PACKET + 512);
    uint32_t len = 0;
    CHECK(!pc.get(PacketCache::key(0, 5, -7), got.data(), (uint32_t)got.size(), len));   // empty
    for (int i = 0; i < 50; i++) {
        uint64_t k = PacketCache::key((uint8_t)(i % 3), i * 7 - 100, -i * 13);
        fill(want, k, 1000 + i * 97);
        CHECK(pc.put(k, want.data(), (uint32_t)want.size()));
    }
    for (int i = 0; i < 50; i++) {
        uint64_t k = PacketCache::key((uint8_t)(i % 3), i * 7 - 100, -i * 13);
        fill(want, k, 1000 + i * 97);
        CHECK(pc.get(k, got.data(), (uint32_t)got.size(), len));
        CHECK_EQ(len, (uint32_t)want.size());
        CHECK(!memcmp(got.data(), want.data(), want.size()));
    }
    // the same world after a restart: the entries are still there
    CHECK(pc.flush());
    PacketCache again;
    CHECK(again.open(&dev, 1234));
    uint64_t k0 = PacketCache::key(0, -100, 0);
    fill(want, k0, 1000);
    CHECK(again.get(k0, got.data(), (uint32_t)got.size(), len));
    CHECK(!memcmp(got.data(), want.data(), want.size()));
    // another world: nothing of the old one is found
    PacketCache other;
    CHECK(other.open(&dev, 999));
    CHECK(!other.get(k0, got.data(), (uint32_t)got.size(), len));
}

TEST(packet_cache_ring_overwrites_oldest) {
    MemDevice dev(1 << 20);   // 1 MiB: about 2000 data sectors
    PacketCache pc;
    CHECK(pc.open(&dev, 7));
    std::vector<uint8_t> want, got(PacketCache::MAX_PACKET + 512);
    uint32_t len = 0;
    const int N = 600;   // 600 records of ~4 KiB: twice around the ring
    for (int i = 0; i < N; i++) {
        uint64_t k = PacketCache::key(0, i, i);
        fill(want, k, 3500 + (i % 7) * 100);
        CHECK(pc.put(k, want.data(), (uint32_t)want.size()));
    }
    int hitsOld = 0, hitsNew = 0;
    for (int i = 0; i < 50; i++) {
        hitsOld += pc.get(PacketCache::key(0, i, i), got.data(), (uint32_t)got.size(), len);
        uint64_t k = PacketCache::key(0, N - 1 - i, N - 1 - i);
        if (pc.get(k, got.data(), (uint32_t)got.size(), len)) {
            fill(want, k, len);
            hitsNew += !memcmp(got.data(), want.data(), len);
        }
    }
    printf("    oldest 50: %d found; newest 50: %d found intact; %u stale\n", hitsOld, hitsNew, (unsigned)pc.stats().stale);
    CHECK_EQ(hitsOld, 0);   // overwritten: detected, never returned wrong
    CHECK(hitsNew >= 45);
}
