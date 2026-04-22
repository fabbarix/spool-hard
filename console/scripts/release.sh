#!/usr/bin/env bash
set -euo pipefail

# SpoolHard Console — Release Build Script
# Builds firmware + frontend, creates SPIFFS image, and generates OTA manifest.
#
# Usage:
#   ./scripts/release.sh                          # builds to release/
#   ./scripts/release.sh --base-url https://...   # sets manifest download URL
#   ./scripts/release.sh --output-dir dist        # custom output directory

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRODUCT_DIR="$(dirname "$SCRIPT_DIR")"
FIRMWARE_DIR="$PRODUCT_DIR/firmware"
FRONTEND_DIR="$PRODUCT_DIR/frontend"
DATA_DIR="$FIRMWARE_DIR/data"
BUILD_DIR="$FIRMWARE_DIR/.pio/build/esp32-s3"
# Capture the caller's $PWD before we cd anywhere — any relative
# --output-dir from the caller is resolved against this, not against
# whatever directory the script happens to be in when it writes.
INVOKE_DIR="$PWD"

OUTPUT_DIR="$PRODUCT_DIR/release"
BASE_URL=""
# Asset-name prefix used inside the generated manifest URLs. CI uploads
# the per-product binaries flattened into a single GitHub Release with
# names like `spoolhard-console-firmware.bin`; pass `--asset-prefix
# spoolhard-console-` here so the manifests reference those names. Local
# builds leave the prefix empty.
ASSET_PREFIX=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --base-url)     BASE_URL="$2";     shift 2 ;;
    --output-dir)   OUTPUT_DIR="$2";   shift 2 ;;
    --asset-prefix) ASSET_PREFIX="$2"; shift 2 ;;
    -h|--help)
      echo "Usage: $0 [--base-url URL] [--output-dir DIR] [--asset-prefix PREFIX]"
      exit 0 ;;
    *) echo "Unknown option: $1"; exit 1 ;;
  esac
done

# Promote OUTPUT_DIR to absolute. The build steps `cd` into firmware/
# and frontend/ later on, so a relative path here would land in the
# wrong place when we write artifacts.
case "$OUTPUT_DIR" in
  /*) ;;                          # already absolute
  *)  OUTPUT_DIR="$INVOKE_DIR/$OUTPUT_DIR" ;;
esac

VERSION_FILE="$PRODUCT_DIR/VERSION"
if [[ ! -f "$VERSION_FILE" ]]; then
  echo "Missing $VERSION_FILE — create it first (one version string per line)."
  exit 1
fi
FW_VERSION=$(tr -d '[:space:]' < "$VERSION_FILE")
FE_VERSION="$FW_VERSION"

echo "╔══════════════════════════════════════════════════════╗"
echo "║  SpoolHard Console Release Build                    ║"
echo "║  Firmware: $FW_VERSION"
echo "║  Frontend: $FE_VERSION"
echo "╚══════════════════════════════════════════════════════╝"

echo "── [1/5] Building frontend..."
cd "$FRONTEND_DIR"
if [ ! -d node_modules ]; then npm ci --silent; fi
npm run build --silent
echo "   Frontend built to $DATA_DIR"

echo "── [2/5] Gzipping frontend assets..."
find "$DATA_DIR" -type f ! -name '*.gz' | while read -r f; do
  gzip -9 -f "$f"
  echo "   $(basename "$f").gz"
done

# Plant the product signature as a plain (non-gzipped) file. mkspiffs embeds
# file bytes verbatim, so the signature appears literally in the packed image
# and the upload-side matcher can recognise it. Must happen *after* gzip so
# the marker itself isn't compressed.
echo "SPOOLHARD-PRODUCT=console" > "$DATA_DIR/.spoolhard-product"
echo "   .spoolhard-product (product signature)"

# Same trick for the frontend version. The upload-side VersionMarkerParser
# scans the stream for "SPOOLHARD-VERSION=<v>\x01" and upgrades the LCD
# label from "frontend.bin" to "v<version>" once it's seen. Printf emits
# the 0x01 sentinel literally; `\x01` is portable across bash/sh.
printf 'SPOOLHARD-VERSION=%s\x01' "$FE_VERSION" > "$DATA_DIR/.spoolhard-version"
echo "   .spoolhard-version (version marker)"

echo "── [3/5] Building firmware..."
cd "$FIRMWARE_DIR"
pio run -e esp32-s3 --silent
echo "   Firmware: $(du -h "$BUILD_DIR/firmware.bin" | cut -f1)"

echo "── [4/5] Building frontend image..."
pio run -e esp32-s3 -t buildfs --silent 2>/dev/null || true
echo "   Frontend: $(du -h "$BUILD_DIR/spiffs.bin" | cut -f1)"

echo "── [5/5] Assembling release..."
mkdir -p "$OUTPUT_DIR"
cp "$BUILD_DIR/firmware.bin" "$OUTPUT_DIR/firmware.bin"
# Rename the SPIFFS artifact to frontend.bin in the release bundle; it's
# purely a user-visible rename — the partition underneath is still SPIFFS.
cp "$BUILD_DIR/spiffs.bin"   "$OUTPUT_DIR/frontend.bin"

# Bootloader + partition table also belong in the release so the web
# flasher (esp-web-tools) can do a clean fresh-flash over USB. boot_app0
# is the static OTA-pointer blob shipped with the arduino-esp32 framework
# — same file every build, just needs to be co-located with the others.
cp "$BUILD_DIR/bootloader.bin"   "$OUTPUT_DIR/bootloader.bin"
cp "$BUILD_DIR/partitions.bin"   "$OUTPUT_DIR/partitions.bin"
BOOT_APP0=$(find "$HOME/.platformio/packages/framework-arduinoespressif32" \
              -name boot_app0.bin 2>/dev/null | head -1)
if [[ -n "$BOOT_APP0" ]]; then
  cp "$BOOT_APP0" "$OUTPUT_DIR/boot_app0.bin"
else
  echo "   ! boot_app0.bin not found — flasher manifest will reference a missing asset" >&2
fi

FW_SIZE=$(stat -c%s "$OUTPUT_DIR/firmware.bin" 2>/dev/null || stat -f%z "$OUTPUT_DIR/firmware.bin")
SP_SIZE=$(stat -c%s "$OUTPUT_DIR/frontend.bin" 2>/dev/null || stat -f%z "$OUTPUT_DIR/frontend.bin")
FW_SHA=$(sha256sum "$OUTPUT_DIR/firmware.bin" | cut -d' ' -f1)
SP_SHA=$(sha256sum "$OUTPUT_DIR/frontend.bin" | cut -d' ' -f1)

if [ -z "$BASE_URL" ]; then BASE_URL="https://REPLACE_WITH_ACTUAL_URL"; fi

# `$BASE_URL/$ASSET_PREFIX<file>` is what each manifest URL becomes; with
# the prefix empty (local builds) it collapses to `$BASE_URL/<file>`.
cat > "$OUTPUT_DIR/manifest.json" <<EOF
{
  "firmware": {
    "version": "$FW_VERSION",
    "url": "$BASE_URL/${ASSET_PREFIX}firmware.bin",
    "size": $FW_SIZE,
    "sha256": "$FW_SHA"
  },
  "frontend": {
    "version": "$FE_VERSION",
    "url": "$BASE_URL/${ASSET_PREFIX}frontend.bin",
    "size": $SP_SIZE,
    "sha256": "$SP_SHA"
  }
}
EOF

# esp-web-tools manifest for the browser-based flasher. Offsets come from
# console/firmware/partitions.csv — bootloader at 0x0, partition table at
# 0x8000, otadata-pointer (boot_app0) at 0xe000, app at 0x10000, SPIFFS
# at 0x610000. `new_install_prompt_erase: true` makes esp-web-tools wipe
# the chip before writing — appropriate for a "clean install" flow.
cat > "$OUTPUT_DIR/flasher-manifest.json" <<EOF
{
  "name": "SpoolHard Console",
  "version": "$FW_VERSION",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "ESP32-S3",
      "parts": [
        { "path": "$BASE_URL/${ASSET_PREFIX}bootloader.bin", "offset": 0 },
        { "path": "$BASE_URL/${ASSET_PREFIX}partitions.bin", "offset": 32768 },
        { "path": "$BASE_URL/${ASSET_PREFIX}boot_app0.bin",  "offset": 57344 },
        { "path": "$BASE_URL/${ASSET_PREFIX}firmware.bin",   "offset": 65536 },
        { "path": "$BASE_URL/${ASSET_PREFIX}frontend.bin",   "offset": 6356992 }
      ]
    }
  ]
}
EOF

echo
echo "Release artifacts:"
ls -lh "$OUTPUT_DIR/" | tail -n +2 | sed 's/^/  /'
echo
cat "$OUTPUT_DIR/manifest.json"
echo
echo "Done."
