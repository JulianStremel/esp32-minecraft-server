# ESP-IDF 5 / esp-emulator migration validation

The firmware now builds directly with ESP-IDF **v5.5.5**, using the same portable
C++17 core as the native server. Espressif **esp-emu 0.48.0** replaces the previous
QEMU runner. PlatformIO, Arduino entry points, WROVER profiles, the custom
OpenCores Ethernet driver and the QEMU NBD relay have been removed. QEMU remains available only for
[README GIF capture](CAPTURE.md), using the current IDF firmware and SDK Ethernet driver.

The supported profiles are `esp32s3-8`, `esp32s3-16`, `esp32p4-8` and
`esp32p4-16`. The suffix is PSRAM capacity in MB; flash is configured separately
and defaults to 8 MB. S3 uses station WiFi. Physical P4 uses configurable RMII
Ethernet with a LAN8720 PHY; the emulator uses its generic PHY. P4 defaults to
silicon revision 3.0 and newer. See [EMULATOR.md](EMULATOR.md) for commands,
network configuration, debugging and recording.

## Checks performed

Validation used the repository at `1005fdd4c4ae1ea41702bf45c60c702a48dfd8f9`
as the pre-migration baseline.

| Check | Result |
|---|---|
| Hardware compile/link, all four profiles | Passed |
| Emulator compile/link and IDF flash-image merge, all four profiles | Passed |
| Firmware protocol/login, terrain, diagnostics and NBD cold-restart persistence, all four profiles | Passed |
| Native unit tests | 86 tests, 16,934 checks, zero failures |
| Native AddressSanitizer + UndefinedBehaviorSanitizer tests | 86 tests, 16,934 checks, zero failures |
| Native bot initialization, smoke, gameplay and persistence | Passed; gameplay 16/16 |
| Native mobs stress suite | 6/6 checks passed after the test-client startup correction below |
| All five end-to-end suites against the ASan/UBSan server | Passed; gameplay 16/16 and mobs/stress 6/6 |
| Repeated eight-client joins | All 48 joins across six rounds passed |
| Delayed bot initialization regression | Reproduced the old failure; passes with the corrected transport startup and rejection cleanup |
| S3 and P4 device benchmarks | Completed; all six paired block/float generator fingerprints match, storage round trips pass |
| S3 and P4 GDB | Remote breakpoint at `app_main` and backtrace verified |
| FreeRTOS traces | Minecraft task execution slices with positive duration verified |
| S3 six-client exploration with workers 0 and 2 | Passed; chunk delivery, connected clients, serial metrics and command instrumentation verified |
| P4 six-client exploration with workers 0 and 2 | Passed; chunk delivery, connected clients, serial metrics and command instrumentation verified |
| Browser GIF recording | Current S3 IDF firmware recorded through a separate QEMU-only capture profile; native and esp-emulator recording also exercised |
| Cloud setup | Complete install/build script repeated successfully |

The firmware checks exercise `/tps`, `/lag`, `/workers`, `/storage`, serial
`[stat]`, select waits and eventfd wakeups. ESP-IDF timer profiling is enabled.
The emulator's task tracing uses ELF/FreeRTOS symbols rather than new gameplay
hooks. Emulator clients have bounded host-time deadlines; load phases wait for
a real device status window before collecting samples.

## Stress-test correction and remaining limits

The initial baseline and migration runs intermittently timed out while joining
eight players, followed by the dependent chat failure. Investigation captured
Join Game arriving before Mineflayer installed its login listeners. Later light
packets accessed an uninitialized `bot.world`; the protocol dependency caught
that downstream exception and misleadingly reported an inflation error. The
captured compressed packet itself was valid.

The shared test helper now uses Mineflayer's transport hook to open the TCP
connection after `inject_allowed`. Failed connections are closed and their
spawn timer/listeners are cleared. `bot_connect.js` deliberately delays plugin
initialization, verifies login and terrain, and checks rejected-client cleanup.
It reproduced the old spawn timeout and passes with the fix. All five end-to-end
suites now pass against both the normal and ASan/UBSan servers, including all
six mobs/stress checks. S3 and P4 firmware smoke checks were also rerun with the
corrected helper and passed diagnostics, task traces and cold-restart persistence. The original
eight-player load, chat and TPS assertions and 20-second spawn timeout remain.
No firmware/core workaround or dependency patch was needed. CI also runs the
end-to-end suites against the sanitizer server.

Short load phases can leave the optional 3x3 terrain probes incomplete; the
harness reports that count. These runs verify the load harness and diagnostics,
not terrain delivery latency guarantees or physical board performance. Timing
from the former QEMU runner is not comparable to esp-emulator.

In the P4 six-client run, the two-worker phase reported about 7 TPS and storage
read stalls reaching 5 seconds, versus 20 TPS in the inline phase. All clients
remained connected and chunks continued arriving. This is an observed emulator
load limitation; its cause and applicability to physical P4 hardware have not
been established. This measurement preceded the dedicated storage-thread follow-up; see
[STORAGE_IO.md](STORAGE_IO.md) for its scope and results.

No physical S3/P4 board was attached, so flash/boot on real boards and LAN8720
wiring/clock behavior remain unverified. The new GitHub workflow builds the
four profiles and runs firmware smoke/benchmarks, but has not been executed on
GitHub here.

The initial migration preserved core behavior and capacity limits; its core edits
were logging casts required by the ESP-IDF toolchain's integer typedefs and updated
build/tool references. The later storage-thread work is documented separately. Existing bounded-string truncation warnings
remain warnings; game formatting was not changed as part of this migration.
