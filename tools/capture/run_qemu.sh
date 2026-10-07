#!/usr/bin/env bash
# QEMU is only a GIF capture backend. Tests, benchmarks and debugging use esp-emulator.
# Source ESP-IDF export.sh; install its optional qemu-xtensa tool first.
# --port PORT --nbd-port PORT --world FILE --no-build
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PORT=25640
NBD_PORT=10809
WORLD=""
DO_BUILD=1
while [ $# -gt 0 ]; do
  case "$1" in
    --port) PORT="${2:?}"; shift ;;
    --nbd-port) NBD_PORT="${2:?}"; shift ;;
    --world) WORLD="$(realpath -m "${2:?}")"; shift ;;
    --no-build) DO_BUILD=0 ;;
    -h|--help) sed -n '2,4p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done
for number in "$PORT" "$NBD_PORT"; do
  [[ "$number" =~ ^[0-9]+$ ]] && ((number > 0 && number < 65536)) || { echo "invalid port" >&2; exit 2; }
done
QEMU="${QEMU:-qemu-system-xtensa}"
command -v "$QEMU" >/dev/null || { echo "Install ESP-IDF's qemu-xtensa tool; see docs/CAPTURE.md" >&2; exit 1; }
BUILD="$ROOT/build/esp32s3-8-qemu-capture"
if [ "$DO_BUILD" = 1 ]; then
  : "${IDF_PATH:?Source ESP-IDF export.sh first}"
  export IDF_PY_BUILD_JOBS="${IDF_PY_BUILD_JOBS:-4}"
  cd "$ROOT"
  idf.py -B "$BUILD" -D IDF_TARGET=esp32s3 -D MC_BOARD=esp32s3-8 \
    -D MC_QEMU_CAPTURE=ON -D MC_EMULATOR=OFF -D MC_BENCH=OFF -D MC_CPU_PROFILE=OFF \
    -D "MC_NBD_PORT=$NBD_PORT" build merge-bin
fi
python3 - "$BUILD/CMakeCache.txt" "$NBD_PORT" <<'PY'
import sys
from pathlib import Path
cache = dict(line.split('=', 1) for line in Path(sys.argv[1]).read_text().splitlines() if '=' in line and not line.startswith(('#', '//')))
expected = {'MC_BOARD:STRING': 'esp32s3-8', 'MC_QEMU_CAPTURE:BOOL': 'ON',
            'MC_EMULATOR:BOOL': 'OFF', 'MC_BENCH:BOOL': 'OFF', 'MC_NBD_PORT:STRING': sys.argv[2]}
for key, value in expected.items():
    if cache.get(key) != value:
        sys.exit(f'Capture build mismatch ({key}); rerun without --no-build')
PY
TMP="$(mktemp -d)"
NBD_PID=""
QEMU_PID=""
cleanup() {
  if [ -n "$QEMU_PID" ]; then kill "$QEMU_PID" 2>/dev/null || true; wait "$QEMU_PID" 2>/dev/null || true; fi
  if [ -n "$NBD_PID" ]; then kill "$NBD_PID" 2>/dev/null || true; wait "$NBD_PID" 2>/dev/null || true; fi
  rm -rf "$TMP"
}
trap cleanup EXIT
trap 'exit 143' TERM
trap 'exit 130' INT
[ -n "$WORLD" ] || WORLD="$TMP/world.img"
mkdir -p "$(dirname "$WORLD")"
python3 "$ROOT/tools/nbd_server.py" --file "$WORLD" --size 512M --port "$NBD_PORT" --bind 127.0.0.1 \
  > "${WORLD}.nbd.log" 2>&1 &
NBD_PID=$!
python3 - "$NBD_PORT" "$NBD_PID" "${WORLD}.nbd.log" <<'PY'
import os, socket, sys, time
from pathlib import Path
for _ in range(100):
    os.kill(int(sys.argv[2]), 0)
    if f'nbd: listening on 127.0.0.1:{sys.argv[1]}' in Path(sys.argv[3]).read_text():
        try:
            with socket.create_connection(('127.0.0.1', int(sys.argv[1])), .2) as s:
                if s.recv(8) == b'NBDMAGIC':
                    break
        except OSError:
            pass
    time.sleep(.05)
else:
    sys.exit('NBD handshake failed')
PY
# Give QEMU a writable, power-of-two flash copy. Never modify a build artifact.
python3 - "$BUILD/merged-binary.bin" "$TMP/flash.bin" <<'PY'
import sys
from pathlib import Path
image = Path(sys.argv[1]).read_bytes()
if len(image) > 8 * 1024 * 1024:
    sys.exit('Capture image exceeds 8 MB flash')
Path(sys.argv[2]).write_bytes(image.ljust(8 * 1024 * 1024, b'\xff'))
PY
"$QEMU" -nographic -M esp32s3 -m 8M -accel tcg,thread=multi \
  -drive "file=$TMP/flash.bin,if=mtd,format=raw" \
  -nic "user,model=open_eth,hostfwd=tcp:127.0.0.1:$PORT-:25565" < /dev/null &
QEMU_PID=$!
wait "$QEMU_PID"
QEMU_PID=""
