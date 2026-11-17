# 07 – Testing and release

Most of this project's code was written with an AI assistant. The tests and the
sanitizer builds are how its behaviour is checked, so run them after every change that
matters.

## Unit tests

```sh
tests/run.sh
```

This builds `tests/test_core.cpp` together with everything in `src/core/`, `src/dict/`
and `src/text/`, plus the PC file layer, and runs it on the 52-word sample dictionary.
It needs LovyanGFX in `.pio/libdeps/` (any `pio run` downloads it) because the gzip
decoder comes from there.

It's a plain program, not a test framework: each check prints a line when it fails, and
the program exits non-zero if anything failed. `tests/hal_stub.cpp` supplies the few
`hal::` functions the tested code calls (`log()`, `millis()`, the secrets), so no display
or Arduino is needed. That's possible only because `core/`, `dict/` and `text/` never
draw (chapter 01).

What it covers: UTF-8 helpers, romaji, de-inflection, URLs and the redirect policy,
settings, every parser and their invariants, the layout with a fake `Metrics`, the
cursor, picture headers, dictionary search and look-up, the on-device dictionary
builder, and the `Strings.def` rules (chapter 06).

**Where to add a test:** next to the existing ones for the same area in
`test_core.cpp`. A bug is best fixed by first writing the test that shows it.

## Sanitizers

Both the tests and the simulator are built with:

- **AddressSanitizer** (`-fsanitize=address`): reading or writing outside a buffer,
  using memory after it was freed, leaks. The program stops at the bad access and prints
  where it happened and where the memory came from.
- **UndefinedBehaviorSanitizer** (`-fsanitize=undefined`): signed overflow, shifts too
  far, misaligned access, and other things C++ calls "undefined": they might work on the
  PC and do something else on the ESP32.

So **using the simulator is also testing**. A memory bug that would corrupt something
silently on the device stops the simulator with a report. Don't ignore those reports.

The simulator tells LeakSanitizer to ignore leaks inside SDL and X11, which hold some
memory on purpose until exit (`sdl_main.cpp`). A leak in our code is still reported.

## The converter comparison

```sh
tests/build_compare.sh                    # the 52-word sample, seconds
tests/build_compare.sh JMdict_e.gz        # the real dictionary, a few minutes
```

This builds the dictionary with the Python converter **and** with the on-device C++
builder (compiled for the PC as `tests/build_cli.cpp`), then compares `entries.dat`,
`kana.idx`, `kanji.idx` and `eng.idx` byte for byte. Run it after any change to
`tools/jmdict2jisho.py` or `src/dict/Jmdict*` / `ExternalSort.*` / `DictBuilder.*`
(chapter 02).

Two environment variables make it harder:

- `JISHO_BUILD_RAM_KB=1024` uses tiny sort buffers, which means many runs and a real
  test of the merge.
- `JISHO_SD_LATENCY_US=1000` makes every file read wait 1 ms, as the SD card does.

## Benchmark

```sh
tests/bench.sh
JISHO_SD_LATENCY_US=1000 tests/bench.sh
```

This runs typical searches on the dictionary in `./sdcard/` and prints the time and the
number of SD **operations** for each. The PC's SSD and file cache make every read nearly
free, so the PC's raw times say little about the device. The operation counts are the
same on both, though, and with the 1 ms latency the times come close to the device's.
Then confirm with the `[search]` lines in the device's log: measuring beats guessing.

## Simulator tricks

| Variable | Effect |
|---|---|
| `JISHO_SD=path` | use another folder as the SD card (default `./sdcard`) |
| `JISHO_SD_LATENCY_US=1000` | every SD read waits 1 ms, like the device |
| `JISHO_NET_KBPS=500` | limit downloads to about real Wi-Fi speed |
| `JISHO_SECRETS=path` | where saved Wi-Fi passwords go (default `./.jisho-secrets`) |
| `JISHO_KEYS='…'` | type a script of keys automatically (below) |

F12 saves a screenshot to `./screenshots/`.

### Key scripts

`JISHO_KEYS` makes the simulator type by itself. That's handy for screenshots and for
repeating a bug the same way every time:

```sh
JISHO_KEYS='d{wait}taberu{wait}{shot}{enter}{wait}{shot}{quit}' pio run -e native -t exec
```

Plain characters are typed as they are. The tokens are `{up}` `{down}` `{left}`
`{right}` `{enter}` `{bs}` `{tab}`, `{wait}` (let the app catch up), `{shot}` (a
screenshot), `{tap:x,y}` (a click at a screen position) and `{quit}`. The screenshots
in `docs/img/` were made this way.

## Hostile input: `docs/security/`

Everything the parsers read can come from outside: a web page, a feed, a book someone
sent you, a download. Two programs attack them on purpose:

**`probe.cpp`** checks specific suspected weaknesses: invalid UTF-8, out-of-range
entities, input that could make a parser quadratic (100,000 `&` without `;`), feeds cut
off mid-tag. It prints timings and results:

```sh
LGFX=$(ls -d .pio/libdeps/*/LovyanGFX/src | head -1)
gcc -c -O2 -I$LGFX $LGFX/lgfx/utility/lgfx_miniz.c -o .pio/lgfx_miniz.o
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -Isrc -I$LGFX docs/security/probe.cpp \
    tests/hal_stub.cpp src/core/*.cpp src/dict/*.cpp src/text/*.cpp src/hal/sdl/Storage.cpp \
    .pio/lgfx_miniz.o -o .pio/probe && .pio/probe
```

Times that grow 4× when the input grows 2× mean something is quadratic.

**`fuzz_all.cpp`** is a libFuzzer target. The fuzzer generates millions of random,
mutating inputs, the first byte choosing which parser gets the rest, and the sanitizers
catch any crash. It needs **clang** (libFuzzer is part of it). This command hasn't been
tested here, because this machine has only g++:

```sh
clang++ -std=c++17 -O1 -g -fsanitize=fuzzer,address,undefined -Isrc -I$LGFX \
    docs/security/fuzz_all.cpp tests/hal_stub.cpp src/core/*.cpp src/dict/*.cpp \
    src/text/*.cpp src/hal/sdl/Storage.cpp .pio/lgfx_miniz.o -o .pio/fuzz_all
.pio/fuzz_all -max_total_time=300
```

A crash leaves a `crash-…` file containing the input. Pass that file to the program
again to reproduce it, and add it to `test_core.cpp` once it's fixed.

## Releasing

A checklist for a version you give to someone else:

1. **Version:** raise `custom_version` in `platformio.ini`.
2. **Certificates:** copy a fresh `tools/bundle/cacert.pem` from your OS (chapter 05).
3. **Tests:** `tests/run.sh` and `tests/build_compare.sh` both pass. Run the latter with
   the real JMdict if anything in `dict/` changed.
4. **Build:** `pio run -e tdeck`. The log shows `make_ca_bundle: 1xx root certificates`
   and the new file name.
5. **On the device:** install through Launcher and try search, a book with furigana and
   pictures, a feed over HTTPS, the touch test. Watch the serial log for errors.
6. **Keep the `.elf`.** `tdeckjisho-<version>.elf` is what decodes crash reports from
   this exact build. A rebuilt `.elf` from the same sources is not guaranteed to match.
7. **SD bundle:** `tools/make_sd_bundle.sh JMdict_e.gz` makes
   `dist/tdeckjisho-sd-<date>.zip` with the dictionary, sample books, default feeds,
   licences and the firmware. For the EDRDG licence, save its two pages into
   `tools/bundle/` once; the script says which.
8. **Licences:** a firmware binary contains LovyanGFX, its fonts, the Arduino core and
   ESP-IDF. Ship their licence texts with it, as listed in
   [THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md). The IPA font licence requires
   it. The Arduino core is LGPL and linked statically, so a binary must come with a way
   to rebuild it: this repository and these instructions are that.
