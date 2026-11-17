#!/bin/sh
# Build a dictionary with BOTH converters and compare the files byte for byte.
# Run from the project root.
#
#   tests/build_compare.sh                         the 52-word sample
#   tests/build_compare.sh JMdict_e                the real thing (plain or .gz)
#   JISHO_BUILD_RAM_KB=1024 tests/build_compare.sh JMdict_e   tiny buffers: many runs
#   JISHO_SD_LATENCY_US=1000 tests/build_compare.sh JMdict_e  feel the SD card
#
# Identical output proves that the C++ port (src/dict/Jmdict*.cpp,
# ExternalSort.cpp) follows every rule of tools/jmdict2jisho.py.
set -e
SRC=${1:-tools/sample_jmdict.xml}
# LovyanGFX: PlatformIO downloads it on the first `pio run` (either environment).
LGFX=${LGFX:-$(ls -d .pio/libdeps/*/LovyanGFX/src 2>/dev/null | head -1)}   # override: LGFX=/path/to/LovyanGFX/src
[ -d "$LGFX" ] || { echo "LovyanGFX not found: run 'pio run -e native' once first, or set LGFX=" >&2; exit 1; }
WORK=.pio/build_compare
rm -rf $WORK && mkdir -p $WORK/sd/tdeckjisho/jmdict $WORK/python

gcc -c -O2 -I$LGFX $LGFX/lgfx/utility/lgfx_miniz.c -o .pio/lgfx_miniz.o
g++ -std=c++17 -O2 -Wall -Isrc -I$LGFX tests/build_cli.cpp tests/hal_stub.cpp \
    src/dict/BuildIo.cpp src/dict/JmdictXml.cpp src/dict/JmdictEncode.cpp \
    src/dict/ExternalSort.cpp src/dict/DictBuilder.cpp src/core/*.cpp \
    src/hal/sdl/Storage.cpp .pio/lgfx_miniz.o -o .pio/build_cli

NAME=$(basename "$SRC")
ln -s "$(realpath "$SRC")" "$WORK/sd/tdeckjisho/jmdict/$NAME"

echo "== Python converter"
python3 tools/jmdict2jisho.py "$SRC" $WORK/python | tail -3
echo "== C++ on-device builder (on the PC)"
.pio/build_cli $WORK/sd "/tdeckjisho/jmdict/$NAME" /tdeckjisho/jisho.new

echo "== compare"
status=0
for f in entries.dat kana.idx kanji.idx eng.idx; do
    if cmp "$WORK/python/$f" "$WORK/sd/tdeckjisho/jisho.new/$f"; then echo "  $f identical"; else status=1; fi
done
exit $status
