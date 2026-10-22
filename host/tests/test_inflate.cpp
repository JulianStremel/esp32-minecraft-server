// The table-driven inflate (net/deflate.cpp) against the previous bit-by-bit one: the
// same bytes out of zlib's streams (stored, fixed and dynamic blocks, long codes), of
// our own deflate, and the same verdict on corrupted input.
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "testing.h"
#include "mc/bytebuf.h"
#include "mc/io.h"
#include "mc/net/deflate.h"
#include "inflate_ref.h"
#include "inflate_vectors.h"

using namespace mc;

static void same(const uint8_t* in, size_t n, int& agree, int& okCount) {
    std::vector<uint8_t> a(1 << 20), b(1 << 20);
    size_t la = 0, lb = 0;
    bool ra = inflateZlib(in, n, a.data(), a.size(), la);
    bool rb = ref::inflateZlibRef(in, n, b.data(), b.size(), lb);
    CHECK_EQ(ra, rb);
    if (ra && rb) {
        CHECK_EQ(la, lb);
        CHECK(!memcmp(a.data(), b.data(), la));
        okCount++;
    }
    agree += ra == rb;
}

TEST(inflate_matches_the_reference) {
    int agree = 0, ok = 0;
    for (const InflateVector& v : INFLATE_VECTORS) same(v.data, v.len, agree, ok);
    // our own deflate's output
    for (int k = 0; k < 20; k++) {
        std::vector<uint8_t> raw(1000 + k * 3000);
        uint32_t s = 0x1234567u + k;
        for (size_t i = 0; i < raw.size(); i++) {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            raw[i] = (uint8_t)(k % 3 == 0 ? s : (s % 9 == 0 ? s >> 8 : i / 37));
        }
        ByteBuf out;
        {
            DeflateSink ds(out);
            ds.put(raw.data(), raw.size());
            ds.finish();
        }
        same(out.data(), out.size(), agree, ok);
        std::vector<uint8_t> back(raw.size() + 16);
        size_t n = 0;
        CHECK(inflateZlib(out.data(), out.size(), back.data(), back.size(), n));
        CHECK_EQ(n, raw.size());
        CHECK(!memcmp(back.data(), raw.data(), raw.size()));
    }
    // corrupted streams: both refuse them, or both give the same bytes
    uint32_t s = 99;
    for (int t = 0; t < 3000; t++) {
        const InflateVector& v = INFLATE_VECTORS[t % (sizeof(INFLATE_VECTORS) / sizeof(INFLATE_VECTORS[0]))];
        std::vector<uint8_t> c(v.data, v.data + v.len);
        if (c.size() < 8) continue;
        for (int f = 0; f < 1 + t % 4; f++) {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            c[2 + s % (c.size() - 6)] ^= (uint8_t)(1u << (s >> 8) % 8);
        }
        same(c.data(), c.size(), agree, ok);
    }
    printf("    %d streams, the same verdict on all, %d decoded the same\n", agree, ok);
}
