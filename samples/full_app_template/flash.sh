#!/usr/bin/env bash
# flash.sh — Upload firmware to the Conexio Stratus Pro via newtmgr (serial)
#
# Usage: run from any sample directory:
#   cd samples/full_app_template
#   ./flash.sh
#
# The application name is derived from the directory this script lives in,
# so the signed binary path resolves to:
#   build/<app_dir>/zephyr/zephyr.signed.bin

set -euo pipefail

# Resolve the directory containing this script (works with symlinks too)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# The application name is the name of that directory
APP_NAME="$(basename "$SCRIPT_DIR")"

BINARY="$SCRIPT_DIR/build/${APP_NAME}/zephyr/zephyr.signed.bin"

if [ ! -f "$BINARY" ]; then
  echo "Error: firmware binary not found:"
  echo "  $BINARY"
  echo ""
  echo "Build the firmware first:"
  echo "  west build -b conexio_stratus_pro/nrf9151/ns --pristine"
  exit 1
fi

echo "Uploading firmware to device..."
echo "  App : $APP_NAME"
echo "  File: $BINARY"
echo ""

newtmgr -c serial image upload "$BINARY"

echo ""
echo "Upload complete. Reset the device to boot into the new image."
