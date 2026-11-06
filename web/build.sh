#!/bin/bash
# Builds the generic firmware and stages the web wizard for upload:
#   web/firmware/sticky-remote-<version>.factory.bin + manifest.json, web/icon.png
#
#   ESPHOME=/path/to/esphome web/build.sh      (default: esphome on PATH)
set -euo pipefail
cd "$(dirname "$0")/.."
ESPHOME=${ESPHOME:-esphome}
VERSION=$(sed -n 's/^  firmware_version: "\(.*\)"/\1/p' firmware/sticky-remote.yaml)
[ -n "$VERSION" ] || { echo "No firmware_version in firmware/sticky-remote.yaml"; exit 1; }

echo "▸ Building the generic firmware $VERSION…"
"$ESPHOME" compile firmware/sticky-remote-generic.yaml
BIN=firmware/.esphome/build/sticky-remote/build/firmware.factory.bin
[ -f "$BIN" ] || BIN=$(find firmware/.esphome/build/sticky-remote -name firmware.factory.bin | head -1)

rm -f web/firmware/*.bin
FILE="sticky-remote-$VERSION.factory.bin"
cp "$BIN" "web/firmware/$FILE"
SIZE=$(stat -f%z "web/firmware/$FILE")
SHA=$(shasum -a 256 "web/firmware/$FILE" | cut -d' ' -f1)
cat > web/firmware/manifest.json <<JSON
{
  "name": "Sticky Remote",
  "version": "$VERSION",
  "chip": "ESP32-S3",
  "file": "$FILE",
  "size": $SIZE,
  "sha256": "$SHA",
  "built": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
JSON
echo "▸ Firmware: web/firmware/$FILE ($SIZE bytes)"

# The device previews: the build's exact glyphs, and screens rendered by the
# firmware's own drawing code (the command-line simulator).
echo "▸ Device previews…"
python3 web/make_web_fonts.py
engine/build.sh > /dev/null
python3 web/make_previews.py

ICTOOL="/Applications/Xcode.app/Contents/Applications/Icon Composer.app/Contents/Executables/ictool"
if [ -x "$ICTOOL" ]; then
  "$ICTOOL" design/AppIcon.icon --export-image --output-file web/icon.png --platform macOS \
    --rendition Default --width 256 --height 256 --scale 1 > /dev/null
fi
web/stage.sh
