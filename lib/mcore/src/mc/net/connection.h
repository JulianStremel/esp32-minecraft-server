// Framing of Minecraft packets over a non-blocking Conn: buffered input with
// packet extraction, buffered output, optional zlib compression.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "mc/bytebuf.h"
#include "mc/io.h"
#include "mc/limits.h"
#include "mc/net/deflate.h"
#include "mc/platform.h"

namespace mc {

// A packet being built: [id varint][fields...] in a scratch buffer from a small pool.
class Packet {
public:
    explicit Packet(int id);
    ~Packet();
    Packet(const Packet&) = delete;
    Packet& operator=(const Packet&) = delete;

    Writer w;
    const uint8_t* data() const { return buf_; }
    size_t size() const { return sink_.size(); }
    bool overflowed() const { return sink_.overflowed(); }

private:
    uint8_t* buf_;
    int slot_;
    BufSink sink_;
};

// Frames one packet produced by body(Writer&) exactly like Connection::sendStreamed
// would for a connection with the given compression threshold (-1: compression off),
// appending it to out. Safe on worker threads: deflateWs is the caller's workspace
// (DeflateSink::WORKSPACE bytes) and tmp a scratch buffer. body must be deterministic.
template <class F>
bool framePacket(F&& body, int threshold, uint8_t* deflateWs, ByteBuf& out, ByteBuf& tmp) {
    CountSink cs;
    {
        Writer cw(cs);
        body(cw);
    }
    size_t raw = cs.count;
    uint8_t hdr[10];
    BufSink hs(hdr, sizeof(hdr));
    Writer hw(hs);
    if (threshold >= 0 && (int)raw >= threshold) {
        tmp.clear();
        {
            DeflateSink ds(tmp, deflateWs);
            Writer dw(ds);
            body(dw);
            ds.finish();
        }
        if (tmp.failed()) return false;
        hw.varint((int32_t)(varintSize((uint32_t)raw) + tmp.size()));
        hw.varint((int32_t)raw);
        out.put(hdr, hs.size());
        out.put(tmp.data(), tmp.size());
    } else {
        if (threshold >= 0) {
            hw.varint((int32_t)raw + 1);
            hw.varint(0);
        } else {
            hw.varint((int32_t)raw);
        }
        out.put(hdr, hs.size());
        Writer w(out);
        body(w);
    }
    return !out.failed();
}

class Connection {
public:
    Connection();
    ~Connection();

    void attach(Conn* c);
    void close();
    bool open() const { return conn_ != nullptr && !closed_; }
    const char* peer() const { return conn_ ? conn_->peer() : "-"; }

    // ---- input
    // Reads from the socket. Returns false when the connection is closed.
    bool poll();
    // Extracts the next complete packet. `r` covers the packet body after the id.
    // The data stays valid until the next poll()/nextPacket() call.
    bool nextPacket(int& id, Reader& r);
    uint32_t lastReceiveMs() const { return lastRecv_; }

    // ---- output
    void send(const Packet& p);
    void sendPayload(const uint8_t* payload, size_t len);
    // Writes an already framed packet (see frame()).
    void sendRaw(const uint8_t* data, size_t len) { writeOut(data, len); }
    // Frames a payload into out (cap bytes) for broadcasting; returns framed size or 0.
    size_t frame(const uint8_t* payload, size_t len, uint8_t* out, size_t cap) const;

    // Big packets: body(Writer&) is called repeatedly (count pass, optional compressed
    // count pass, emit pass) so nothing larger than the output buffer is ever held.
    // body must write the packet id itself and be deterministic.
    template <class F>
    void sendStreamed(F&& body) {
        if (!open()) return;
        CountSink cs;
        { Writer cw(cs); body(cw); }
        size_t raw = cs.count;
        if (compression_ >= 0 && (int)raw >= compression_) {
            // usually the compressed packet fits a scratch buffer: deflate once
            uint8_t* scratch = compressScratch();
            if (scratch) {
                BufSink bs(scratch, MC_COMPRESS_BUF);
                {
                    DeflateSink ds(bs);
                    Writer dw(ds);
                    body(dw);
                    ds.finish();
                }
                if (!bs.overflowed()) {
                    writeHeader(raw, bs.size());
                    writeOut(scratch, bs.size());
                    return;
                }
            }
            CountSink cc;
            {
                DeflateSink ds(cc);
                Writer dw(ds);
                body(dw);
                ds.finish();
            }
            writeHeader(raw, cc.count);
            DeflateSink ds(outSink_);
            Writer w(ds);
            body(w);
            ds.finish();
        } else {
            writeHeader(raw, 0);
            Writer w(outSink_);
            body(w);
        }
    }

    // A constant packet (id + payload) that was deflated ahead of time (z: the zlib stream
    // of the id and payload): sent as is when this connection compresses it, else plain.
    void sendPrebuilt(int id, const uint8_t* payload, size_t len, const uint8_t* z, size_t zLen) {
        size_t raw = (size_t)varintSize((uint32_t)id) + len;
        if (!open()) return;
        if (compression_ >= 0 && (int)raw >= compression_ && z) {
            writeHeader(raw, zLen);
            writeOut(z, zLen);
            return;
        }
        sendStreamed([&](Writer& w) {
            w.varint(id);
            w.bytes(payload, len);
        });
    }

    // Pushes buffered output to the socket without blocking. false = connection dead.
    bool flush();
    size_t pendingOut() const { return outLen_; }
    uint32_t bytesSent() const { return bytesSent_; }
    uint32_t bytesReceived() const { return bytesRecv_; }

    void setCompression(int threshold) { compression_ = threshold; }
    // Milliseconds the game loop waited for full sockets (all connections) since the last call.
    static uint32_t takeBlockedMs();
    int compression() const { return compression_; }

private:
    class OutSink : public Sink {
    public:
        Connection* c = nullptr;
        void put(const uint8_t* d, size_t n) override { c->writeOut(d, n); }
    };
    friend class OutSink;

    void writeOut(const uint8_t* d, size_t n);
    bool flushBlocking(size_t want);
    static uint8_t* compressScratch();
    // Writes the length header. compressedLen == 0 means "send uncompressed".
    void writeHeader(size_t rawLen, size_t compressedLen);

    Conn* conn_ = nullptr;
    bool closed_ = false;
    uint8_t* in_ = nullptr;
    size_t inLen_ = 0, inPos_ = 0;
    size_t skip_ = 0;               // bytes of an oversized packet still to discard
    uint8_t* out_ = nullptr;
    size_t outLen_ = 0;
    uint8_t* inflated_ = nullptr;   // scratch for decompressed packets (lazy)
    int compression_ = -1;
    uint32_t lastRecv_ = 0;
    uint32_t bytesSent_ = 0, bytesRecv_ = 0;
    OutSink outSink_;
};

}  // namespace mc
