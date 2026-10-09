#!/usr/bin/env bash
# Native ESP-IDF build wrapper. Source ESP-IDF's export.sh first.
# tools/idf/build.sh [--board esp32s3-8|esp32p4-8]
#                    [--emulator] [--bench] [--cpu-profile] [--dashboard] [--release] [--nbd-port PORT]
#                    [idf.py actions/options]
# --dashboard: include the HTTP status dashboard (MC_DASHBOARD_PORT in config.h, default 80)
# --release: the prebuilt firmware (tools/release/config.h instead of include/config.h,
#            with the dashboard), in build/<board>-release
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BOARD=esp32s3-8
EMU=OFF
BENCH=OFF
CPU_PROFILE=OFF
DASHBOARD=OFF
RELEASE=OFF
NBD_PORT=10809
while [ $# -gt 0 ]; do
  case "$1" in
    --board) BOARD="${2:?missing board}"; shift 2 ;;
    --emulator) EMU=ON; shift ;;
    --bench) BENCH=ON; shift ;;
    --cpu-profile) CPU_PROFILE=ON; shift ;;
    --dashboard) DASHBOARD=ON; shift ;;
    --release) RELEASE=ON; DASHBOARD=ON; shift ;;
    --nbd-port) NBD_PORT="${2:?missing port}"; shift 2 ;;
    -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
    *) break ;;
  esac
done
case "$BOARD" in esp32s3-8|esp32p4-8) ;; *) echo "unsupported board: $BOARD" >&2; exit 2 ;; esac
[[ "$NBD_PORT" =~ ^[0-9]+$ ]] && ((NBD_PORT > 0 && NBD_PORT < 65536)) || { echo "invalid NBD port" >&2; exit 2; }
: "${IDF_PATH:?Source ESP-IDF 5.5.x export.sh first}"
export IDF_PY_BUILD_JOBS="${IDF_PY_BUILD_JOBS:-4}"
CHIP="${BOARD%-*}"
PROFILE="$BOARD"
[ "$EMU" = OFF ] || PROFILE+="-emulator"
[ "$BENCH" = OFF ] || PROFILE+="-bench"
[ "$CPU_PROFILE" = OFF ] || PROFILE+="-cpu-profile"
[ "$RELEASE" = OFF ] || PROFILE+="-release"
cd "$ROOT"
[ $# -gt 0 ] || set -- build
exec idf.py -B "build/$PROFILE" -D "IDF_TARGET=$CHIP" -D "MC_BOARD=$BOARD" \
  -D "MC_EMULATOR=$EMU" -D "MC_BENCH=$BENCH" -D "MC_CPU_PROFILE=$CPU_PROFILE" \
  -D "MC_DASHBOARD=$DASHBOARD" -D "MC_RELEASE_CONFIG=$RELEASE" \
  -D MC_QEMU_CAPTURE=OFF -D "MC_NBD_PORT=$NBD_PORT" "$@"
