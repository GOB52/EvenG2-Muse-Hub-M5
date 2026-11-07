#!/usr/bin/env bash
# Build / flash / monitor the hub firmware (M5Stack StickS3) with ESP-IDF v6.0.3.
#
#   hub/tools/build.sh build
#   hub/tools/build.sh flash         PORT
#   hub/tools/build.sh monitor       PORT
#   hub/tools/build.sh flash-monitor PORT
#
# build applies hub/patches/ to the SDK submodule first (tools/apply_patches.sh).
# Activate ESP-IDF v6.0.3 first (IDF_PATH must be set). Set IDF_PY to use
# another idf.py front end (e.g. a wrapper that serializes configure).
# hub/sdkconfig.local, if present, is applied last (put CONFIG_GADGET_SDK_TOKEN
# there; it is git-ignored). sdkconfig defaults only seed a new sdkconfig, so
# after changing hub/sdkconfig.hub or hub/sdkconfig.local (including pulling a
# change to sdkconfig.hub), remove hub/build-sticks3/sdkconfig and rebuild.
set -euo pipefail

cmd=${1:?build|flash|monitor|flash-monitor}
port=${2:-}

[ -n "${IDF_PATH:-}" ] || { echo "IDF_PATH is not set; activate ESP-IDF v6.0.3 first" >&2; exit 1; }

hub_dir=$(cd "$(dirname "$0")/.." && pwd)
sdk_dir=$hub_dir/third_party/muse-gadget-sdk/esp32
build_dir=$hub_dir/build-sticks3

[ -f "$sdk_dir/cmake/project.cmake" ] || { echo "SDK not found; run: git submodule update --init" >&2; exit 1; }

defaults="$sdk_dir/sdkconfig.defaults;$sdk_dir/devices/sdkconfig.muse;$sdk_dir/devices/sdkconfig.muse-m5stack-sticks3;$hub_dir/sdkconfig.hub"
[ -f "$hub_dir/sdkconfig.local" ] && defaults="$defaults;$hub_dir/sdkconfig.local"

if [ -n "${IDF_PY:-}" ]; then
    idf=("$IDF_PY")
else
    idf=(python "$IDF_PATH/tools/idf.py")
fi

common=(-C "$hub_dir" -B "$build_dir" -DIDF_TARGET=esp32s3
        -DSDKCONFIG="$build_dir/sdkconfig" -DSDKCONFIG_DEFAULTS="$defaults")

case $cmd in
    build)
        "$hub_dir/tools/apply_patches.sh"
        "${idf[@]}" "${common[@]}" build ;;
    flash|monitor|flash-monitor)
        [ -n "$port" ] || { echo "PORT is required (e.g. /dev/cu.usbmodem1101)" >&2; exit 1; }
        actions=${cmd/-/ }
        # shellcheck disable=SC2086
        "${idf[@]}" "${common[@]}" -p "$port" $actions ;;
    *)
        echo "unknown command: $cmd" >&2; exit 1 ;;
esac
