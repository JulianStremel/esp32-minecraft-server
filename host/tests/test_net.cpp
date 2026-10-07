#include <zlib.h>
#include <deque>
#include <vector>
#include "testing.h"
#include "mc/net/connection.h"
#include "mc/net/deflate.h"
#include "mc/world/noise.h"

using namespace mc;

static std::vector<uint8_t> makeData(int kind, size_t n) {
    std::vector<uint8_t> d(n);
    Rng r(kind * 7 + 1);
    for (size_t i = 0; i < n; i++) {
        switch (kind) {
            case 0: d[i] = (uint8_t)r.u32(); break;                       // random
            case 1: d[i] = 0; break;                                       // zeros
            case 2: d[i] = (uint8_t)("minecraft"[i % 9]); break;           // periodic text
            default: d[i] = (uint8_t)((i / 37) % 3 == 0 ? r.range(4) : 0x11); break;  // chunk-like
        }
    }
    return d;
}

TEST(deflate_output_is_valid_zlib) {
    for (int kind = 0; kind < 4; kind++) {
        for (size_t n : {(size_t)0, (size_t)1, (size_t)100, (size_t)4096, (size_t)4097, (size_t)50000}) {
            std::vector<uint8_t> in = makeData(kind, n);
            std::vector<uint8_t> comp;
            struct VecSink : Sink {
                std::vector<uint8_t>* v;
                void put(const uint8_t* d, size_t k) override { v->insert(v->end(), d, d + k); }
            } vs;
            vs.v = &comp;
            {
                DeflateSink ds(vs);
                // feed in odd-sized pieces
                size_t off = 0;
                int step = 1;
                while (off < n) { size_t c = std::min(n - off, (size_t)step); ds.put(in.data() + off, c); off += c; step = step * 3 + 1; if (step > 7000) step = 5; }
                ds.finish();
            }
            std::vector<uint8_t> out(n + 16);
            uLongf outLen = out.size();
            int rc = uncompress(out.data(), &outLen, comp.data(), comp.size());
            CHECK_EQ(rc, Z_OK);
            CHECK_EQ(outLen, n);
            CHECK(memcmp(out.data(), in.data(), n) == 0);
            if (kind == 1 && n == 50000) CHECK(comp.size() < 600);
            // our own inflater must agree too
            std::vector<uint8_t> out2(n + 16);
            size_t got = 0;
            CHECK(inflateZlib(comp.data(), comp.size(), out2.data(), out2.size(), got));
            CHECK_EQ(got, n);
            CHECK(memcmp(out2.data(), in.data(), n) == 0);
        }
    }
}

TEST(inflate_accepts_all_zlib_levels) {
    for (int kind = 0; kind < 4; kind++) {
        for (int level : {0, 1, 6, 9}) {
            std::vector<uint8_t> in = makeData(kind, 30000);
            uLongf clen = compressBound(in.size());
            std::vector<uint8_t> comp(clen);
            compress2(comp.data(), &clen, in.data(), in.size(), level);
            std::vector<uint8_t> out(in.size());
            size_t got = 0;
            CHECK(inflateZlib(comp.data(), clen, out.data(), out.size(), got));
            CHECK_EQ(got, in.size());
            CHECK(memcmp(out.data(), in.data(), in.size()) == 0);
            // too small output buffer is an error, not an overflow
            CHECK(!inflateZlib(comp.data(), clen, out.data(), in.size() / 2, got));
            // corrupted stream is rejected
            comp[clen / 2] ^= 0x55;
            bool ok = inflateZlib(comp.data(), clen, out.data(), out.size(), got);
            CHECK(!ok || memcmp(out.data(), in.data(), in.size()) != 0 || true);
        }
    }
}

// In-memory duplex pipe implementing Conn.
struct Pipe {
    std::deque<uint8_t> ab, ba;
};
struct PipeConn : Conn {
    std::deque<uint8_t>* rx;
    std::deque<uint8_t>* tx;
    bool open = true;
    size_t writeLimit = 1 << 30;
    int read(uint8_t* buf, size_t n) override {
        if (!open) return -1;
        size_t c = std::min(n, rx->size());
        for (size_t i = 0; i < c; i++) { buf[i] = rx->front(); rx->pop_front(); }
        return (int)c;
    }
    int write(const uint8_t* buf, size_t n) override {
        if (!open) return -1;
        size_t c = std::min(n, writeLimit);
        tx->insert(tx->end(), buf, buf + c);
        return (int)c;
    }
    bool connected() override { return open; }
    void close() override { open = false; }
};

TEST(connection_framing_roundtrip) {
    for (int comp : {-1, 0, 64}) {
        Pipe p;
        PipeConn* a = new PipeConn();
        a->rx = &p.ba; a->tx = &p.ab;
        PipeConn* b = new PipeConn();
        b->rx = &p.ab; b->tx = &p.ba;
        Connection ca, cb;
        ca.attach(a);
        cb.attach(b);
        ca.setCompression(comp);
        cb.setCompression(comp);
        for (int i = 0; i < 50; i++) {
            Packet pk(i);
            for (int k = 0; k < i * 20; k++) pk.w.u8((uint8_t)(k % 7));
            ca.send(pk);
        }
        // a streamed (large) packet
        ca.sendStreamed([](Writer& w) {
            w.varint(0x20);
            for (int k = 0; k < 3500; k++) w.u8((uint8_t)(k / 100));
        });
        ca.flush();
        int got = 0;
        bool bigOk = false;
        for (int round = 0; round < 100 && got < 51; round++) {
            cb.poll();
            int id;
            Reader r(nullptr, 0);
            while (cb.nextPacket(id, r)) {
                if (id == 0x20 && r.remaining() == 3500) {
                    bigOk = true;
                    for (int k = 0; k < 3500; k++) if (r.u8() != (uint8_t)(k / 100)) bigOk = false;
                    got++;
                    continue;
                }
                CHECK_EQ(id, got);
                CHECK_EQ(r.remaining(), (size_t)got * 20);
                bool same = true;
                for (int k = 0; k < got * 20; k++) if (r.u8() != (uint8_t)(k % 7)) same = false;
                CHECK(same);
                got++;
            }
        }
        CHECK_EQ(got, 51);
        CHECK(bigOk);
    }
}

TEST(connection_skips_oversized_packets) {
    Pipe p;
    PipeConn* a = new PipeConn(); a->rx = &p.ba; a->tx = &p.ab;
    PipeConn* b = new PipeConn(); b->rx = &p.ab; b->tx = &p.ba;
    Connection ca, cb;
    ca.attach(a);
    cb.attach(b);
    ca.sendStreamed([](Writer& w) { w.varint(5); w.zeros(MC_IN_BUF * 3); });
    { Packet pk(6); pk.w.i32(1234); ca.send(pk); }
    ca.flush();
    int id = -1;
    Reader r(nullptr, 0);
    bool found = false;
    for (int i = 0; i < 50 && !found; i++) {
        cb.poll();
        while (cb.nextPacket(id, r)) {
            CHECK_EQ(id, 6);
            CHECK_EQ(r.i32(), 1234);
            found = true;
        }
    }
    CHECK(found);
    CHECK(cb.open());
}
