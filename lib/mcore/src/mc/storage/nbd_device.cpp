#include "mc/storage/nbd_device.h"
#include <stdio.h>
#include <string.h>

namespace mc {

static const uint64_t NBD_MAGIC = 0x4e42444d41474943ull;       // "NBDMAGIC"
static const uint64_t IHAVEOPT = 0x49484156454F5054ull;        // "IHAVEOPT"
static const uint64_t OPT_REPLY_MAGIC = 0x0003e889045565a9ull;
static const uint32_t REQUEST_MAGIC = 0x25609513;
static const uint32_t SIMPLE_REPLY_MAGIC = 0x67446698;
static const uint32_t OPT_EXPORT_NAME = 1, OPT_GO = 7;
static const uint32_t REP_ACK = 1, REP_INFO = 3;
static const uint16_t CMD_READ = 0, CMD_WRITE = 1, CMD_DISC = 2, CMD_FLUSH = 3;
static const uint16_t FLAG_SEND_FLUSH = 4;
static const uint32_t IO_TIMEOUT_MS = 8000;

static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (24 - 8 * i)); }
static void put64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (56 - 8 * i)); }
static uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t get64(const uint8_t* p) { return (uint64_t)get32(p) << 32 | get32(p + 4); }

NbdDevice::NbdDevice(const char* host, uint16_t port, const char* exportName) : port_(port) {
    snprintf(host_, sizeof(host_), "%s", host);
    snprintf(export_, sizeof(export_), "%s", exportName ? exportName : "");
    snprintf(desc_, sizeof(desc_), "nbd://%s:%u/%s (not connected)", host_, port_, export_);
}

NbdDevice::~NbdDevice() { disconnect(); }

bool NbdDevice::sendAll(const void* d, size_t n) {
    const uint8_t* p = (const uint8_t*)d;
    while (n > 0) {
        if (wlen_ == sizeof(wbuf_) && !flushSend()) return false;
        size_t c = sizeof(wbuf_) - wlen_;
        if (c > n) c = n;
        memcpy(wbuf_ + wlen_, p, c);
        wlen_ += c;
        p += c;
        n -= c;
    }
    return true;
}

bool NbdDevice::flushSend() {
    if (!wlen_) return true;
    bool ok = conn_ && connWriteFully(conn_, wbuf_, wlen_, IO_TIMEOUT_MS);
    wlen_ = 0;
    return ok;
}

bool NbdDevice::recvAll(void* d, size_t n) {
    if (!flushSend()) return false;
    return conn_ && connReadFully(conn_, (uint8_t*)d, n, IO_TIMEOUT_MS);
}

bool NbdDevice::fail(const char* what) {
    MC_LOGW("nbd %s:%u: %s", host_, port_, what);
    stats_.errors++;
    disconnect();
    return false;
}

void NbdDevice::disconnect() {
    if (conn_) {
        conn_->close();
        delete conn_;
        conn_ = nullptr;
    }
    pendingWrites_ = 0;
    streamLeft_ = 0;
    wlen_ = 0;
    snprintf(desc_, sizeof(desc_), "nbd://%s:%u/%s (disconnected)", host_, port_, export_);
}

bool NbdDevice::connect() {
    disconnect();
    lastAttemptMs_ = plat::millis();
    conn_ = plat::connectTcp(host_, port_, 5000);
    if (!conn_) {
        backoffMs_ = backoffMs_ ? (backoffMs_ * 2 > 30000 ? 30000 : backoffMs_ * 2) : 1000;
        MC_LOGW("nbd: cannot connect to %s:%u (retry in %u ms)", host_, port_, (unsigned)backoffMs_);
        return false;
    }
    if (!handshake()) {
        backoffMs_ = backoffMs_ ? (backoffMs_ * 2 > 30000 ? 30000 : backoffMs_ * 2) : 1000;
        return false;
    }
    backoffMs_ = 0;
    snprintf(desc_, sizeof(desc_), "nbd://%s:%u/%s (%llu MiB%s)", host_, port_, export_,
             (unsigned long long)(size_ >> 20), readOnly() ? ", read-only" : "");
    MC_LOGI("nbd: connected to %s", desc_);
    return true;
}

bool NbdDevice::available() {
    if (conn_ && conn_->connected()) return true;
    if (conn_) disconnect();
    if (plat::millis() - lastAttemptMs_ < backoffMs_) return false;
    bool ok = connect();
    if (ok) stats_.reconnects++;
    return ok;
}

bool NbdDevice::handshake() {
    uint8_t b[256];
    if (!recvAll(b, 18)) return fail("no greeting");
    if (get64(b) != NBD_MAGIC || get64(b + 8) != IHAVEOPT) return fail("not an NBD server (or oldstyle only)");
    uint16_t hflags = get16(b + 16);
    bool noZeroes = (hflags & 2) != 0;
    uint8_t cf[4];
    put32(cf, (hflags & 1) | (noZeroes ? 2 : 0));
    if (!sendAll(cf, 4)) return fail("handshake write");

    size_t nl = strlen(export_);
    // ---- NBD_OPT_GO: name + one info request (NBD_INFO_EXPORT is always sent anyway)
    if (hflags & 1) {
        uint8_t h[16];
        put64(h, IHAVEOPT);
        put32(h + 8, OPT_GO);
        put32(h + 12, (uint32_t)(4 + nl + 2));
        uint8_t tail[2] = {0, 0};
        uint8_t ln[4];
        put32(ln, (uint32_t)nl);
        if (!sendAll(h, 16) || !sendAll(ln, 4) || (nl && !sendAll(export_, nl)) || !sendAll(tail, 2)) return fail("opt write");
        bool gotInfo = false;
        for (int guard = 0; guard < 16; guard++) {
            uint8_t r[20];
            if (!recvAll(r, 20)) return fail("opt reply");
            if (get64(r) != OPT_REPLY_MAGIC) return fail("bad option reply magic");
            uint32_t type = get32(r + 12), len = get32(r + 16);
            if (type & 0x80000000u) {
                // error: discard payload, fall back to EXPORT_NAME (old servers)
                while (len) {
                    uint32_t c = len > sizeof(b) ? sizeof(b) : len;
                    if (!recvAll(b, c)) return fail("opt error payload");
                    len -= c;
                }
                if (type == 0x80000001u) break;  // NBD_REP_ERR_UNSUP
                char msg[64];
                snprintf(msg, sizeof(msg), "export '%s' refused (error 0x%x)", export_, (unsigned)type);
                return fail(msg);
            }
            if (type == REP_INFO) {
                uint8_t info[256];
                if (len > sizeof(info)) return fail("oversized info");
                if (!recvAll(info, len)) return fail("info payload");
                if (len >= 12 && get16(info) == 0) {
                    size_ = get64(info + 2);
                    flags_ = get16(info + 10);
                    gotInfo = true;
                }
                continue;
            }
            // skip payloads of other replies
            while (len) {
                uint32_t c = len > sizeof(b) ? sizeof(b) : len;
                if (!recvAll(b, c)) return fail("opt payload");
                len -= c;
            }
            if (type == REP_ACK) {
                if (!gotInfo) return fail("server sent no export info");
                return true;
            }
        }
    }
    // ---- fallback: NBD_OPT_EXPORT_NAME (no reply header; size + flags follow directly)
    uint8_t h[16];
    put64(h, IHAVEOPT);
    put32(h + 8, OPT_EXPORT_NAME);
    put32(h + 12, (uint32_t)nl);
    if (!sendAll(h, 16) || (nl && !sendAll(export_, nl))) return fail("export name write");
    if (!recvAll(b, 10)) return fail("export name reply (unknown export?)");
    size_ = get64(b);
    flags_ = get16(b + 8);
    if (!noZeroes && !recvAll(b, 124)) return fail("zero padding");
    return true;
}

bool NbdDevice::sendRequest(uint16_t type, uint64_t handle, uint64_t off, uint32_t len) {
    uint8_t r[28];
    put32(r, REQUEST_MAGIC);
    put16(r + 4, 0);
    put16(r + 6, type);
    put64(r + 8, handle);
    put64(r + 16, off);
    put32(r + 24, len);
    return sendAll(r, 28);
}

bool NbdDevice::readReply(uint64_t& handle, uint32_t& error) {
    uint8_t r[16];
    if (!recvAll(r, 16)) return false;
    if (get32(r) != SIMPLE_REPLY_MAGIC) return false;
    error = get32(r + 4);
    handle = get64(r + 8);
    return true;
}

// Collects the replies of posted writes.
bool NbdDevice::drainWrites() {
    while (pendingWrites_ > 0) {
        uint64_t h;
        uint32_t err;
        if (!readReply(h, err)) return fail("write reply lost");
        pendingWrites_--;
        if (err) {
            char msg[48];
            snprintf(msg, sizeof(msg), "write failed (errno %u)", (unsigned)err);
            return fail(msg);
        }
    }
    return true;
}

bool NbdDevice::read(uint64_t off, void* buf, uint32_t len) {
    ReadOp op = {off, buf, len};
    return readMany(&op, 1);
}

bool NbdDevice::readMany(ReadOp* ops, int n) {
    if (!available() || streamLeft_) return false;
    uint32_t t0 = plat::millis();
    if (!drainWrites()) return false;
    uint64_t base = nextHandle_;
    nextHandle_ += (uint64_t)n;
    for (int i = 0; i < n; i++)
        if (!sendRequest(CMD_READ, base + i, ops[i].offset, ops[i].len)) return fail("read request");
    // replies may arrive in any order; the data follows each reply header
    for (int k = 0; k < n; k++) {
        uint64_t h;
        uint32_t err;
        if (!readReply(h, err)) return fail("read reply");
        if (h < base || h >= base + (uint64_t)n) return fail("unexpected reply handle");
        ReadOp& op = ops[h - base];
        if (err) {
            char msg[48];
            snprintf(msg, sizeof(msg), "read failed (errno %u)", (unsigned)err);
            // the stream stays in sync: error replies carry no data
            stats_.errors++;
            MC_LOGW("nbd: %s", msg);
            for (int j = k + 1; j < n; j++) {
                if (!readReply(h, err)) return fail("read reply");
                if (!err && !recvAll(ops[h - base].buf, ops[h - base].len)) return fail("read data");
            }
            return false;
        }
        if (!recvAll(op.buf, op.len)) return fail("read data");
        stats_.reads++;
        stats_.bytesRead += op.len;
    }
    stats_.lastLatencyMs = plat::millis() - t0;
    return true;
}

bool NbdDevice::write(uint64_t off, const void* buf, uint32_t len) {
    return beginWrite(off, len) && writeData(buf, len) && endWrite();
}

bool NbdDevice::beginWrite(uint64_t off, uint32_t len) {
    if (!available() || streamLeft_) return false;
    if (readOnly()) { MC_LOGE("nbd: export is read-only"); return false; }
    if (pendingWrites_ >= 16 && !drainWrites()) return false;
    if (!sendRequest(CMD_WRITE, nextHandle_++, off, len)) return fail("write request");
    streamLeft_ = len;
    stats_.writes++;
    stats_.bytesWritten += len;
    return true;
}

bool NbdDevice::writeData(const void* buf, uint32_t len) {
    if (!conn_ || len > streamLeft_) return false;
    if (!sendAll(buf, len)) return fail("write data");
    streamLeft_ -= len;
    return true;
}

bool NbdDevice::endWrite() {
    if (!conn_ || streamLeft_ != 0) return fail("short write stream");
    pendingWrites_++;
    // push it out now; the reply is collected later
    if (!flushSend()) return fail("write flush");
    return true;
}

bool NbdDevice::flush() {
    if (!available() || streamLeft_) return false;
    if (!drainWrites()) return false;
    stats_.flushes++;
    if (!(flags_ & FLAG_SEND_FLUSH)) return true;
    uint64_t handle = nextHandle_++;
    if (!sendRequest(CMD_FLUSH, handle, 0, 0)) return fail("flush request");
    uint64_t h;
    uint32_t err;
    if (!readReply(h, err) || h != handle) return fail("flush reply");
    return err == 0;
}

}  // namespace mc
