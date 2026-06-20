#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IDF="$ROOT_DIR/scripts/idf.sh"
ACTION="${1:-help}"
PORT="${PORT:-/dev/cu.usbmodem101}"
BUILD_BASE="${BUILD_BASE:-build-hardware-smoke}"

idf_build() {
    local target="$1"
    local build_dir="$2"
    local defaults="$3"

    "$IDF" -B "$build_dir" \
        -D "IDF_TARGET=$target" \
        -D "SDKCONFIG=$build_dir/sdkconfig" \
        -D "SDKCONFIG_DEFAULTS=$defaults" \
        build
}

usage() {
    cat <<'USAGE'
Usage: scripts/hardware_smoke.sh <action>

Actions:
  build-s3          Build the default ESP32-S3 firmware.
  build-s3-secure   Build ESP32-S3 with flash encryption + encrypted NVS.
  build-h752        Build the LilyGo H752 e-paper firmware profile.
  build-cyd         Build an ESP32/CYD firmware profile.
  flash-encrypted   Run encrypted flash. Requires MESHPAY_HW_CONFIRM=flash.
  monitor           Open idf.py monitor on PORT.
  help              Show this help.

Environment:
  PORT              Serial port, default /dev/cu.usbmodem101.
  BUILD_BASE        Build directory prefix, default build-hardware-smoke.
USAGE
}

case "$ACTION" in
    build-s3)
        idf_build esp32s3 \
            "$ROOT_DIR/$BUILD_BASE-s3" \
            "$ROOT_DIR/sdkconfig.defaults;$ROOT_DIR/sdkconfig.defaults.esp32s3"
        ;;
    build-s3-secure)
        idf_build esp32s3 \
            "$ROOT_DIR/$BUILD_BASE-s3-secure" \
            "$ROOT_DIR/sdkconfig.defaults;$ROOT_DIR/sdkconfig.defaults.esp32s3;$ROOT_DIR/sdkconfig.defaults.secure"
        ;;
    build-h752)
        idf_build esp32s3 \
            "$ROOT_DIR/$BUILD_BASE-h752" \
            "$ROOT_DIR/sdkconfig.defaults;$ROOT_DIR/sdkconfig.defaults.h752"
        ;;
    build-cyd)
        idf_build esp32 \
            "$ROOT_DIR/$BUILD_BASE-cyd" \
            "$ROOT_DIR/sdkconfig.defaults;$ROOT_DIR/sdkconfig.defaults.esp32"
        ;;
    flash-encrypted)
        if [[ "${MESHPAY_HW_CONFIRM:-}" != "flash" ]]; then
            echo "Refusing to flash without MESHPAY_HW_CONFIRM=flash" >&2
            exit 2
        fi
        secure_build="$ROOT_DIR/$BUILD_BASE-s3-secure"
        if [[ ! -f "$secure_build/sdkconfig" ]]; then
            echo "Missing secure sdkconfig. Run build-s3-secure first." >&2
            exit 2
        fi
        "$IDF" -B "$secure_build" \
            -D "SDKCONFIG=$secure_build/sdkconfig" \
            -p "$PORT" encrypted-flash
        ;;
    monitor)
        "$IDF" -B "$ROOT_DIR/$BUILD_BASE-s3" -p "$PORT" monitor
        ;;
    help|-h|--help)
        usage
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac
