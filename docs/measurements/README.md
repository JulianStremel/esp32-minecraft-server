# Performance measurements on the ESP32-S3 board

Waveshare ESP32-S3-Touch-AMOLED-1.8 (8 MB octal PSRAM), WiFi, NBD storage on a PC.
Each `perf-*.json` is a `test/perf_suite.js` result: the median of 3 runs per scenario
over fresh terrain, plus the device micro-benchmark (`test/hardware_bench.js`) where
noted. Compare two with `node tools/perf_compare.js <before> <after>`.

Scenarios:
- **A**: 8 players in spectator mode flying apart at 11 blocks/s, view distance 32, 60 s
- **B**: 4 players at 5 blocks/s, view distance 12, 60 s
- **idle**: 1 hovering player, view distance 12, 30 s

| File | Change | TPS (A/B/idle) | chunks/s generated (A/B/idle) | longest stall ms (A/B) | free heap min KB (A/B/idle) | new chunk, bench |
|---|---|---|---|---|---|---|
| `perf-baseline.json` | before the light rewrite | 20.0/20.0/20.0 | 13.2/14.5/15.8 | 33/46 | 5021/4540/4954 | 57.0 ms (light 15.9) |
| `perf-phase1-light.json` | cross-chunk light, faster per-chunk light | 20.0/20.0/20.0 | 12.9/14.0/15.5 | 38/44 | 4434/3845/4379 | 48.1 ms (light 7.0, exact 36) |
| `perf-phase1-light-trim2.json` | snapshot cache trimmed every tick (B, idle) | -/20.0/20.0 | -/14.1/15.3 | -/54 | -/4247/4781 | |
| `perf-phase2-spawning.json` | mob spawning by light level | 20.0/20.0/20.0 | 13.9/14.3/15.2 | 33/47 | 4454/4188/4705 | |

Notes:
- The device bench measures one core without contention; on the server two workers
  share PSRAM bandwidth, and exact light took 38–140 ms per chunk depending on the
  terrain (one job at a time).
- `perf-phase1-light-trim.json` (smaller light scratch buffers, B and idle) changed
  nothing measurable; the free-heap drop came from snapshots kept up to a second after
  their jobs, fixed in trim2.
- `perf-phase2-spawning.json`: one run of A timed out logging in a flyer while the
  previous run's players were still leaving (a re-run passed); perf_suite now retries
  such a run once at the next spot and waits longer between runs.
