// Small shared game types: item stacks, block positions, angles.
#pragma once
#include <math.h>
#include <stdint.h>
#include "mc/io.h"
#include "mc/item.h"

namespace mc {



// Packed block position (x:26, z:26, y:12)
inline uint64_t packPos(int x, int y, int z) {
    return ((uint64_t)(x & 0x3FFFFFF) << 38) | ((uint64_t)(z & 0x3FFFFFF) << 12) | (uint64_t)(y & 0xFFF);
}
inline void unpackPos(uint64_t v, int& x, int& y, int& z) {
    x = (int)(int64_t)(v >> 38);
    if (x >= (1 << 25)) x -= (1 << 26);
    z = (int)((v >> 12) & 0x3FFFFFF);
    if (z >= (1 << 25)) z -= (1 << 26);
    y = (int)(v & 0xFFF);
    if (y >= (1 << 11)) y -= (1 << 12);
}

inline uint8_t angleByte(float deg) {
    float a = fmodf(deg, 360.0f);
    if (a < 0) a += 360.0f;
    return (uint8_t)(int)(a * 256.0f / 360.0f);
}

// Block face directions as used by the protocol (0 down, 1 up, 2 north, 3 south, 4 west, 5 east)
static const int8_t FACE_DX[6] = {0, 0, 0, 0, -1, 1};
static const int8_t FACE_DY[6] = {-1, 1, 0, 0, 0, 0};
static const int8_t FACE_DZ[6] = {0, 0, -1, 1, 0, 0};

}  // namespace mc
