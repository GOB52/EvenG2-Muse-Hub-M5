#!/usr/bin/env bash
# Build / flash / monitor the hub firmware (the Gadget) with ESP-IDF v6.0.3.
#
#   hub/tools/build.sh build
#   hub/tools/build.sh flash         PORT
#   hub/tools/build.sh monitor       PORT
#   hub/tools/build.sh flash-monitor PORT
#
# BOARD picks the M5Stack device (default sticks3):
#   sticks3    M5Stack StickS3       (esp32s3, build in hub/build-sticks3)
#   cplus2     M5Stack StickC Plus2  (esp32,   build in hub/build-cplus2, flashed at 230400 baud)
#   stopwatch  M5Stack StopWatch     (esp32s3, build in hub/build-stopwatch)
# e.g. BOARD=cplus2 hub/tools/build.sh build
#
# build applies hub/patches/ to the SDK submodule first (tools/apply_patches.sh).
# Activate ESP-IDF v6.0.3 first (IDF_PATH must be set). Set IDF_PY to use
# another idf.py front end (e.g. a wrapper that serializes configure).
# sdkconfig layers: the SDK's defaults and board overlay, hub/sdkconfig.hub,
# hub/sdkconfig.hub-<board> if present, then hub/sdkconfig.local if present
# (put CONFIG_GADGET_SDK_TOKEN there; it is git-ignored). sdkconfig defaults
# only seed a new sdkconfig, so after changing any of them (including pulling
# a change), remove hub/build-<board>/sdkconfig and rebuild.
set -euo pipefail

cmd=${1:?build|flash|monitor|flash-monitor}
port=${2:-}
board=${BOARD:-sticks3}

[ -n "${IDF_PATH:-}" ] || { echo "IDF_PATH is not set; activate ESP-IDF v6.0.3 first" >&2; exit 1; }

hub_dir=$(cd "$(dirname "$0")/.." && pwd)
sdk_dir=$hub_dir/third_party/muse-gadget-sdk/esp32

# The CH9102 USB-UART bridge on the StickC Plus2 drops out above 230400 baud.
flash_baud=()
case $board in
    sticks3)
        target=esp32s3
        sdk_overlay=sdkconfig.muse-m5stack-sticks3 ;;
    cplus2)
        target=esp32
        sdk_overlay=sdkconfig.muse-m5stack-stickc-plus2
        flash_baud=(-b 230400) ;;
    stopwatch)
        target=esp32s3
        sdk_overlay=sdkconfig.muse-m5stack-stopwatch ;;
    *)
        echo "unknown BOARD: $board (sticks3, cplus2 or stopwatch)" >&2; exit 1 ;;
esac
build_dir=$hub_dir/build-$board

[ -f "$sdk_dir/cmake/project.cmake" ] || { echo "SDK not found; run: git submodule update --init" >&2; exit 1; }

defaults="$sdk_dir/sdkconfig.defaults;$sdk_dir/devices/sdkconfig.muse;$sdk_dir/devices/$sdk_overlay;$hub_dir/sdkconfig.hub"
[ -f "$hub_dir/sdkconfig.hub-$board" ] && defaults="$defaults;$hub_dir/sdkconfig.hub-$board"
[ -f "$hub_dir/sdkconfig.local" ] && defaults="$defaults;$hub_dir/sdkconfig.local"

if [ -n "${IDF_PY:-}" ]; then
    idf=("$IDF_PY")
else
    idf=(python "$IDF_PATH/tools/idf.py")
fi

common=(-C "$hub_dir" -B "$build_dir" -DIDF_TARGET="$target"
        -DSDKCONFIG="$build_dir/sdkconfig" -DSDKCONFIG_DEFAULTS="$defaults")

case $cmd in
    build)
        "$hub_dir/tools/apply_patches.sh"
        "${idf[@]}" "${common[@]}" build ;;
    flash|monitor|flash-monitor)
        [ -n "$port" ] || { echo "PORT is required (e.g. /dev/cu.usbmodem1101)" >&2; exit 1; }
        actions=${cmd/-/ }
        # shellcheck disable=SC2086
        # ${a[@]+...}: an empty array is "unbound" under set -u in bash 3.2 (macOS).
        "${idf[@]}" "${common[@]}" ${flash_baud[@]+"${flash_baud[@]}"} -p "$port" $actions ;;
    *)
        echo "unknown command: $cmd" >&2; exit 1 ;;
esac
