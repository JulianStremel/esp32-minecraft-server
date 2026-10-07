// CPU cost of the chunk pipeline (generate, light, encode, compress, store),
// measured with plat::micros() on the machine it runs on. Meant for the device:
// tools/qemu/run.sh --bench runs it on the emulated ESP32-S3 (see docs/QEMU.md).
#pragma once

namespace mc {

// radius: chunks around the origin to generate ((2r+1)^2); the inner (2r-1)^2 chunks
// (whose neighbours are all resident) are lit, encoded, compressed and stored.
// print receives one line at a time (no trailing newline).
void runChunkBench(int radius, void (*print)(const char* line));

}  // namespace mc
