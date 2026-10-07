#!/usr/bin/env bash
# Build the ESP32-S3 firmware and run it in Espressif's QEMU.
#
#   tools/qemu/run.sh                 build, start an NBD world server, boot (Ctrl-A X quits)
#   tools/qemu/run.sh --gdb           halt at reset and wait for GDB on :1234 (see docs/QEMU.md)
#   tools/qemu/run.sh --gdb-port 3333 GDB port (implies --gdb)
#   tools/qemu/run.sh --icount 2      instruction-counted CPU: 2^2 ns per instruction (250 MIPS, close
#                                     to a 240 MHz LX7); timing becomes deterministic, but both cores
#                                     then share one virtual clock (no parallel speed-up)
#   tools/qemu/run.sh --mttcg         one host thread per emulated core (real parallelism; timing
#                                     follows the host CPU, so only ratios are meaningful)
#   tools/qemu/run.sh --bench         run the chunk pipeline benchmark (default --icount 2) and exit
#   tools/qemu/run.sh --no-nbd        run without world storage
#   tools/qemu/run.sh --world F.img   world image (default tools/qemu/world.img, created sparse)
#   tools/qemu/run.sh --nbd-port 10810  host port of the NBD server run.sh starts (default 10809)
#   tools/qemu/run.sh --port 25566    host port forwarded to the server (default 25565)
#   tools/qemu/run.sh --no-build      reuse the last firmware build
#   tools/qemu/run.sh --headless      no interactive console; serial log to stdout
#   tools/qemu/run.sh --env NAME      build and boot environment NAME; with --no-build, any
#                                     .pio/build/NAME, e.g. a saved copy of an older build
#
# Environment: PIO (pio executable), QEMU_DIR (QEMU install, downloaded if missing)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/tools/qemu"
QEMU_VERSION="esp_develop_9.2.2_20250817"
QEMU_TAG="esp-develop-9.2.2-20250817"
GDB_VERSION="14.2_20240403"
QEMU_DIR="${QEMU_DIR:-$HERE/.qemu}"
PIO="${PIO:-pio}"
PORT=25565
NBD_PORT=10809
WORLD="$HERE/world.img"
GDB=0
GDB_PORT=1234
ICOUNT=""
NBD=1
DO_BUILD=1
HEADLESS=0
ENV_NAME=esp32s3-qemu
BENCH=0
MTTCG=0
while [ $# -gt 0 ]; do
  case "$1" in
    --gdb) GDB=1 ;;
    --gdb-port) GDB=1; GDB_PORT="$2"; shift ;;
    --icount) ICOUNT="$2"; shift ;;
    --no-nbd) NBD=0 ;;
    --port) PORT="$2"; shift ;;
    --nbd-port) NBD_PORT="$2"; shift ;;
    --world) WORLD="$(realpath -m "$2")"; shift ;;
    --no-build) DO_BUILD=0 ;;
    --headless) HEADLESS=1 ;;
    --env) ENV_NAME="$2"; shift ;;
    --bench) BENCH=1 ;;
    --mttcg) MTTCG=1 ;;
    -h|--help) sed -n '2,22p' "$0"; exit 0 ;;
    *) echo "unknown option $1"; exit 2 ;;
  esac
  shift
done

if [ "$BENCH" = 1 ]; then
  ENV_NAME=esp32s3-qemu-bench
  NBD=0
  HEADLESS=1
  [ -z "$ICOUNT" ] && [ "$MTTCG" = 0 ] && ICOUNT=2
fi
if [ -n "$ICOUNT" ] && [ "$MTTCG" = 1 ]; then
  echo "--icount and --mttcg exclude each other (QEMU has no multi-threaded TCG with icount)"
  exit 2
fi
BUILD="$ROOT/.pio/build/$ENV_NAME"

# 1) QEMU
QEMU="$QEMU_DIR/qemu/bin/qemu-system-xtensa"
if [ ! -x "$QEMU" ]; then
  echo "downloading Espressif QEMU ($QEMU_VERSION)"
  mkdir -p "$QEMU_DIR"
  curl -sSL -o "$QEMU_DIR/qemu.tar.xz" \
    "https://dl.espressif.com/github_assets/espressif/qemu/releases/download/$QEMU_TAG/qemu-xtensa-softmmu-$QEMU_VERSION-x86_64-linux-gnu.tar.xz"
  tar -C "$QEMU_DIR" -xf "$QEMU_DIR/qemu.tar.xz"
  rm "$QEMU_DIR/qemu.tar.xz"
fi
if ! "$QEMU" --version >/dev/null 2>&1; then
  echo "QEMU needs libslirp and libgcrypt: sudo apt-get install libslirp0 libgcrypt20 libsdl2-2.0-0"
  exit 1
fi

# 2) firmware + flash image
if [ "$DO_BUILD" = 1 ]; then
  (cd "$ROOT" && "$PIO" run -e "$ENV_NAME")
fi
PIO_CORE="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}"
ESPTOOL="$PIO_CORE/packages/tool-esptoolpy/esptool.py"
BOOT_APP0="$PIO_CORE/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"
PY="$(command -v python3)"
[ -x "$(dirname "$(command -v "$PIO")")/python" ] && PY="$(dirname "$(command -v "$PIO")")/python"
"$PY" "$ESPTOOL" --chip esp32s3 merge_bin -o "$BUILD/qemu_flash.bin" --fill-flash-size 8MB \
  0x0 "$BUILD/bootloader.bin" 0x8000 "$BUILD/partitions.bin" 0xe000 "$BOOT_APP0" 0x10000 "$BUILD/firmware.bin" >/dev/null
echo "flash image: $BUILD/qemu_flash.bin"

# 3) world storage on the host
NBD_PID=""
cleanup() { [ -n "$NBD_PID" ] && kill "$NBD_PID" 2>/dev/null || true; }
trap cleanup EXIT
if [ "$NBD" = 1 ]; then
  python3 "$ROOT/tools/nbd_server.py" --file "$WORLD" --size 512M --port "$NBD_PORT" --bind 127.0.0.1 \
    > "${WORLD%.img}.nbd.log" 2>&1 &
  NBD_PID=$!
  echo "NBD world server on 127.0.0.1:$NBD_PORT ($WORLD, log ${WORLD%.img}.nbd.log)"
fi

# 4) boot
# hostfwd: host port -> server; guestfwd: the firmware's NBD_HOST (10.0.2.100:10809) -> host NBD server
ARGS=(-nographic -M esp32s3 -drive "file=$BUILD/qemu_flash.bin,if=mtd,format=raw")
[ "$BENCH" = 0 ] && ARGS+=(-nic "user,model=open_eth,hostfwd=tcp::$PORT-:25565,guestfwd=tcp:10.0.2.100:10809-cmd:python3 $HERE/relay.py 127.0.0.1 $NBD_PORT")
[ -n "$ICOUNT" ] && ARGS+=(-icount "shift=$ICOUNT,align=off,sleep=on")
[ "$MTTCG" = 1 ] && ARGS+=(-accel tcg,thread=multi)
ARGS+=(-m 8M)   # 8 MB quad SPI PSRAM (the minimum supported configuration)
if [ "$GDB" = 1 ]; then
  ARGS+=(-gdb "tcp::$GDB_PORT" -S)
  # PlatformIO's gdb needs libpython2.7, which current distributions lack: fall back
  # to Espressif's standalone gdb (python optional)
  GDBBIN="$PIO_CORE/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-gdb"
  if ! "$GDBBIN" --version >/dev/null 2>&1; then
    GDBBIN="$QEMU_DIR/xtensa-esp-elf-gdb/bin/xtensa-esp32s3-elf-gdb"
    if [ ! -x "$GDBBIN" ]; then
      echo "downloading Espressif gdb ($GDB_VERSION)"
      curl -sSL -o "$QEMU_DIR/gdb.tar.gz" \
        "https://github.com/espressif/binutils-gdb/releases/download/esp-gdb-v$GDB_VERSION/xtensa-esp-elf-gdb-$GDB_VERSION-x86_64-linux-gnu.tar.gz"
      tar -C "$QEMU_DIR" -xzf "$QEMU_DIR/gdb.tar.gz"
      rm "$QEMU_DIR/gdb.tar.gz"
    fi
  fi
  echo "QEMU is halted waiting for GDB. In another terminal:"
  echo "  $GDBBIN $BUILD/firmware.elf -ex 'target remote :$GDB_PORT' -ex 'b setup' -ex 'c'"
fi
if [ "$BENCH" = 1 ]; then
  # print the benchmark lines (and crashes) as they arrive; stop QEMU at the end marker
  LOG="$BUILD/bench.log"
  MODE="plain TCG"
  [ -n "$ICOUNT" ] && MODE="-icount shift=$ICOUNT"
  [ "$MTTCG" = 1 ] && MODE="MTTCG"
  echo "benchmark on the emulated ESP32-S3 ($MODE), log $LOG"
  "$QEMU" "${ARGS[@]}" < /dev/null > "$LOG" 2>&1 &
  QEMU_PID=$!
  trap 'kill "$QEMU_PID" 2>/dev/null; exit 143' TERM INT
  START=$SECONDS
  RC=1
  while kill -0 "$QEMU_PID" 2>/dev/null && [ $((SECONDS - START)) -lt "${BENCH_TIMEOUT:-1800}" ]; do
    if grep -aq "\[bench\] done" "$LOG"; then RC=0; break; fi
    if grep -aqE "Guru Meditation|assert failed|\[FATAL\]" "$LOG"; then break; fi
    sleep 1
  done
  kill "$QEMU_PID" 2>/dev/null; wait "$QEMU_PID" 2>/dev/null || true
  grep -aE "\[bench\]|Guru Meditation|assert failed|Backtrace|\[FATAL\]" "$LOG" || true
  [ "$RC" = 0 ] || echo "benchmark did not finish (see $LOG)"
  exit "$RC"
elif [ "$HEADLESS" = 1 ]; then
  # -nographic already routes the serial port to stdout. Run QEMU as a child so that
  # SIGTERM/SIGINT (e.g. from a test harness) stops it and the NBD server together.
  "$QEMU" "${ARGS[@]}" < /dev/null &
  QEMU_PID=$!
  trap 'kill "$QEMU_PID" 2>/dev/null; wait "$QEMU_PID" 2>/dev/null; exit 143' TERM INT
  wait "$QEMU_PID"
else
  ARGS+=(-serial mon:stdio)
  echo "Minecraft server: localhost:$PORT   (QEMU console: Ctrl-A X to quit, Ctrl-A C for the monitor)"
  "$QEMU" "${ARGS[@]}"
fi
