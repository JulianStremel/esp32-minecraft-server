#include "mc/md5.h"
#include <string.h>

namespace mc {

static inline uint32_t rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

static const uint32_t K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
static const uint8_t S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                              5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                              4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

static void block(uint32_t st[4], const uint8_t* p) {
    uint32_t M[16];
    for (int i = 0; i < 16; i++)
        M[i] = (uint32_t)p[i * 4] | ((uint32_t)p[i * 4 + 1] << 8) | ((uint32_t)p[i * 4 + 2] << 16) | ((uint32_t)p[i * 4 + 3] << 24);
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
        else { f = c ^ (b | ~d); g = (7 * i) & 15; }
        uint32_t t = d;
        d = c;
        c = b;
        b = b + rol(a + f + K[i] + M[g], S[i]);
        a = t;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
}

void md5(const uint8_t* data, size_t len, uint8_t out[16]) {
    uint32_t st[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    size_t i = 0;
    for (; i + 64 <= len; i += 64) block(st, data + i);
    uint8_t tail[128];
    size_t rem = len - i;
    memcpy(tail, data + i, rem);
    tail[rem++] = 0x80;
    size_t padded = (rem + 8 <= 64) ? 64 : 128;
    memset(tail + rem, 0, padded - rem);
    uint64_t bits = (uint64_t)len * 8;
    for (int k = 0; k < 8; k++) tail[padded - 8 + k] = (uint8_t)(bits >> (8 * k));
    block(st, tail);
    if (padded == 128) block(st, tail + 64);
    for (int k = 0; k < 4; k++)
        for (int j = 0; j < 4; j++) out[k * 4 + j] = (uint8_t)(st[k] >> (8 * j));
}

void offlineUuid(const char* name, uint8_t out[16]) {
    char buf[16 + 40];
    size_t n = 0;
    const char* prefix = "OfflinePlayer:";
    while (*prefix) buf[n++] = *prefix++;
    while (*name && n < sizeof(buf) - 1) buf[n++] = *name++;
    md5((const uint8_t*)buf, n, out);
    out[6] = (out[6] & 0x0f) | 0x30;  // version 3
    out[8] = (out[8] & 0x3f) | 0x80;  // IETF variant
}

}  // namespace mc
