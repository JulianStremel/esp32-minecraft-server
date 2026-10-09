# ESP-IDF firmware in esp-emulator

The firmware uses **ESP-IDF v5.5.5** and runs on both ESP32-S3 (Xtensa LX7)
and ESP32-P4 (RISC-V). The emulator is
[Espressif esp-emulator](https://github.com/espressif/esp-emulator), pinned to
**esp-emu 0.48.0**. Arduino, PlatformIO and the custom QEMU OpenCores driver
are no longer part of the firmware build.

## Setup

Install ESP-IDF v5.5.5 using its official `install.sh esp32s3,esp32p4`, then
source `export.sh`. Install esp-emulator using its official installer, which
verifies the release archive against its published SHA256SUMS:

```sh
git clone --branch v5.5.5 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32s3,esp32p4 && . ./export.sh
# In a separate directory:
git clone https://github.com/espressif/esp-emulator.git
sh esp-emulator/install.sh --version 0.48.0
```

From the Minecraft repository:

```sh
tools/emulator/run.sh                              # S3, 8 MB PSRAM
tools/emulator/run.sh --board esp32p4-8
tools/emulator/run.sh --no-build                    # reuse a matching merged image
tools/emulator/run.sh --port 25570 --nbd-port 10810 --world /tmp/world.img
```

`ESP_EMU=/path/to/esp-emu` overrides the emulator executable. UART output goes
to stdout; Ctrl-C stops the emulator and its NBD server. `--headless` uses the
same stream for automated tests. Build outputs and sdkconfig are isolated in
`build/<board>-emulator`; hardware and benchmark outputs use separate directories.
`idf.py merge-bin` supplies the bootloader, partition table and application at
the offsets of the selected chip. There is no hand-maintained flash merge list.

## Memory and networking

The suffix `-8` denotes the minimum **PSRAM**, not flash. Both require at least 8 MB
flash. S3 uses octal PSRAM; P4 uses its native external RAM controller. Firmware
checks the actual detected RAM against the selected profile and keeps the existing
capacity limits; the migration does not increase player/entity/view limits.

S3 connects through the normal ESP-IDF WiFi station driver, with the emulator's
public test AP (`myssid`/`mypassword`). P4 uses the native ESP-IDF EMAC driver and
the emulator's generic PHY. Physical P4 builds instead use LAN8720; configure
its PHY address, reset GPIO, RMII pins and input/output clock in
`tools/idf/build.sh --board esp32p4-8 menuconfig`. P4 defaults target silicon
revision 3.0 and newer, matching the embedded emulator ROM. Select the appropriate
ESP-IDF revision configuration for older physical P4 silicon.

The runner forces user-mode networking and binds Minecraft's forwarded port to
host loopback. The guest gateway `192.168.4.1` reaches host loopback, so the
firmware connects directly to the runner's Python NBD server. The NBD port is
compiled into the emulator profile; `--no-build` verifies it matches the cached
build. Different NBD ports need a rebuild. `--no-nbd` leaves an **external** NBD
server responsible for that port; it does not disable persistence in the image.

## Tests

```sh
make -C host -j4 test server
make -C host SAN=1 -j4 test server
cd test && npm ci --ignore-scripts && node run_all.js
# Back at the repository root:
node test/emulator_smoke.js --board esp32s3-8
node test/emulator_smoke.js --board esp32p4-8
node test/emulator_load.js --board esp32s3-8 --bots 6 --seconds 30 --workers 0,2
tools/emulator/run.sh --board esp32s3-8 --bench
tools/emulator/run.sh --board esp32p4-8 --bench
```

The firmware smoke check logs in a real protocol client, receives generated terrain,
checks `/tps`, `/lag`, `/workers`, `/storage` and `[stat]`, changes and saves a block,
then cold-boots the same NBD world and verifies the block. It also records a task
trace and checks the Minecraft task appears in it. The native gameplay, mobs,
persistence, packet fuzzing, jobs and timer tests retain their existing coverage.
The load harness waits for a device measurement window before each phase. Emulator
clients use a bounded 10-minute host-time keepalive deadline, since firmware
keepalives and statistics follow simulated time.

The device benchmark checks the integer/block and floating-point generator
fingerprints for both generator versions. The runner requires `[bench] done`,
propagates emulator failure and rejects generator mismatches or firmware panics.
Serial logs and benchmark results belong to the current run, not a stored binary.

## Instrumentation and debugging

```sh
tools/emulator/run.sh --board esp32p4-8 --trace /tmp/minecraft-trace.json
tools/emulator/run.sh --board esp32s3-8 --gdb-port 3333
# In another terminal, after sourcing ESP-IDF:
xtensa-esp32s3-elf-gdb build/esp32s3-8-emulator/mcserver.elf \
  -ex 'target remote :3333' -ex 'break app_main' -ex continue
# P4 uses riscv32-esp-elf-gdb instead.
```

`--trace` uses the ELF's FreeRTOS symbols and writes Chrome Trace Event data for
Perfetto, including task slices and sampled PCs. It requires no guest trace hooks.
`--gdb`/`--gdb-port` halts at reset until a debugger connects. Use the selected ELF
for backtraces; ESP-IDF `monitor` decodes hardware panics.

The existing `/tps`, `/lag`, `/workers` and serial `[stat]` formats are retained.
ESP-IDF timer profiling is enabled. Emulator/host timing is useful for comparing
runs, but it is not a claim about physical board performance. QEMU `--icount`,
`--mttcg`, machine names and the old timing measurements do not apply here.

For per-core CPU/idle measurements, use the separate runtime-statistics build:

```sh
node test/emulator_profile.js --board esp32s3-8 --seconds 20 --bots 6 \
  --out build/profiles/s3-new-run
# Reuse the profiling image on subsequent runs:
node test/emulator_profile.js --no-build --out build/profiles/s3-another-run
```

This runs an empty server and six exploring clients with zero/two workers, each
with a fresh world. Both workload pacing and CPU counter windows use firmware
`esp_timer` time. The report includes host-process CPU separately. Raw counters,
serial logs, traces and `summary.json` are saved under the chosen output directory;
use a fresh directory per run. See [S3_CPU_PROFILE.md](S3_CPU_PROFILE.md) for the
recorded results and measurement limits.

`tools/idf/build.sh --board esp32s3-8 --emulator --cpu-profile build merge-bin`
builds the optional instrumentation into `build/esp32s3-8-emulator-cpu-profile`.
`tools/emulator/run.sh --cpu-profile --no-build` runs that image. Hardware builds
can also enable `--cpu-profile` to print the same `[cpu]` runtime-counter records.
Normal firmware builds leave this sampling task and FreeRTOS runtime accounting off.

The README GIF uses a separate QEMU capture profile to leave more host CPU
available for browser rendering. QEMU is only used for recording; tests,
benchmarks and instrumentation continue to use esp-emulator. See
[CAPTURE.md](CAPTURE.md) for installation and reproduction commands.
