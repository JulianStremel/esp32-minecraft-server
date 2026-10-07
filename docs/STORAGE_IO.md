# Dedicated storage I/O thread

The storage connection now has one owner: `mcstorage`, pinned to core 0 at
FreeRTOS priority 2 with a 16 KiB stack. CPU generation/lighting/compression remain
on the existing workers. This change does not change the world format, generator,
lighting, or gameplay rules.

## Requests and persistence

`StorageIo` accepts at most 32 outstanding asynchronous tasks, including completed
results not yet collected. A FIFO preserves write/read ordering, including logout
followed by another login. A completion wakes the game loop, which applies the
result and releases its memory. The queue lock is never held during I/O or callbacks.

- Chunk loads: pipelined NBD fetch → CPU decode/generation → game-loop adoption.
  Load admission remains bounded and distance prioritized. Stale pending fetches
  are discarded; a corrupt record's fallback load also runs on the I/O thread.
- Chunk saves: snapshot and scheduled ticks → CPU encoding → I/O write and flush
  → game-loop acknowledgement. The live chunk stays pinned throughout. Edits made
  after the snapshot remain dirty; failed encoding, writes or flushes restore dirty
  state for retry. Dirty eviction schedules a save and keeps the chunk resident.
- Login reads finish asynchronously with a player-session check. Abandoned/reused
  slots cannot receive an old result; unavailable storage rejects login rather than
  treating a failed read as a new player. Player and metadata writes own snapshots.
- `/save-all` schedules a save and responds only after the flush; errors are reported.
  Shutdown drains both CPU and I/O stages before destroying their owners. `/storage`
  reads a cached diagnostic snapshot rather than touching the connection.

Explicit synchronous `World::load`/`saveAll` compatibility calls still wait for the
I/O thread. These cover startup/shutdown, edits or interactions requiring an unloaded
chunk, respawn lookups, and direct metadata commands. Changing `/workers` drains
pending work. If the bounded queue fills, essential player/metadata snapshots use
one reserved synchronous slot rather than dropping data. This is not a guarantee
that every command remains nonblocking during a prolonged storage outage. Existing
NBD timeouts/backoff are unchanged; shutdown can wait for them. Failed logout writes
are logged, with no new persistent retry journal for player records.

## Before/after measurement

Native workload: fresh 256 MiB NBD export, seed 42, six creative clients, two CPU
workers, view distance 4, mobs disabled, 30 seconds. All six clients teleport every
2 seconds on a fixed wall-clock schedule (90 teleports); an independent probe asks
for `/tps` approximately every 200 ms. The Python server optionally adds **10 ms
of serial service time per NBD request**, not a simulated 10 ms network RTT.

The original executable was copied before modifying the server. The repeat pair
below ran sequentially without a concurrent firmware build or emulator. TPS and
loop stalls come from sampled two-second server windows; latency is host-observed
command round-trip time. They are not physical ESP32 timings.

| Native measurement | Before, 10 ms service | After, 10 ms service | Before, no delay | After, no delay |
|---|---:|---:|---:|---:|
| Command latency p50 | 63 ms | 1 ms | 1 ms | 1 ms |
| Command latency p95 | 117 ms | 2 ms | 2 ms | 2 ms |
| Maximum command latency | 243 ms | 3 ms | 6 ms | 4 ms |
| Maximum reported loop stall | 247 ms | 4 ms | 12 ms | 5 ms |
| Mean sampled TPS | 19.98 | 20.00 | 20.00 | 20.00 |
| Minimum sampled TPS | 19.8 | 20.0 | 20.0 | 20.0 |
| Chunk packets / host second | 41.96 | 43.93 | 48.78 | 48.55 |

The initial pair also showed p95 latency falling from 126 to 2 ms and the maximum
reported loop stall from 250 to 10 ms. No throughput improvement is claimed for
CPU-bound workloads.

### ESP32-S3 runtime profile

Same esp-emulator 0.48.0, S3/8 MiB profile, seed 42, six clients, two CPU workers,
20 firmware seconds and ten radial moves per client as the previous
[S3 profile](S3_CPU_PROFILE.md). This uses ordinary local NBD with no injected delay.
FreeRTOS runtime counters and `esp_timer` define guest CPU/idle time; host CPU and
elapsed time are separate. There is one run per revision, so small differences can
reflect scheduling and workload variation (including incidental entity activity).

| Measurement | Before | After |
|---|---:|---:|
| Firmware window | 20.010 s | 20.020 s |
| Core 0 idle | 8.02% / 1.604 s | 9.35% / 1.871 s |
| Core 1 idle | 7.94% / 1.588 s | 9.22% / 1.845 s |
| Storage task CPU, one-core share | — | 0.77% |
| Chunk packets within window | 933 | 913 |
| TPS, following diagnostic window | 20.0 | 20.0 |
| Maximum loop stall, following diagnostic window | 8 ms | 4 ms |
| Host elapsed time | 304.3 s | 288.5 s |
| Emulator host CPU, one-core share | 99.76% | 99.95% |

Both cores remain heavily occupied by generation/lighting workers. This run shows
no throughput win (about 2% fewer chunk packets); moving I/O primarily removes
storage waits from the game loop. The low-latency profile's final `/lag` window is
not the maximum over the entire 20-second capture. No physical S3/P4 was attached.

## Reproduction and evidence

```sh
make -C host -j4 test server
# Copy the pre-change executable before rebuilding the changed sources:
# cp host/build/mcserver build/storage-io/mcserver-before
SERVER_BIN="$PWD/build/storage-io/mcserver-before" node test/storage_perf.js build/storage-io/before.json 10 30
node test/storage_perf.js build/storage-io/after.json 10 30
node test/storage_io.js

# With ESP-IDF and esp-emu activated:
node test/emulator_profile.js --board esp32s3-8 --scenario two-workers \
  --seconds 20 --bots 6 --out build/profiles/s3-storage-new
```

Local raw artifacts (ignored by Git): `build/storage-io/` contains JSON probe samples
and serial logs; `build/profiles/s3-runtime-20261007/` and
`build/profiles/s3-storage-io/` contain firmware hashes, counter snapshots, logs and
traces. `docs/measurements/storage-io.json` preserves the compact measurements for
review without checking in large traces or binaries.

Validation covers bounded FIFO/ownership/shutdown, edits during an in-flight save,
failed flush acknowledgement and retry, decode fallback, read-only load failure,
stalled NBD with responsive clients, disconnected login-slot reuse, NBD reconnect,
and persisted edits after restart. The fault test is included in `test/run_all.js`.
