// The previous inflate (bit by bit, as zlib's puff): the reference the fast one is
// checked against (test_inflate.cpp).
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
namespace ref {
static const uint16_t LEN_BASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t LEN_EXTRA[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t DIST_BASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t DIST_EXTRA[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
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

bool inflateZlibRef(const uint8_t* in, size_t inLen, uint8_t* out, size_t outCap, size_t& outLen) {
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

}  // namespace ref
