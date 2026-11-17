#!/bin/sh
# Build and run the PC unit tests. Run from the project root.
set -e
mkdir -p .pio
# LovyanGFX: PlatformIO downloads it on the first `pio run` (either environment).
LGFX=${LGFX:-$(ls -d .pio/libdeps/*/LovyanGFX/src 2>/dev/null | head -1)}   # override: LGFX=/path/to/LovyanGFX/src
[ -d "$LGFX" ] || { echo "LovyanGFX not found: run 'pio run -e native' once first, or set LGFX=" >&2; exit 1; }
# miniz (gzip inflate) comes from LovyanGFX. It is C, so compile it as C.
gcc -c -O2 -I$LGFX $LGFX/lgfx/utility/lgfx_miniz.c -o .pio/lgfx_miniz.o
g++ -std=c++17 -g -Wall -fsanitize=address,undefined -Isrc -I$LGFX tests/test_core.cpp tests/hal_stub.cpp \
    src/core/*.cpp src/dict/*.cpp src/text/*.cpp src/hal/sdl/Storage.cpp .pio/lgfx_miniz.o \
    -o .pio/test_core
# A gzipped copy of the sample, so the builder test also exercises the inflater.
mkdir -p tests/sdcard/tdeckjisho/jmdict
gzip -9 -c tools/sample_jmdict.xml > tests/sdcard/tdeckjisho/jmdict/sample_jmdict.xml.gz
# The test dictionary: the 52-word sample, converted by tools/jmdict2jisho.py.
python3 tools/jmdict2jisho.py tools/sample_jmdict.xml tests/sdcard/tdeckjisho/jisho > /dev/null
JISHO_SD=tests/sdcard .pio/test_core   # the 52-word sample, not the real dictionary
