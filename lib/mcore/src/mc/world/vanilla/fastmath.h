// sin and cos as polynomials in floats: a few multiplications instead of newlib's software
// sinf and cosf (several microseconds each on the ESP32-S3). The same results on every
// platform (no fused multiply-add: lib/mcore/CMakeLists.txt, host/Makefile), within about
// 1e-6 of the exact values.
#pragma once

namespace mc {
namespace vanilla {

inline float fastSin(float x) {
    const float TWO_PI = 6.28318531f, INV_TWO_PI = 0.159154943f, PI = 3.14159265f, HALF_PI = 1.57079633f;
    float k = x * INV_TWO_PI + 0.5f;   // to [-pi, pi]
    int ki = (int)k;
    if ((float)ki > k) ki--;
    x -= TWO_PI * (float)ki;
    if (x > HALF_PI) x = PI - x;   // to [-pi/2, pi/2]
    else if (x < -HALF_PI) x = -PI - x;
    float x2 = x * x;
    return x * (1.0f + x2 * (-0.166666667f + x2 * (0.00833333333f + x2 * (-0.000198412698f + x2 * 2.75573192e-6f))));
}

inline float fastCos(float x) { return fastSin(x + 1.57079633f); }

}  // namespace vanilla
}  // namespace mc
