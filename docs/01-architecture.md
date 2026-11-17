# 01 – Architecture

## One code base, two programs

`platformio.ini` defines two build environments that compile the same source tree:

| | `native` (the simulator) | `tdeck` (the device) |
|---|---|---|
| Command | `pio run -e native -t exec` | `pio run -e tdeck` |
| Runs on | your PC, in an SDL window shown at 2× size | the ESP32-S3 in the T-Deck |
| Screen | `lgfx::Panel_sdl` | ST7789 panel over SPI |
| SD card | the `./sdcard/` folder | a micro-SD card over SPI |
| Network | libcurl | Arduino `WiFi` + `HTTPClient` + mbedTLS |
| Extra checks | AddressSanitizer + UBSan | – |
| Output | `.pio/build/native/program` | `.pio/build/tdeck/tdeckjisho-<version>.bin` |

Almost all development happens in the simulator, where a build takes seconds, a crash
gives a stack trace, and the sanitizers catch memory bugs as they happen. The device
build is for checking what only the device can show: speed, memory, the real touch
panel, Wi-Fi.

## The layers

```
main.cpp            setup() / loop(), the same on both targets
  └─ ui/App         the screen stack, the main loop step, drawing
       ├─ screens/  one class per screen; Actions.cpp = all navigation
       ├─ ui/       widgets (ListView, TextField, RichView), Draw, Theme
       ├─ text/     document format, parsers, line breaking, pictures
       ├─ dict/     dictionary search, and the on-device dictionary builder
       └─ core/     UTF-8, romaji, URLs, settings, translations, paths
                       │
          everything ──┴─► hal/Hal.hpp   ◄── the only door to the machine
                             ├─ hal/tdeck/   the T-Deck
                             └─ hal/sdl/     the PC
```

The rule that keeps this tidy: **`core/`, `dict/` and `text/` never draw.** They know
nothing about the display. That's why `tests/run.sh` can compile them into a plain test
program with no window, no SDL and no Arduino. `text/Layout` needs to know how wide
characters are, but it asks through an interface (`rt::Metrics`), and the tests answer
with a fake "every character is 8 px wide" version.

## The HAL: choosing the machine at link time

`src/hal/Hal.hpp` declares everything the program needs from the machine: time, keys,
taps, files, Wi-Fi, HTTP, the display. It only *declares* them. Two folders implement
them:

- `src/hal/tdeck/*.cpp`, the real hardware
- `src/hal/sdl/*.cpp`, the PC

`build_src_filter` in `platformio.ini` lets the compiler see exactly one of the two
(`-<hal/sdl/>` for the device, `-<hal/tdeck/>` for the PC). A call like
`hal::millis()` is then connected by the **linker** to whichever implementation was
compiled. There are no `virtual` functions and no `#ifdef` in the callers.

This is link-time polymorphism. It costs nothing at run time, and the trade-off is that
one binary can only have one implementation, which is exactly right for "which board am
I on". In Rust this would be `#[cfg(...)]` modules. The comment at the top of `Hal.hpp`
compares it with `virtual` functions in more detail.

To add something the program needs from the machine (say, a vibration motor):

1. Declare it in `src/hal/Hal.hpp`.
2. Implement it in `src/hal/tdeck/` **and** in `src/hal/sdl/`. On the PC a stub that
   only logs is fine.
3. If the unit tests link code that calls it, add a stub to `tests/hal_stub.cpp` as
   well.

If you forget one of them, the linker tells you: `undefined reference to hal::...`.

## The main loop

The Arduino framework calls `setup()` once and then `loop()` forever (`src/main.cpp`).
On the PC, `src/hal/sdl/sdl_main.cpp` provides a `main()` that does the same in a
second thread, because SDL wants the main thread for the window.

```cpp
void setup() { hal::begin(); app.begin(); }
void loop()  { app.tick(); hal::sleepMs(5); }
```

`App::tick()` (`src/ui/App.cpp`) is the heart of the program. Every call does these
steps:

1. **Keys:** read all pending keys and hand each one to the top screen's `onKey()`.
2. **Taps:** the same for touches, through `onTap()`.
3. **Background work:** call the top screen's `update()`. Searches, downloads and the
   dictionary builder do their work here.
4. **Power saving:** dim the backlight at ¾ of the idle timeout, and switch the display
   off at the timeout (settings key `sleep_s`, default 120 s). The key that wakes the
   screen is swallowed, so it doesn't also trigger something.
5. **Status bar:** once a second, check whether the clock minute, Wi-Fi state or
   battery changed.
6. **Draw:** if anything is "dirty", redraw the whole screen.

The 5 ms sleep in `loop()` gives the ESP32's Wi-Fi tasks time to run and saves a little
power. One loop takes a few milliseconds, so `update()` runs roughly 100 times a
second.

The loop runs in a FreeRTOS task whose stack is 8 KB by default. HTTPS handshakes need
more, so `main.cpp` raises it to 16 KB with `SET_LOOP_TASK_STACK_SIZE`. If the device
reboots with "stack overflow" or "Stack canary watchpoint triggered" in the log, that is
the number to look at.

## Screens

Every screen derives from `Screen` (`src/ui/Screen.hpp`):

| Function | When it's called |
|---|---|
| `title()`, `hints()` | every frame, for the top and bottom bars |
| `onEnter(app)` | when the screen becomes the top one: when pushed, and again when the screen above it is popped |
| `onExit(app)` | just before it is removed for good |
| `onKey(app, key)` | a key; key codes are in `src/hal/Keys.hpp` |
| `onTap(app, x, y)` | a tap, in screen pixels |
| `update(app)` | every loop; must not block for long |
| `draw(app, g, area)` | draw into the off-screen canvas `g`, inside `area` |

The screens form a **stack**, like a browser's history: Home → Dictionary → Entry.
`app.push()`, `app.pop()` and `app.replace()` change it. Home can never be popped.

**Navigation is deferred.** `push`/`pop` don't change the stack immediately. They queue
an operation, which `App::applyPending()` carries out after the current `onKey()` (or
`onTap()`/`update()`) has returned. The reason: a screen often pops *itself* from
inside its own `onKey()`. If that deleted the screen right away, the rest of `onKey()`
would run on freed memory, a use-after-free that might crash only sometimes. In Rust the
borrow checker would refuse to compile it. In C++ the queue makes it impossible.

**Screens don't create each other.** They call functions in `src/screens/Actions.cpp`:
`actions::openFile()`, `actions::openUrl()`, `actions::openEntry()`, and so on. To see
the whole flow of the app, read that one file. It works like a web app's URL router.

Many screens are just "pick one item from a list". Those are not separate classes: they
are one `ListScreen` with a function object that says what happens on Enter. The Home
menu, the feed list, the articles of a feed and the language menu are all
`ListScreen`s.

## Drawing: the whole screen, every time, off-screen

The app never draws directly on the display. `App::render()` paints the whole frame
into a 320 × 240 **sprite** (an image in RAM, 150 KB, in PSRAM on the device), then
copies it to the panel in one go with `pushSprite()`.

- **No flicker.** You never see a half-drawn frame.
- **Simple screens.** A screen draws everything every time, with no "erase the old
  text" logic.
- **No fights on the SPI bus.** The display and the SD card share one bus (chapter 04).
  Drawing in one short burst means display traffic never gets mixed into an SD read.

Frames are drawn only when something changed. A screen calls `app.redraw()` when its
`onKey()` or `update()` changed what it shows. Navigation, toasts and status bar changes
ask for a frame automatically, but a key by itself does not. If a key seems to do
nothing until the next key, a missing `app.redraw()` is the usual cause.

On the device, `pushSprite()` from PSRAM uses DMA and returns *before* the transfer is
finished. `render()` therefore starts with `waitDMA()`, so the next frame isn't painted
into the sprite while the previous one is still being copied out of it.

## Long jobs

The loop must keep turning, or keys pile up and the screen freezes. The code has three
ways of doing work that takes longer than one frame.

**1. Show "Loading…", then block** (`src/screens/LoadingScreen.hpp`). For jobs of a
second or two: opening a book, fetching a web page. The screen draws "Loading…" first,
and on the *next* `update()` runs the job and replaces itself with the result. The UI
is frozen while the job runs, but the user sees why. It's the simplest pattern, and the
right one when the job can't be cut into pieces (a blocking `HTTPClient::GET()`).

**2. Start, then poll.** The job runs somewhere else (a Wi-Fi driver task, a network
stack), and the screen asks every frame "done yet?". Wi-Fi scanning
(`hal::wifiScanStart()` / `wifiScanPoll()`) and big downloads (`hal::Download`) work
like this.

**3. Steps with a time budget.** The job is cut into slices. The dictionary builder's
`step(30)` works for about 30 ms and returns, and `UpdateDictScreen::update()` calls it
every frame. The UI stays responsive through a 10-minute build.

The ESP32-S3 has two cores, so the obvious fourth way is a second FreeRTOS task. The
code avoids it on purpose: the simulator would need a different solution, and the SD
card and the display share the SPI bus, so with one thread they can never get in each
other's way. It would make a good exercise. The comment at the top of
`src/dict/DictBuilder.hpp` names the one place where it would pay off.

## Memory

| | Size | Speed | What lives there |
|---|---|---|---|
| Internal RAM | ~320 KB, ~240 KB free while running | fast | stacks, small objects, Wi-Fi/TLS buffers |
| PSRAM | 8 MB | slower | the frame sprite, books, web pages, the dictionary's sparse indexes, decoded pictures |
| Flash | 16 MB | read-only while running | the program and the bitmap fonts (two 6.25 MB app slots, see chapter 04) |

On the device, a `malloc` (and so `new`, `std::string`, `std::vector`) of more than
about 4 KB goes to PSRAM automatically, and smaller ones to internal RAM. That's why the
code prefers **one big allocation over thousands of small ones**. A document is one
`std::string` with control bytes (chapter 03), not a tree of objects. Thousands of
small allocations would fragment the little internal RAM, and one big block goes
straight to PSRAM.

The About screen (Home → `a`) shows free internal RAM and PSRAM, and every HTTP request
logs the free heap. Watch those numbers when you add features.

## The build

The important parts of `platformio.ini`:

- `[env]` holds `custom_name` and `custom_version`, our own settings. The version lives
  **only** there (chapter 06).
- `extra_scripts` are Python scripts that PlatformIO runs before the build (`pre:`):
  - `tools/name_firmware.py` names the output `tdeckjisho-<version>.bin` and compiles the
    version into the program as `JISHO_VERSION`.
  - `tools/make_ca_bundle.py` (device only) turns `tools/bundle/cacert.pem` into
    `ca_bundle.h`, the root certificates for HTTPS (chapter 05).
- `[env:tdeck]` pins the platform to `espressif32@6.9.0`, which means Arduino-ESP32
  2.0.17 on ESP-IDF 4.4. It replaces the core's C++11 flag with `-std=gnu++17`.
- `[env:native]` builds with `-fsanitize=address` for the whole program and
  `-fsanitize=undefined` for our code only (`build_src_flags`). LovyanGFX does unaligned
  reads that UBSan would report on every frame.

LovyanGFX (graphics, fonts, image decoders, and the gzip decoder the dictionary builder
uses) is the only library. PlatformIO downloads it into `.pio/libdeps/` on the first
build.
