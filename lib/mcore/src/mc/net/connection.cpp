#include "mc/net/connection.h"
#include <stdlib.h>
#include <string.h>
#include "mc/net/deflate.h"

namespace mc {

// ---------------------------------------------------------------- Packet pool
// The server is single threaded; a handful of scratch buffers covers nesting
// (building one packet while another is still alive).
static const int POOL = 3;
static uint8_t* s_pool[POOL];
static bool s_used[POOL];

Packet::Packet(int id) : w(sink_), buf_(nullptr), slot_(-1), sink_(nullptr, 0) {
    for (int i = 0; i < POOL; i++) {
        if (!s_used[i]) {
            if (!s_pool[i]) s_pool[i] = (uint8_t*)malloc(MC_PACKET_SCRATCH);
            if (s_pool[i]) { s_used[i] = true; slot_ = i; buf_ = s_pool[i]; }
            break;
        }
    }
    if (!buf_) buf_ = (uint8_t*)malloc(MC_PACKET_SCRATCH);
    sink_ = BufSink(buf_, buf_ ? MC_PACKET_SCRATCH : 0);
    w.varint(id);
}

Packet::~Packet() {
    if (slot_ >= 0) s_used[slot_] = false;
    else free(buf_);
}

// ---------------------------------------------------------------- Connection
// shared by all connections (single-threaded server)
uint8_t* Connection::compressScratch() {
    static uint8_t* buf = nullptr;
    static bool tried = false;
    if (!buf && !tried) {
        tried = true;
        buf = (uint8_t*)plat::bigAlloc(MC_COMPRESS_BUF);
    }
    return buf;
}

Connection::Connection() {}

Connection::~Connection() {
    close();
    free(in_);
    free(out_);
    free(inflated_);
}

void Connection::attach(Conn* c) {
    close();
    conn_ = c;
    closed_ = false;
    if (!in_) in_ = (uint8_t*)malloc(MC_IN_BUF);
    if (!out_) out_ = (uint8_t*)malloc(MC_OUT_BUF);
    inLen_ = inPos_ = 0;
    outLen_ = 0;
    skip_ = 0;
    compression_ = -1;
    lastRecv_ = plat::millis();
    bytesSent_ = bytesRecv_ = 0;
    outSink_.c = this;
    if (!in_ || !out_) closed_ = true;
}

void Connection::close() {
    if (conn_) {
        if (!closed_ && outLen_) flush();
        conn_->close();
        delete conn_;
        conn_ = nullptr;
    }
    closed_ = true;
    outLen_ = 0;
}

bool Connection::poll() {
    if (!open()) return false;
    // compact consumed input
    if (inPos_ > 0) {
        memmove(in_, in_ + inPos_, inLen_ - inPos_);
        inLen_ -= inPos_;
        inPos_ = 0;
    }
    for (int rounds = 0; rounds < 4; rounds++) {
        if (skip_ > 0) {
            uint8_t tmp[256];
            int r = conn_->read(tmp, skip_ < sizeof(tmp) ? skip_ : sizeof(tmp));
            if (r < 0) { closed_ = true; return false; }
            if (r == 0) break;
            skip_ -= (size_t)r;
            bytesRecv_ += (uint32_t)r;
            lastRecv_ = plat::millis();
            continue;
        }
        if (inLen_ >= MC_IN_BUF) break;
        int r = conn_->read(in_ + inLen_, MC_IN_BUF - inLen_);
        if (r < 0) { closed_ = true; return false; }
        if (r == 0) break;
        inLen_ += (size_t)r;
        bytesRecv_ += (uint32_t)r;
        lastRecv_ = plat::millis();
    }
    return true;
}

bool Connection::nextPacket(int& id, Reader& out) {
    for (;;) {
        if (skip_ > 0 || inPos_ >= inLen_) return false;
        // parse the length prefix
        uint32_t len = 0;
        size_t p = inPos_;
        int shift = 0;
        bool complete = false;
        while (p < inLen_ && shift < 35) {
            uint8_t b = in_[p++];
            len |= (uint32_t)(b & 0x7F) << shift;
            shift += 7;
            if (!(b & 0x80)) { complete = true; break; }
        }
        if (!complete) {
            if (shift >= 35) { closed_ = true; }
            return false;
        }
        if (len == 0 || len > 2097151) { closed_ = true; return false; }
        size_t header = p - inPos_;
        if (header + len > MC_IN_BUF) {
            // too big for us: discard it entirely
            size_t have = inLen_ - p;
            if (have >= len) {
                inPos_ = p + len;
                continue;
            }
            skip_ = len - have;
            inPos_ = inLen_ = 0;
            return false;
        }
        if (inLen_ - p < len) return false;  // wait for the rest
        const uint8_t* body = in_ + p;
        size_t bodyLen = len;
        inPos_ = p + len;
        if (compression_ >= 0) {
            Reader hr(body, bodyLen);
            int32_t dataLen = hr.varint();
            if (!hr.ok()) { closed_ = true; return false; }
            if (dataLen == 0) {
                body = hr.cursor();
                bodyLen = hr.remaining();
            } else {
                if (dataLen > MC_INFLATE_MAX) continue;  // ignore oversized packet
                if (!inflated_) inflated_ = (uint8_t*)malloc(MC_INFLATE_MAX);
                if (!inflated_) continue;
                size_t outLen = 0;
                if (!inflateZlib(hr.cursor(), hr.remaining(), inflated_, MC_INFLATE_MAX, outLen) ||
                    outLen != (size_t)dataLen) {
                    closed_ = true;
                    return false;
                }
                body = inflated_;
                bodyLen = outLen;
            }
        }
        Reader r(body, bodyLen);
        id = r.varint();
        if (!r.ok()) { closed_ = true; return false; }
        out = r;
        return true;
    }
}

// ---------------------------------------------------------------- output
bool Connection::flush() {
    if (!open()) return false;
    size_t sent = 0;
    while (sent < outLen_) {
        int r = conn_->write(out_ + sent, outLen_ - sent);
        if (r < 0) { closed_ = true; outLen_ = 0; return false; }
        if (r == 0) break;
        sent += (size_t)r;
    }
    bytesSent_ += (uint32_t)sent;
    if (sent > 0) {
        memmove(out_, out_ + sent, outLen_ - sent);
        outLen_ -= sent;
    }
    return true;
}

// Waits (bounded) until `want` bytes fit into the output buffer.
bool Connection::flushBlocking(size_t want) {
    uint32_t start = plat::millis();
    while (open() && MC_OUT_BUF - outLen_ < want) {
        size_t before = outLen_;
        if (!flush()) return false;
        if (outLen_ == before) {
            if (plat::millis() - start > 5000) {
                MC_LOGW("%s: send timeout, dropping connection", peer());
                closed_ = true;
                return false;
            }
            plat::yield();
        } else {
            start = plat::millis();
        }
    }
    return open();
}

void Connection::writeOut(const uint8_t* d, size_t n) {
    if (!open()) return;
    while (n > 0) {
        size_t space = MC_OUT_BUF - outLen_;
        if (space == 0) {
            if (!flushBlocking(MC_OUT_BUF / 4)) return;
            continue;
        }
        size_t c = n < space ? n : space;
        memcpy(out_ + outLen_, d, c);
        outLen_ += c;
        d += c;
        n -= c;
    }
}

// Header for a packet of rawLen bytes (id + body). With compression enabled and
// compressedLen > 0 the body that follows is a zlib stream of compressedLen bytes.
void Connection::writeHeader(size_t rawLen, size_t compressedLen) {
    uint8_t hdr[10];
    BufSink hs(hdr, sizeof(hdr));
    Writer hw(hs);
    if (compression_ < 0) {
        hw.varint((int32_t)rawLen);
    } else if (compressedLen == 0) {
        hw.varint((int32_t)rawLen + 1);
        hw.varint(0);
    } else {
        hw.varint((int32_t)(varintSize((uint32_t)rawLen) + compressedLen));
        hw.varint((int32_t)rawLen);
    }
    writeOut(hdr, hs.size());
}

static size_t compressedSize(const uint8_t* payload, size_t len) {
    CountSink cs;
    DeflateSink d(cs);
    d.put(payload, len);
    d.finish();
    return cs.count;
}

size_t Connection::frame(const uint8_t* payload, size_t len, uint8_t* out, size_t cap) const {
    BufSink s(out, cap);
    Writer w(s);
    if (compression_ < 0) {
        w.varint((int32_t)len);
        w.bytes(payload, len);
    } else if ((int)len < compression_) {
        w.varint((int32_t)len + 1);
        w.varint(0);
        w.bytes(payload, len);
    } else {
        size_t clen = compressedSize(payload, len);
        w.varint((int32_t)(varintSize((uint32_t)len) + clen));
        w.varint((int32_t)len);
        DeflateSink d(s);
        d.put(payload, len);
        d.finish();
    }
    return s.overflowed() ? 0 : s.size();
}

void Connection::send(const Packet& p) {
    if (p.overflowed()) {
        // never emit a truncated packet: it would desynchronise the stream
        Reader r(p.data(), p.size());
        MC_LOGE("packet 0x%02x exceeds MC_PACKET_SCRATCH (%d bytes), not sent", (unsigned)r.varint(), MC_PACKET_SCRATCH);
        return;
    }
    sendPayload(p.data(), p.size());
}

void Connection::sendPayload(const uint8_t* payload, size_t len) {
    if (!open()) return;
    if (compression_ >= 0 && (int)len >= compression_) {
        writeHeader(len, compressedSize(payload, len));
        DeflateSink d(outSink_);
        d.put(payload, len);
        d.finish();
    } else {
        writeHeader(len, 0);
        writeOut(payload, len);
    }
}

}  // namespace mc
