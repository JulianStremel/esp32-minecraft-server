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
| `perf-phase3-paths.json` | path finding; connection, light and path buffers moved out of internal RAM | 20.0/20.0/20.0 | 13.2/14.2/16.1 | 36/41 | 4638/4299/4887 | |
| `perf-phase4-storage.json` | unbounded world storage (format 3) | 20.0/20.0/20.0 | 13.2/14.1/16.0 | 31/51 | 4511/4047/4684 | |
| `perf-phase5-dimensions.json` | the Nether and the End (dimension-keyed world) | 20.0/20.0/20.0 | 13.0/12.3/15.4 | 39/50 | 4451/4202/4709 | 48.3 ms (light 7.1, exact 38), `bench-phase5-dimensions.json` |

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
- Before `perf-phase3-paths.json` the board could abort under 9 players at view 32:
  internal RAM (shared with WiFi and lwIP) ran out, which also explains the login
  timeouts in the phase 2 and 3 runs. The suite now watches the serial console
  (`--serial`), and `/tps` and the results include the internal RAM minimum (38 KB
  since boot, reached while WiFi starts). The free heap minimum is 383 KB below the
  baseline in A: the exact-light grid while a region job runs.
- `perf-phase5-dimensions.json` was flagged twice against phase 4; both are noise:
  - B, chunks/s -13%: one run (at 40000, 15000) made 10.7/s; re-run at the same spot,
    13.8/s (phase 4: 14.1/s). The other two runs differ by 0 and -0.4/s.
  - A, longest stall 31 -> 39 ms: the runs were 33/40/39 ms; phases 3 and 4 had
    34/44/36 and 36/28/31.
  - idle ms/tick (5.3 against 4.3) is not flagged, but checked with phase 4 and phase 5
    firmware back to back at the same two spots, twice each: 4.4/5.5/4.6/4.8 (phase 4)
    against 4.8/5.2/5.0/5.3 (phase 5), so about +0.25 ms per tick (5%), and 15.4 against
    15.2 chunks/s.
  - Travelling into new Nether chunks: the slowest loop step was 83-103 ms while the
    arrival's chunks were generated on the game loop; they are now loaded on the
    workers first (13-20 ms, the same as without travel, `test/travel_stall.js`).
