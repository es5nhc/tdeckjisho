#!/bin/sh
# ============================================================================
#  make_sd_bundle.sh – everything a user copies onto the SD card, in one zip
# ============================================================================
#
#   tools/make_sd_bundle.sh JMdict_e.gz            (or a plain JMdict_e)
#
# Result: dist/tdeckjisho-sd-<date>.zip containing ONE folder, tdeckjisho/, so it
# never collides with Launcher or other firmware on the same card:
#
#   tdeckjisho/jisho/        the prebuilt dictionary (Python converter)
#   tdeckjisho/books/        sample books and pictures
#   tdeckjisho/config/       feeds.txt, wifi.txt.example
#   tdeckjisho/firmware/     tdeckjisho-<version>.bin for Launcher (if built: pio run -e tdeck)
#   tdeckjisho/LICENCES/     JMdict credits + licence copies (see below)
#
# The EDRDG licence asks software packages to SHIP copies of the licence and
# documentation files. Save these two pages into tools/bundle/ once (browser:
# "Save page as…"); they are then copied into every bundle:
#   https://www.edrdg.org/edrdg/licence.html            -> tools/bundle/edrdg-licence.html
#   https://creativecommons.org/licenses/by-sa/4.0/legalcode.txt -> tools/bundle/cc-by-sa-4.0.txt
# ============================================================================
set -e
SRC=${1:?usage: tools/make_sd_bundle.sh JMdict_e.gz}
OUT=dist/sd
rm -rf "$OUT" && mkdir -p "$OUT/tdeckjisho/LICENCES" "$OUT/tdeckjisho/config" "$OUT/tdeckjisho/jmdict"

python3 tools/jmdict2jisho.py "$SRC" "$OUT/tdeckjisho/jisho"
cp -r sdcard/tdeckjisho/books "$OUT/tdeckjisho/"
cp sdcard/tdeckjisho/config/feeds.txt sdcard/tdeckjisho/config/wifi.txt.example "$OUT/tdeckjisho/config/"
cp tools/bundle/JMDICT-CREDITS.txt "$OUT/tdeckjisho/LICENCES/"
for f in edrdg-licence.html cc-by-sa-4.0.txt; do
    if [ -f "tools/bundle/$f" ]; then cp "tools/bundle/$f" "$OUT/tdeckjisho/LICENCES/"
    else echo "WARNING: tools/bundle/$f missing – the licence asks for a copy in the package"; fi
done
# The build names its firmware <custom_name>-<custom_version>.bin (tools/name_firmware.py);
# read both from platformio.ini so this script never has a version of its own.
NAME=$(sed -n 's/^custom_name *= *//p' platformio.ini | head -1)
VERSION=$(sed -n 's/^custom_version *= *//p' platformio.ini | head -1)
BIN=".pio/build/tdeck/$NAME-$VERSION.bin"
if [ -f "$BIN" ]; then
    mkdir -p "$OUT/tdeckjisho/firmware"
    cp "$BIN" "$OUT/tdeckjisho/firmware/"
else
    echo "note: no firmware – run 'pio run -e tdeck' first to include it"
fi

ZIP="dist/tdeckjisho-sd-$(date +%Y%m%d).zip"
rm -f "$ZIP"
(cd "$OUT" && python3 -m zipfile -c "../$(basename "$ZIP")" tdeckjisho)
echo "wrote $ZIP:"; du -sh "$OUT/tdeckjisho"/* "$ZIP"
