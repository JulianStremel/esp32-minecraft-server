# GIF capture in QEMU

The README's GIFs are now recorded on the real board (see [GIFS.md](GIFS.md)). This
QEMU profile stays for recordings without hardware. Firmware tests, benchmarks, GDB and
execution traces use [esp-emulator](EMULATOR.md). The recording runs the current
ESP-IDF 5.5.5 server and its real network protocol, viewed through Mineflayer and
prismarine-viewer in headless Chromium.

Source ESP-IDF's `export.sh`, then install its optional, checksum-verified tool:

```sh
python "$IDF_PATH/tools/idf_tools.py" install qemu-xtensa
. "$IDF_PATH/export.sh"
cd test
npm ci --ignore-scripts
npx playwright install chromium
cd ..
node test/record_gif.js --server qemu --seconds 18 --fps 10
```

The tested QEMU release is `esp_develop_9.2.2_20260417`, selected by IDF 5.5.5's
`tools.json`. Its host shared libraries (including `libslirp0`) and ffmpeg must
also be installed. `QEMU` may override the executable path.

The recorder defaults to `--server qemu`. It builds into
`build/esp32s3-8-qemu-capture`, starts a fresh NBD world, boots an S3 with 8 MB
PSRAM, and stops both processes after capture. Subsequent recordings can use
`--no-build`. Use `--keep-frames --debug` to retain source frames and log player
positions. `--out /tmp/example.gif` keeps trial recordings out of the README.

This isolated profile uses IDF's built-in OpenCores Ethernet driver, quad PSRAM,
and QEMU's multi-threaded TCG acceleration. It has no benchmark or debugger
options. It uses the same server sources and seed as the esp-emulator profile;
the hardware and esp-emulator builds retain their existing memory/network setup.
No custom Ethernet driver, NBD relay, Arduino or PlatformIO build is needed.

After adding capture, S3/P4 esp-emulator images were rebuilt with
`MC_QEMU_CAPTURE=OFF`; S3 retained octal PSRAM with OpenCores disabled. The S3
esp-emulator smoke check passed login, terrain, diagnostics, task traces and NBD
cold-restart persistence. CI continues to use esp-emulator exclusively.

Capture waits for terrain at the filming location and visible players, then
settles their opening positions before recording. The default walking speed is
3.5 blocks/second. To reduce browser CPU contention, the README recording uses
420×237 capture/output, a camera view distance of three chunks and 15 seconds of
settling time (`--width 420 --height 237 --view 3 --lead 15000`, now the defaults).
The verified run rendered 251 source frames over the 18-second capture window
(about 14 FPS), encoded to a 17.1-second, 171-frame GIF at 10 FPS. Startup and
occasional compositor stalls still depend on host load.

Frame timestamps follow Chromium's compositor; this is a
visual demonstration, not a device performance measurement. `--server emulator
--board esp32s3-8` and `--server host` remain available for explicit comparisons.
