#!/bin/sh
# Dictionary benchmark. Run from the project root.
#   tests/bench.sh                              PC speed + I/O counts
#   JISHO_SD_LATENCY_US=1000 tests/bench.sh     simulate ~1 ms per SD read
# Uses ./sdcard/tdeckjisho/jisho unless JISHO_SD points elsewhere.
set -e
mkdir -p .pio
# LovyanGFX: PlatformIO downloads it on the first `pio run` (either environment).
LGFX=${LGFX:-$(ls -d .pio/libdeps/*/LovyanGFX/src 2>/dev/null | head -1)}   # override: LGFX=/path/to/LovyanGFX/src
[ -d "$LGFX" ] || { echo "LovyanGFX not found: run 'pio run -e native' once first, or set LGFX=" >&2; exit 1; }
gcc -c -O2 -I$LGFX $LGFX/lgfx/utility/lgfx_miniz.c -o .pio/lgfx_miniz.o
g++ -std=c++17 -O2 -Isrc -I$LGFX tests/bench_dict.cpp tests/hal_stub.cpp src/dict/*.cpp src/core/*.cpp \
    src/hal/sdl/Storage.cpp .pio/lgfx_miniz.o -o .pio/bench_dict
.pio/bench_dict
