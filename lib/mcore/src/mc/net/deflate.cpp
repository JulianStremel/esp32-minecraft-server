#include "mc/net/deflate.h"
#include <stdlib.h>
#include <string.h>
#include "mc/platform.h"

namespace mc {

static const int WINDOW = 4096;
static const int BLOCK = 4096;
static const int HASH_BITS = 12;
static const int MAX_MATCH = 258;

static uint8_t* s_ws = nullptr;   // shared workspace
static bool s_wsBusy = false;

static const uint16_t LEN_BASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                      35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t LEN_EXTRA[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t DIST_BASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                       1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t DIST_EXTRA[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

DeflateSink::DeflateSink(Sink& out) : out_(out), buf_(nullptr), head_(nullptr) {
    if (!s_wsBusy) {
        if (!s_ws) s_ws = (uint8_t*)plat::bigAlloc(WINDOW + BLOCK + (1 << HASH_BITS) * 2);
        if (s_ws) {
            s_wsBusy = true;
            buf_ = s_ws;
            head_ = (uint16_t*)(s_ws + WINDOW + BLOCK);
            memset(head_, 0xFF, (1 << HASH_BITS) * 2);
        }
    }
    stored_ = buf_ == nullptr;
    if (stored_) {
        // stored-block fallback still needs a small buffer for block framing
        buf_ = (uint8_t*)malloc(BLOCK);
    }
    // zlib header: deflate, 4K window (CINFO=4), fastest, FCHECK so header % 31 == 0
    uint8_t cmf = 0x48, flg = 0x01;
    uint16_t h = (uint16_t)(cmf << 8 | flg);
    flg = (uint8_t)(flg + (31 - h % 31) % 31);
    bits(cmf, 8);
    bits(flg, 8);
}

DeflateSink::~DeflateSink() {
    if (!finished_) finish();
    if (stored_) free(buf_);
    else s_wsBusy = false;
}

void DeflateSink::flushOut() {
    if (olen_) { out_.put(obuf_, olen_); olen_ = 0; }
}

void DeflateSink::bits(uint32_t value, int count) {
    bitBuf_ |= value << bitCount_;
    bitCount_ += count;
    while (bitCount_ >= 8) {
        obuf_[olen_++] = (uint8_t)bitBuf_;
        if (olen_ == sizeof(obuf_)) flushOut();
        bitBuf_ >>= 8;
        bitCount_ -= 8;
    }
}

void DeflateSink::huff(uint32_t code, int len) {
    uint32_t rev = 0;
    for (int i = 0; i < len; i++) { rev = (rev << 1) | (code & 1); code >>= 1; }
    bits(rev, len);
}

void DeflateSink::literal(uint8_t b) {
    if (b < 144) huff(0x30 + b, 8);
    else huff(0x190 + (b - 144), 9);
}

void DeflateSink::match(int len, int dist) {
    int li = 0;
    while (li < 28 && LEN_BASE[li + 1] <= len) li++;
    int sym = 257 + li;
    if (sym < 280) huff((uint32_t)(sym - 256), 7);
    else huff((uint32_t)(0xC0 + sym - 280), 8);
    if (LEN_EXTRA[li]) bits((uint32_t)(len - LEN_BASE[li]), LEN_EXTRA[li]);
    int di = 0;
    while (di < 29 && DIST_BASE[di + 1] <= dist) di++;
    huff((uint32_t)di, 5);
    if (DIST_EXTRA[di]) bits((uint32_t)(dist - DIST_BASE[di]), DIST_EXTRA[di]);
}

static inline uint32_t hash3b(const uint8_t* p) {
    uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    return (v * 2654435761u) >> (32 - HASH_BITS);
}

void DeflateSink::compressBlock(bool final) {
    if (stored_) {
        // stored block: BFINAL, BTYPE=00, align, LEN, NLEN, data
        bits(final ? 1 : 0, 3);
        if (bitCount_) bits(0, 8 - bitCount_);
        uint16_t n = (uint16_t)pend_;
        bits(n & 0xFF, 8); bits(n >> 8, 8);
        bits((uint16_t)~n & 0xFF, 8); bits(((uint16_t)~n) >> 8, 8);
        flushOut();
        if (n) out_.put(buf_, n);
        pend_ = 0;
        return;
    }
    // one fixed-Huffman block per call
    bits(final ? 1 : 0, 1);
    bits(1, 2);
    size_t start = hist_, end = hist_ + pend_;
    size_t i = start;
    while (i < end) {
        int bestLen = 0, bestDist = 0;
        if (i + 3 <= end) {
            uint32_t h = hash3b(buf_ + i);
            uint32_t abs = absBase_ + (uint32_t)i;
            uint16_t cand16 = head_[h];
            head_[h] = (uint16_t)abs;
            if (cand16 != 0xFFFF) {
                uint32_t cand = (abs & ~0xFFFFu) | cand16;
                if (cand >= abs) cand -= 0x10000;
                uint32_t dist = abs - cand;
                if (dist > 0 && dist <= (uint32_t)WINDOW && dist <= i) {
                    const uint8_t* a = buf_ + i;
                    const uint8_t* b = a - dist;
                    size_t maxLen = end - i < (size_t)MAX_MATCH ? end - i : (size_t)MAX_MATCH;
                    size_t l = 0;
                    while (l < maxLen && a[l] == b[l]) l++;
                    if (l >= 3) { bestLen = (int)l; bestDist = (int)dist; }
                }
            }
        }
        if (bestLen) {
            match(bestLen, bestDist);
            // index the skipped positions (cheap, improves ratio)
            for (int k = 1; k < bestLen && i + k + 3 <= end; k++)
                head_[hash3b(buf_ + i + k)] = (uint16_t)(absBase_ + i + k);
            i += (size_t)bestLen;
        } else {
            literal(buf_[i]);
            i++;
        }
    }
    huff(0, 7);  // end of block (symbol 256)
    // slide: keep the last WINDOW bytes as history
    size_t total = end;
    size_t keep = total < (size_t)WINDOW ? total : (size_t)WINDOW;
    memmove(buf_, buf_ + total - keep, keep);
    absBase_ += (uint32_t)(total - keep);
    hist_ = keep;
    pend_ = 0;
}

void DeflateSink::put(const uint8_t* d, size_t n) {
    // adler32
    uint32_t a = adlerA_, b = adlerB_;
    for (size_t i = 0; i < n; i++) {
        a += d[i];
        if (a >= 65521) a -= 65521;
        b += a;
        if (b >= 65521) b -= 65521;
    }
    adlerA_ = a;
    adlerB_ = b;
    while (n > 0) {
        size_t base = stored_ ? 0 : hist_;
        size_t space = BLOCK - pend_;
        size_t c = n < space ? n : space;
        memcpy(buf_ + base + pend_, d, c);
        pend_ += c;
        d += c;
        n -= c;
        if (pend_ == (size_t)BLOCK) compressBlock(false);
    }
}

void DeflateSink::finish() {
    if (finished_) return;
    finished_ = true;
    compressBlock(true);
    if (bitCount_) bits(0, 8 - bitCount_);
    uint32_t adler = (adlerB_ << 16) | adlerA_;
    bits((adler >> 24) & 0xFF, 8);
    bits((adler >> 16) & 0xFF, 8);
    bits((adler >> 8) & 0xFF, 8);
    bits(adler & 0xFF, 8);
    flushOut();
}

// ====================================================================== inflate
namespace {

struct Huff {
    int16_t count[16];
    int16_t symbol[288];
};

struct State {
    const uint8_t* in;
    size_t inLen, inPos;
    uint32_t bitBuf;
    int bitCnt;
    uint8_t* out;
    size_t outCap, outPos;
    bool err;
};

int getBits(State& s, int need) {
    uint32_t v = s.bitBuf;
    while (s.bitCnt < need) {
        if (s.inPos >= s.inLen) { s.err = true; return 0; }
        v |= (uint32_t)s.in[s.inPos++] << s.bitCnt;
        s.bitCnt += 8;
    }
    s.bitBuf = v >> need;
    s.bitCnt -= need;
    return (int)(v & ((1u << need) - 1));
}

int decodeSym(State& s, const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= getBits(s, 1);
        if (s.err) return -1;
        int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s.err = true;
    return -1;
}

bool buildHuff(Huff& h, const uint8_t* lengths, int n) {
    for (int i = 0; i < 16; i++) h.count[i] = 0;
    for (int i = 0; i < n; i++) h.count[lengths[i]]++;
    if (h.count[0] == n) return true;  // no codes (allowed for distance tree)
    int left = 1;
    for (int len = 1; len < 16; len++) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return false;
    }
    int16_t offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + h.count[len];
    for (int i = 0; i < n; i++)
        if (lengths[i]) h.symbol[offs[lengths[i]]++] = (int16_t)i;
    return true;
}

bool codes(State& s, const Huff& lit, const Huff& dist) {
    for (;;) {
        int sym = decodeSym(s, lit);
        if (s.err || sym < 0) return false;
        if (sym < 256) {
            if (s.outPos >= s.outCap) return false;
            s.out[s.outPos++] = (uint8_t)sym;
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            int len = LEN_BASE[sym] + getBits(s, LEN_EXTRA[sym]);
            int ds = decodeSym(s, dist);
            if (s.err || ds < 0 || ds >= 30) return false;
            size_t d = (size_t)DIST_BASE[ds] + (size_t)getBits(s, DIST_EXTRA[ds]);
            if (s.err || d > s.outPos) return false;
            if (s.outPos + (size_t)len > s.outCap) return false;
            for (int k = 0; k < len; k++, s.outPos++) s.out[s.outPos] = s.out[s.outPos - d];
        }
    }
}

}  // namespace

bool inflateZlib(const uint8_t* in, size_t inLen, uint8_t* out, size_t outCap, size_t& outLen) {
    outLen = 0;
    if (inLen < 6) return false;
    if ((in[0] & 0x0F) != 8 || ((in[0] << 8) | in[1]) % 31 != 0 || (in[1] & 0x20)) return false;
    State s = {in, inLen - 4, 2, 0, 0, out, outCap, 0, false};
    Huff* lit = (Huff*)malloc(sizeof(Huff) * 2);
    if (!lit) return false;
    Huff* dist = lit + 1;
    bool ok = true;
    int last = 0;
    while (ok && !last) {
        last = getBits(s, 1);
        int type = getBits(s, 2);
        if (s.err) { ok = false; break; }
        if (type == 0) {
            s.bitBuf = 0;
            s.bitCnt = 0;
            if (s.inPos + 4 > s.inLen) { ok = false; break; }
            unsigned len = s.in[s.inPos] | (s.in[s.inPos + 1] << 8);
            unsigned nlen = s.in[s.inPos + 2] | (s.in[s.inPos + 3] << 8);
            s.inPos += 4;
            if (len != (~nlen & 0xFFFF) || s.inPos + len > s.inLen || s.outPos + len > s.outCap) { ok = false; break; }
            memcpy(s.out + s.outPos, s.in + s.inPos, len);
            s.inPos += len;
            s.outPos += len;
        } else if (type == 1) {
            uint8_t l[288];
            int i = 0;
            for (; i < 144; i++) l[i] = 8;
            for (; i < 256; i++) l[i] = 9;
            for (; i < 280; i++) l[i] = 7;
            for (; i < 288; i++) l[i] = 8;
            buildHuff(*lit, l, 288);
            for (i = 0; i < 30; i++) l[i] = 5;
            buildHuff(*dist, l, 30);
            ok = codes(s, *lit, *dist);
        } else if (type == 2) {
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            int nlen = getBits(s, 5) + 257, ndist = getBits(s, 5) + 1, ncode = getBits(s, 4) + 4;
            if (s.err || nlen > 286 || ndist > 30) { ok = false; break; }
            uint8_t lengths[320];
            memset(lengths, 0, sizeof(lengths));
            for (int i = 0; i < ncode; i++) lengths[order[i]] = (uint8_t)getBits(s, 3);
            if (!buildHuff(*lit, lengths, 19)) { ok = false; break; }
            int idx = 0;
            while (idx < nlen + ndist) {
                int sym = decodeSym(s, *lit);
                if (s.err || sym < 0) { ok = false; break; }
                if (sym < 16) {
                    lengths[idx++] = (uint8_t)sym;
                } else {
                    int len = 0, rep;
                    if (sym == 16) {
                        if (idx == 0) { ok = false; break; }
                        len = lengths[idx - 1];
                        rep = 3 + getBits(s, 2);
                    } else if (sym == 17) {
                        rep = 3 + getBits(s, 3);
                    } else {
                        rep = 11 + getBits(s, 7);
                    }
                    if (idx + rep > nlen + ndist) { ok = false; break; }
                    while (rep--) lengths[idx++] = (uint8_t)len;
                }
            }
            if (!ok) break;
            if (!buildHuff(*lit, lengths, nlen) || !buildHuff(*dist, lengths + nlen, ndist)) { ok = false; break; }
            ok = codes(s, *lit, *dist);
        } else {
            ok = false;
        }
        if (s.err) ok = false;
    }
    free(lit);
    if (!ok) return false;
    // verify adler32
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < s.outPos; i++) {
        a = (a + out[i]) % 65521;
        b = (b + a) % 65521;
    }
    const uint8_t* t = in + inLen - 4;
    uint32_t want = ((uint32_t)t[0] << 24) | ((uint32_t)t[1] << 16) | ((uint32_t)t[2] << 8) | t[3];
    if (((b << 16) | a) != want) return false;
    outLen = s.outPos;
    return true;
}

}  // namespace mc
