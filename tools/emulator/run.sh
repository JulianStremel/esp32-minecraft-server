#!/usr/bin/env bash
# Run ESP-IDF firmware in esp-emulator (esp-emu 0.48.0).
# --board esp32s3-8|esp32s3-16|esp32p4-8|esp32p4-16 (default esp32s3-8)
# --port PORT --nbd-port PORT --world FILE --no-build --bench --cpu-profile --trace FILE
# --gdb --gdb-port PORT --headless --no-nbd
# Environment: ESP_EMU executable; source ESP-IDF export.sh for builds.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BOARD=esp32s3-8
PORT=25565
NBD_PORT=10809
WORLD="$ROOT/tools/emulator/world.img"
DO_BUILD=1
BENCH=0
CPU_PROFILE=0
NBD=1
GDB=0
GDB_PORT=1234
TRACE=""
while [ $# -gt 0 ]; do
  case "$1" in
    --board) BOARD="${2:?}"; shift ;;
    --port) PORT="${2:?}"; shift ;;
    --nbd-port) NBD_PORT="${2:?}"; shift ;;
    --world) WORLD="$(realpath -m "${2:?}")"; shift ;;
    --no-build) DO_BUILD=0 ;;
    --bench) BENCH=1; NBD=0 ;;
    --cpu-profile) CPU_PROFILE=1 ;;
    --no-nbd) NBD=0 ;;
    --trace) TRACE="$(realpath -m "${2:?}")"; shift ;;
    --gdb) GDB=1 ;;
    --gdb-port) GDB=1; GDB_PORT="${2:?}"; shift ;;
    --headless) ;; # UART is always on stdout; accepted for test harnesses.
    -h|--help) sed -n '2,6p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done
case "$BOARD" in esp32s3-8|esp32s3-16|esp32p4-8|esp32p4-16) ;; *) echo "unsupported board: $BOARD" >&2; exit 2 ;; esac
for number in "$PORT" "$NBD_PORT" "$GDB_PORT"; do
  [[ "$number" =~ ^[0-9]+$ ]] && ((number > 0 && number < 65536)) || { echo "invalid port" >&2; exit 2; }
done
ESP_EMU="${ESP_EMU:-esp-emu}"
command -v "$ESP_EMU" >/dev/null || { echo "Install esp-emu 0.48.0; see docs/EMULATOR.md" >&2; exit 1; }
BUILD="$ROOT/build/$BOARD-emulator"
BUILD_ARGS=(--board "$BOARD" --emulator --nbd-port "$NBD_PORT")
if [ "$BENCH" = 1 ]; then BUILD+="-bench"; BUILD_ARGS+=(--bench); fi
if [ "$CPU_PROFILE" = 1 ]; then BUILD+="-cpu-profile"; BUILD_ARGS+=(--cpu-profile); fi
if [ "$DO_BUILD" = 1 ]; then
  "$ROOT/tools/idf/build.sh" "${BUILD_ARGS[@]}" build merge-bin
fi
test -f "$BUILD/merged-binary.bin" || { echo "Missing merged image: build first" >&2; exit 1; }
# --no-build must not silently use a benchmark, hardware or differently configured image.
python3 - "$BUILD/CMakeCache.txt" "$BOARD" "$NBD_PORT" "$BENCH" "$CPU_PROFILE" <<'PY'
import sys
from pathlib import Path
cache = dict(line.split('=', 1) for line in Path(sys.argv[1]).read_text().splitlines() if '=' in line and not line.startswith(('#', '//')))
expected = {'MC_BOARD:STRING': sys.argv[2], 'MC_EMULATOR:BOOL': 'ON', 'MC_NBD_PORT:STRING': sys.argv[3], 'MC_BENCH:BOOL': 'ON' if sys.argv[4] == '1' else 'OFF'}
for key, value in expected.items():
    if cache.get(key) != value:
        sys.exit(f'Build configuration mismatch ({key}); rerun without --no-build')
if cache.get('MC_CPU_PROFILE:BOOL', 'OFF') != ('ON' if sys.argv[5] == '1' else 'OFF'):
    sys.exit('CPU profile build mismatch; rerun without --no-build')
PY
ARGS=(--chip "${BOARD%-*}" --psram-size "${BOARD##*-}M" --firmware "$BUILD/merged-binary.bin"
      --elf "$BUILD/mcserver.elf" --log-color never)
ARGS+=(--net "user,hostfwd=tcp:127.0.0.1:$PORT-:25565")
if [[ "$BOARD" == esp32s3-* ]]; then ARGS+=(--wifi-ssid myssid --wifi-password mypassword --wifi-auth wpa2-psk); fi
[ -z "$TRACE" ] || ARGS+=(--trace "$TRACE")
if [ "$GDB" = 1 ]; then
  ARGS+=(--gdb "$GDB_PORT" --gdb-halt)
  echo "GDB: target remote :$GDB_PORT (ELF $BUILD/mcserver.elf)"
fi
NBD_PID=""
EMU_PID=""
cleanup() {
  if [ -n "$EMU_PID" ]; then kill "$EMU_PID" 2>/dev/null || true; wait "$EMU_PID" 2>/dev/null || true; fi
  if [ -n "$NBD_PID" ]; then kill "$NBD_PID" 2>/dev/null || true; wait "$NBD_PID" 2>/dev/null || true; fi
}
trap cleanup EXIT
trap 'exit 143' TERM
trap 'exit 130' INT
if [ "$NBD" = 1 ]; then
  mkdir -p "$(dirname "$WORLD")"
  python3 "$ROOT/tools/nbd_server.py" --file "$WORLD" --size 512M --port "$NBD_PORT" --bind 127.0.0.1 \
    > "${WORLD}.nbd.log" 2>&1 &
  NBD_PID=$!
  # Verify the NBD handshake, rather than treating a PID or a port as readiness.
  python3 - "$NBD_PORT" "$NBD_PID" "${WORLD}.nbd.log" <<'PY'
import os, socket, sys, time
from pathlib import Path
for _ in range(100):
    os.kill(int(sys.argv[2]), 0)
    if f'nbd: listening on 127.0.0.1:{sys.argv[1]}' not in Path(sys.argv[3]).read_text():
        time.sleep(.05)
        continue
    try:
        with socket.create_connection(('127.0.0.1', int(sys.argv[1])), .2) as s:
            if s.recv(8) == b'NBDMAGIC':
                break
    except OSError:
        time.sleep(.05)
else:
    sys.exit('NBD handshake failed')
PY
elif [ "$BENCH" = 0 ]; then
  echo "--no-nbd requires an external NBD server on host loopback:$NBD_PORT" >&2
fi
if [ "$BENCH" = 1 ]; then
  LOG="$BUILD/bench.log"
  set +e
  "$ESP_EMU" "${ARGS[@]}" --timeout "${BENCH_TIMEOUT:-1800}s" --exit-on '[bench] done' > "$LOG" 2>&1 &
  EMU_PID=$!
  wait "$EMU_PID"
  RC=$?
  EMU_PID=""
  set -e
  cat "$LOG"
  [ "$RC" = 0 ] && grep -aq '\[bench\] done' "$LOG" && ! grep -aqE 'generator check FAILED|store round trip failed|Guru Meditation|assert failed|\[FATAL\]|Firmware abort|CPU fault' "$LOG"
else
  "$ESP_EMU" "${ARGS[@]}" < /dev/null &
  EMU_PID=$!
  wait "$EMU_PID"
  EMU_PID=""
fi
