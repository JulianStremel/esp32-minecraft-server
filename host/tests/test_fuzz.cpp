// Feeds random and mutated packets into a logged-in player session; under the
// sanitizer build this catches out-of-bounds reads/writes in packet handlers.
#include <deque>
#include <unistd.h>
#include "testing.h"
#include "mc/registry.h"
#include "mc/server/server.h"
#include "mc/world/noise.h"

using namespace mc;

namespace {
struct Q { std::deque<uint8_t> toServer, toClient; bool closed = false; };
struct FuzzConn : Conn {
    Q* q;
    explicit FuzzConn(Q* qq) : q(qq) {}
    int read(uint8_t* buf, size_t n) override {
        if (q->closed) return -1;
        size_t c = std::min(n, q->toServer.size());
        for (size_t i = 0; i < c; i++) { buf[i] = q->toServer.front(); q->toServer.pop_front(); }
        return (int)c;
    }
    int write(const uint8_t* buf, size_t n) override {
        if (q->closed) return -1;
        q->toClient.insert(q->toClient.end(), buf, buf + n);
        if (q->toClient.size() > (1u << 20)) q->toClient.clear();  // discard output
        return (int)n;
    }
    bool connected() override { return !q->closed; }
    void close() override { q->closed = true; }
};

void sendFrame(Q& q, const std::vector<uint8_t>& payload) {
    uint8_t hdr[5];
    BufSink s(hdr, 5);
    Writer w(s);
    w.varint((int32_t)payload.size());
    q.toServer.insert(q.toServer.end(), hdr, hdr + s.size());
    q.toServer.insert(q.toServer.end(), payload.begin(), payload.end());
}

}  // namespace

TEST(fuzz_play_packets) {
    Server s;
    ServerConfig cfg;
    cfg.port = 0;
    cfg.worldType = WORLD_FLAT;
    cfg.compressionThreshold = -1;
    cfg.defaultGameMode = GM_CREATIVE;
    cfg.ops = "Fuzzer";
    CHECK(s.begin(cfg, nullptr));
    Rng rng(20240611);
    int sessions = 0, packets = 0;
    for (int round = 0; round < 60; round++) {
        Q q;
        Player& p = s.players[0];
        p.reset(&s, 0);
        p.conn.attach(new FuzzConn(&q));
        p.state = CS_HANDSHAKE;
        p.connectedAt = plat::millis();
        // handshake + login start
        {
            std::vector<uint8_t> hs;
            BufSink bs(nullptr, 0);
            uint8_t buf[64];
            BufSink b(buf, sizeof(buf));
            Writer w(b);
            w.varint(0); w.varint(754); w.string("localhost"); w.u16(25565); w.varint(2);
            sendFrame(q, std::vector<uint8_t>(buf, buf + b.size()));
            BufSink b2(buf, sizeof(buf));
            Writer w2(b2);
            w2.varint(0); w2.string("Fuzzer");
            sendFrame(q, std::vector<uint8_t>(buf, buf + b2.size()));
        }
        s.loop();
        CHECK_EQ(p.state, CS_PLAY);
        sessions++;
        // a valid teleport confirm so movement is accepted
        for (int i = 0; i < 2000 && p.state == CS_PLAY; i++) {
            std::vector<uint8_t> payload;
            int id = rng.range(0x30);
            payload.push_back((uint8_t)id);
            int len = rng.range(4) == 0 ? rng.range(600) : rng.range(40);
            int mode = rng.range(3);
            for (int k = 0; k < len; k++) {
                uint8_t b;
                if (mode == 0) b = (uint8_t)rng.u32();
                else if (mode == 1) b = (uint8_t)(rng.range(4) == 0 ? 0xFF : rng.range(3));  // small ints / varint edge
                else b = (uint8_t)(k < 8 ? 0x7F : rng.u32());
                payload.push_back(b);
            }
            sendFrame(q, payload);
            packets++;
            if (i % 16 == 0) s.loop();
        }
        s.loop();
        if (p.state != CS_FREE) p.kick("fuzz round over");
        s.loop();
    }
    printf("    %d sessions, %d fuzzed packets, server still running: %s\n", sessions, packets, s.running() ? "yes" : "no");
    CHECK(s.running());
}

TEST(fuzz_pre_login_states) {
    Server s;
    ServerConfig cfg;
    cfg.port = 0;
    cfg.worldType = WORLD_FLAT;
    CHECK(s.begin(cfg, nullptr));
    Rng rng(777);
    for (int round = 0; round < 300; round++) {
        Q q;
        Player& p = s.players[1];
        p.reset(&s, 1);
        p.conn.attach(new FuzzConn(&q));
        p.state = CS_HANDSHAKE;
        p.connectedAt = plat::millis();
        // sometimes a valid handshake first, so the status/login handlers get garbage too
        if (rng.range(2)) {
            uint8_t buf[64];
            BufSink b(buf, sizeof(buf));
            Writer w(b);
            w.varint(0); w.varint(754); w.string("x"); w.u16(1); w.varint(1 + rng.range(2));
            sendFrame(q, std::vector<uint8_t>(buf, buf + b.size()));
        }
        for (int i = 0; i < 4; i++) {
            std::vector<uint8_t> payload;
            int len = rng.range(80);
            for (int k = 0; k < len; k++) payload.push_back((uint8_t)rng.u32());
            if (!payload.empty() && rng.range(2)) payload[0] = (uint8_t)rng.range(3);
            sendFrame(q, payload);
        }
        // raw garbage without framing too
        for (int k = 0; k < 8; k++) q.toServer.push_back((uint8_t)rng.u32());
        s.loop();
        if (p.state != CS_FREE) { p.conn.close(); p.state = CS_FREE; }
    }
    CHECK(s.running());
}
