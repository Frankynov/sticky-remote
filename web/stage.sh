#!/bin/bash
# Copies what goes online into build/upload/sticky/ (drag that folder's
# contents to /sticky/ on the server).
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=build/upload/sticky
rm -rf "$OUT" && mkdir -p "$OUT/vendor" "$OUT/firmware"
cp web/index.html web/wizard.css web/wizard.js web/sticky-serial.js web/ha.js web/device-preview.js web/icon.png "$OUT/"
ditto web/icons "$OUT/icons"          # Lucide-style icons (ISC), used as CSS masks
ditto web/fonts "$OUT/fonts"          # the firmware's exact glyphs (make_web_fonts.py)
ditto web/previews "$OUT/previews"    # screens rendered by the firmware code (make_previews.py)
cp web/htaccess "$OUT/.htaccess"
cp web/vendor/*.js web/vendor/*.txt "$OUT/vendor/"
cp web/firmware/manifest.json web/firmware/*.factory.bin "$OUT/firmware/"
echo "▸ Upload folder: $OUT"
find "$OUT" -type f | sed "s|$OUT/|  |" | sort
