# Running, debugging and load-testing the firmware in QEMU

The real firmware (not the PC build) runs on a virtual ESP32-S3 in
[Espressif's QEMU](https://github.com/espressif/qemu) with 8 MB of PSRAM. There is
no WiFi in the emulator, so the `esp32s3-qemu` build drives the emulated OpenCores
Ethernet MAC (`src/qemu_eth.c`) and gets an address from QEMU's user-mode network.
Everything else (server task, worker threads, NBD storage, PSRAM) is the same
code that runs on a board.

`tools/qemu/run.sh` does all of it:

1. downloads QEMU (and, for `--gdb`, Espressif's gdb) into `tools/qemu/.qemu`
   (`QEMU_DIR` overrides the location)
2. builds the firmware with PlatformIO (`PIO=/path/to/pio` if `pio` is not on `PATH`)
3. merges bootloader, partition table and app into an 8 MB flash image
4. starts `tools/nbd_server.py` with a sparse world image
5. boots QEMU and forwards the ports

```
tools/qemu/run.sh                     # build + boot; Minecraft on localhost:25565
tools/qemu/run.sh --port 25570        # another host port
tools/qemu/run.sh --world /tmp/w.img  # another world image (created sparse, 512 MiB)
tools/qemu/run.sh --no-nbd            # no storage: the world is not saved
tools/qemu/run.sh --no-build          # reuse the last build
tools/qemu/run.sh --headless          # no interactive console (scripts, CI)
```

The serial console prints the boot log and a `[stat]` line every 10 s. In the
interactive console, `Ctrl-A X` quits QEMU and `Ctrl-A C` opens the QEMU monitor.

Requirements: `libslirp0`, `libgcrypt20` and `libsdl2-2.0-0` for QEMU, Python 3 for
the NBD server, and Node.js for the load test.

## Networking

QEMU's user-mode network puts the firmware at 10.0.2.15 behind a NAT:

- `hostfwd` forwards the host port (`--port`, default 25565) to the server.
- `guestfwd` forwards 10.0.2.100:10809, the `NBD_HOST` in `include/config_qemu.h`,
  to the NBD server that `run.sh` starts on the host (any port, see `--nbd-port`).
  `tools/qemu/relay.py` connects each guest connection to it, so the firmware can
  reconnect like it would over WiFi.

## Debugging with gdb

```
tools/qemu/run.sh --gdb               # halts at reset, gdb server on :1234
tools/qemu/run.sh --gdb-port 3333     # another port
```

`run.sh` prints the gdb command line. The gdb in PlatformIO's toolchain needs
`libpython2.7`, which current distributions no longer ship, so `run.sh` falls back
to Espressif's standalone gdb when that one does not start. The `esp32s3-qemu`
build has debug info (`-g`). Each CPU core shows up as a gdb thread:

```
(gdb) target remote :1234
(gdb) break mc::LoadJob::run          # chunk generation on a worker thread
(gdb) continue
Thread 1 hit Breakpoint 1, mc::LoadJob::run (this=0x3fcf5bfc) at lib/mcore/src/mc/server/chunk_jobs.cpp:22
(gdb) bt
#0  mc::LoadJob::run (...) at lib/mcore/src/mc/server/chunk_jobs.cpp:22
#1  mc::JobQueue::workerMain (...) at lib/mcore/src/mc/jobs.cpp:90
#2  mc::plat::(anonymous namespace)::threadTrampoline (...) at src/platform_esp32.cpp:169
(gdb) info threads
* 1    Thread 1.1 (CPU#0 [running]) mc::LoadJob::run (...)
  2    Thread 1.2 (CPU#1 [running]) ...
```

To decode a crash backtrace printed on the console:
`xtensa-esp32s3-elf-addr2line -pfiaC -e .pio/build/esp32s3-qemu/firmware.elf <addresses>`.

## Timing modes: what the emulator can and cannot tell

| mode | option | good for | caveat |
|---|---|---|---|
| plain TCG | (default) | functional testing | times follow the host CPU |
| instruction counting | `--icount N` | device-like CPU cost of single-threaded code: 2^N ns per instruction, `--icount 2` = 250 MIPS, close to a 240 MHz LX7 | both cores share one virtual clock, so parallel work shows no speed-up and cross-core lock waits look much longer than they are |
| multi-threaded TCG | `--mttcg` | parallelism: each core is a host thread | times follow the host CPU and its load, so compare runs, not absolute values |

Instruction counting ignores cache misses and PSRAM wait states. A real board is
somewhat slower than the `--icount 2` numbers.

## Benchmark: cost of the chunk pipeline on the device

```
tools/qemu/run.sh --bench             # --icount 2 unless --icount/--mttcg is given
tools/qemu/run.sh --bench --mttcg     # worker scaling with two real (emulated) cores
```

This builds `esp32s3-qemu-bench`. That firmware runs `mc::runChunkBench`
(`lib/mcore/src/mc/bench.cpp`) on core 1 instead of the server, prints the
results and exits. Results with `--icount 2`:

```
[bench] generate (normal terrain)             33.58 ms avg     64.98 ms max  (81)
[bench] light (sky + block, with neighbours)     8.52 ms avg     14.75 ms max  (49)
[bench] encode chunk packet                    1.01 ms avg      1.44 ms max  (49)
[bench] deflate chunk packet                   4.10 ms avg      6.71 ms max  (49)
[bench] encode light packet                    0.06 ms avg      0.10 ms max  (49)
[bench] deflate light packet                   3.80 ms avg      6.02 ms max  (49)
[bench] store: save (2x deflate + CRC)         9.77 ms avg     15.81 ms max  (49)
[bench] store: load (inflate + CRC + decode)    14.23 ms avg     22.90 ms max  (49)
[bench] => sending a resident chunk: 18.56 ms; a new chunk (generate + send): 52.14 ms
[bench] generate (flat)                        2.65 ms avg      2.66 ms max  (81)
```

A new chunk costs about one whole 50 ms tick of one core. That is why this work
runs on the worker threads (see the README). With `--mttcg` the same firmware
measures how the job queue scales:

```
[bench] generate chunk    x24, 0 workers (game loop):    984.1 ms  ( 24.4 jobs/s, x1.00)
[bench] generate chunk    x24, 1 worker             :    977.7 ms  ( 24.5 jobs/s, x1.01)
[bench] generate chunk    x24, 2 workers            :    488.8 ms  ( 49.1 jobs/s, x2.01)
[bench] ALU-only (ref.)   x24, 2 workers            :     21.9 ms  (1096.7 jobs/s, x1.89)
```

The ALU-only job touches no memory. It shows how much parallelism the emulator
offers at all (the scaling varies by roughly ±10% between runs).

## Load test with virtual players

```
cd test && npm install
node qemu_load.js                       # 6 players, workers 0 vs 2, MTTCG, 30 s each
node qemu_load.js --bots 8 --seconds 60 --workers 2,1,0 --no-build
node qemu_load.js --icount 2            # deterministic CPU timing (see the caveats above)
```

`qemu_load.js` boots the firmware headless with a fresh world and connects
mineflayer players. For each pool size it switches the worker pool at runtime
(`/workers N`), teleports the players into untouched terrain and moves them 24
blocks every 3 s, so every chunk they need has to be generated, lit, compressed
and sent by the device. It samples `/tps` and `/lag` every 2 s:

```
workers |  TPS (min)  | ms/tick | max tick | loop stall avg / max | chunks/s
      0 | 19.8 (18.6) |     3.4 |   130 ms |      244 /   374 ms   |     12.6
      2 | 20.0 (20.0) |     6.6 |    80 ms |       24 /    88 ms   |     17.8
   slowest loop seen: 88 ms: input 1 stream 79 entities 2 tracking 1 output 5
                      (stream: snapshots 0, storage reads 77) ...
```

*Loop stall* is the longest single `Server::loop()` call in a 2 s window: the time
in which nothing else, such as packet handling, keep-alives or other players'
movement, can happen. With the work on the game loop (`0` workers), generating
and compressing chunks stalls it for hundreds of milliseconds. With the workers
it only takes snapshots and applies results. What remains is storage I/O: the
pipelined NBD lookup for the chunks being loaded, which goes through QEMU's
emulated network (a few ms over real WiFi).

`/lag` (any player) breaks down the slowest loop iteration of the last 2 s.
`/workers` (operators) shows or changes the pool size. `/tps` shows TPS, tick
times and the worker utilisation.
