// CPU cost of the chunk pipeline (generate, light, encode, compress, store),
// measured with plat::micros() on the machine it runs on. Meant for the device:
// tools/emulator/run.sh --bench runs it on the emulated ESP32-S3 (see docs/EMULATOR.md).
#pragma once

namespace mc {

// radius: chunks around the origin to generate ((2r+1)^2); the inner (2r-1)^2 chunks
// (whose neighbours are all resident) are lit, encoded, compressed and stored.
// print receives one line at a time (no trailing newline). quick (the emulator, whose
// CI job has 30 minutes and which runs about 9x slower than its clock): fingerprints of
// the structures' versions only at the newest one, seed 42; timings at radius 2 at most,
// for versions 1 to 3 and the newest. The real board checks and measures every version.
void runChunkBench(int radius, void (*print)(const char* line), bool quick = false);

}  // namespace mc
