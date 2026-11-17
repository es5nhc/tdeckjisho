# 06 – Customising

Where to change things, from the simplest to the most involved. Try each change in the
simulator first (`pio run -e native -t exec`). It takes seconds there.

## Version and firmware name

The version lives in **one** place, `platformio.ini`:

```ini
[env]
custom_name = tdeckjisho
custom_version = 0.1.1
```

`tools/name_firmware.py` turns these into:

- the file names `.pio/build/tdeck/tdeckjisho-0.1.1.bin` and `.elf`, instead of the
  `firmware.bin` every ESP32 project gets by default
- the macro `JISHO_VERSION`, which the About screen shows and the HTTP `User-Agent`
  carries

So the file name and the version on the screen can never disagree. The simulator's
program stays `.pio/build/native/program`.

The script is reusable. Copy it into another PlatformIO project and set the two
`custom_` options there. Your editor will mark `Import` and `env` in it as undefined.
That's normal for PlatformIO scripts: SCons provides them when it runs the script.

## Colours

All colours are in `src/ui/Theme.hpp`:

| Constant | Used for |
|---|---|
| `kBg` | the background |
| `kText` | normal text |
| `kBgBar` | the top and bottom bars, and the toast background |
| `kDim` | grey text: tags, dates, small print |
| `kAccent` | highlights: numbers in entries, the toast border, low battery |
| `kLink` | links |
| `kSelBg` | the selected row in a list |
| `kRule` | horizontal lines |
| `kOk`, `kWifiOK` | green: success, the Wi-Fi indicator |
| `kPreColor` | the background of preformatted text |

Write colours as `rgb(r, g, b)` with 0–255 per channel. The function packs them into
**RGB565**: 16 bits per pixel, 5 for red, 6 for green, 5 for blue, the panel's own
format. Being `constexpr`, it runs at compile time and costs nothing. RGB565 has only
32 levels of red and blue, so subtle shades can collapse into one. Check a new palette
on the device, because the panel shows colours differently from a PC monitor.

The bar text is always white (`kWhiteColour` in `App::drawTopBar` and `drawBottomBar`),
so a light `kBgBar` also needs a change there.

## Fonts

Also in `Theme.hpp`. Every font is a pointer to a bitmap font compiled into flash:

| Function | Used for | Now |
|---|---|---|
| `fontUi()` | lists, input fields | `lgfxJapanGothicP_16` |
| `fontSmall()` | the bars, toasts | `lgfxJapanGothicP_12` |
| `fontBody(size)` | the reader, by size 12 / 16 / 20 | `lgfxJapanGothicP_*` |
| `fontH1(size)` | headings | 24 px, or 16 with small body text |
| `fontMono(size)` | preformatted text | `efontJA_12` / `_16` |
| `fontRuby()` | furigana | `efontJA_10` |
| `fallback(font)` | characters the font lacks (š, ž, „) | the efontJA of the same size |

Before switching to another LovyanGFX font, check that it has every character you need.
For Estonian that's õ ä ö ü š ž, and a missing one is drawn by the fallback font or as a
stand-in (chapter 03). Every size you use costs a few hundred KB of flash, and the build
log's `Flash:` line shows how much is left.

LovyanGFX can also load smooth `.vlw` fonts from the SD card (`loadFont()`): any size,
real bold, any characters. That would be the way to get beyond what the built-in fonts
offer. The font functions exist so that such a change stays inside `Theme.hpp`.

## Screen layout

`Theme.hpp` also has the fixed geometry: `kStatusH` (top bar, 16 px), `kHintH` (bottom
bar, 13 px), and from those `kContentY` and `kContentH`. Screens draw inside the `area`
they are given, so changing the bar heights moves everything consistently. Search for
the bar heights before changing them, though: `SearchScreen` has its own `kListY`.

## Texts and translations

Every text the user sees is in `src/core/Strings.def`, one row per text with all three
languages side by side:

```cpp
X(HomeLibrary,  "Teek", "Library", "本棚")
```

The code names texts, it never contains them: `tr(Str::HomeLibrary)`. Placeholders are
filled by name, `fmt(Str::OpeningFile, {{"name", file}})`, and plurals have two rows
used through `trn(one, other, n)`.

- **To change a text,** edit its row. Nothing else.
- **To add a text,** add a row, then use `Str::YourId` in the code. Leaving a language
  `""` means "not translated yet", and English is shown instead.
- **Placeholders** like `{name}` must be the same in all three languages. Word order
  differs between languages, so give the translator the whole sentence. Never glue a
  sentence together from pieces in code.
- **Japanese:** avoid `~` and `\`. The Japanese font draws them as ‾ and ¥ (chapter 03).
- `tests/run.sh` fails if a row's Estonian or English is empty, or if the placeholders
  differ between languages.

Log lines, file names, settings keys and the JMdict licence text stay in English and are
**not** in this file.

How it works: `Strings.def` is included twice, each time with a different definition of
the `X` macro. Once it produces the `enum class Str` (`I18n.hpp`), and once the table of
texts (`I18n.cpp`). A row can't be in one and missing from the other. This is called the
**X-macro** technique.

**Adding a language** is bigger: every row gets a fourth column. Add the language to
`enum class Lang` (the column order) and to `kCodes` / `kNames` in `I18n.cpp`, give it a
rule in `pluralForm()`, and give the `X` macro a fourth parameter in both places that
define it. The Language screen lists every language in the enum automatically.

## The Home menu

`actions::home(App&)` in `src/screens/Actions.cpp` has a table:

```cpp
static const Entry kEntries[] = {
    {Str::HomeDictionary, 'd', openDictionary},
    {Str::HomeLibrary,    'l', openLibrary},
    ...
};
```

Each row is the text, the shortcut key and the function to call. Reorder the rows to
reorder the menu, and change the letter to change the shortcut. A new entry needs a
`Str::` text and an `open…(App&)` function. The texts are filled in by the screen's
rebuild function each time Home comes back into view, which is also where a label can
depend on the app's state: Update dictionary reads Download dictionary while no
dictionary is open.

## Adding a screen

Take a small screen as the model: `TouchTestScreen.hpp` is one header, about 60 lines.

1. Make `src/screens/MyScreen.hpp` (and a `.cpp` if it grows) with a class derived from
   `Screen`. Implement at least `title()`, `onKey()` and `draw()`.
2. In `onKey()`, call `app.pop()` for Backspace and `app.redraw()` whenever what you
   show changed (chapter 01).
3. Draw only inside the `area` you are given, with `draw::text()` (for the font
   fallback) and the colours from `theme::`.
4. Add `void openMyScreen(App&)` to `Actions.hpp` / `.cpp`, and call it from a menu
   entry or a key in another screen.
5. Put every text in `Strings.def`.

If the screen is just a list to pick from, you don't need a class at all. Build a
`ListScreen` with a lambda, as `openFeeds()` and `openLanguage()` do. `setKeyHandler()`
adds keys, `setHints()` sets the bottom bar, and `setRebuild()` refills the items every
time the screen comes back to the top.

## Settings

`/tdeckjisho/config/settings.txt` is a plain `key=value` file. You can edit it on the PC
while the card is out.

| Key | Default | Set by | Meaning |
|---|---|---|---|
| `lang` | `en` | Settings → Language | `et`, `en` or `ja` |
| `font` | `16` | `f` in the reader | body text size: 12, 16, 20 |
| `ruby` | `1` | `r` in the reader | furigana shown |
| `dict_mode` | `0` | ← → in the search | 0 auto, 1 Japanese→English, 2 English→Japanese |
| `brightness` | `100` | Home → Settings | full backlight level in percent (5–100); dimming goes to half of it |
| `sleep_s` | `120` | Home → Settings | seconds without input before the screen goes off; it dims at ¾ of this |
| `wifi_off_s` | `60` | Home → Settings | seconds of looking for a network before the radio is switched off; 0 = never |
| `last_url` | `https://` | the web prompt | the address typed last |
| `jmdict_url` | the EDRDG's | Update dictionary | where to download JMdict from, e.g. `http://192.168.1.10:8000/JMdict_e.gz` for a server on your PC |

The settings screen (Home → Settings, `c`) is a table, `settingDefs()` in
`src/screens/Actions.cpp`: one row per setting with its key, default and the values
← → step through. To put a new number setting on that screen, add a row there and its
label to `Strings.def`. Below the values, `kLinks` lists rows that open a screen of their
own (Language, Wi-Fi, Touch test), each with a shortcut key. A value typed by hand that isn't in the list still works; the
screen starts stepping from the nearest choice above it.

To add a setting, read it with `app.settings().getInt("key", default)` or `get()`,
change it with `set()` / `setInt()`, and call `app.settings().save()`. The save writes
`settings.txt.tmp` first and renames it over the old file, so a power cut can't leave a
half-written file. Saving costs a few SD writes, so don't save every frame.

Reading positions go to a separate file, `positions.txt`, with the same format.

## Smaller knobs

| What | Where |
|---|---|
| Time zone (the clock) | **two places**: `setenv("TZ", …)` in `hal/tdeck/Board.cpp` and `configTzTime(…)` in `hal/tdeck/Net.cpp`. A POSIX TZ string; Estonia's is `EET-2EEST,M3.5.0/3,M10.5.0/4` |
| NTP servers | `configTzTime(…)` in `hal/tdeck/Net.cpp` |
| Trackball sensitivity | `kTrackballThreshold` in `hal/tdeck/Input.cpp` |
| How long the search waits after a key | `kDebounceMs` in `screens/SearchScreen.cpp` |
| Number of search results | `kMaxResults` in `screens/SearchScreen.cpp` |
| Biggest book / web page | `kMaxBookBytes` / `kMaxWebBytes` in `screens/Actions.cpp` |
| Biggest picture height | `kMaxImageH` in `text/Layout.hpp` |
| Memory for decoded pictures | the `ImageCache` budget (1 MB) in `ui/ImageCache.hpp` |
| The folder on the SD card | `src/core/Paths.hpp`. Every path in the program comes from there |
| Default news feeds | `sdcard/tdeckjisho/config/feeds.txt`, copied to the card by `tools/make_sd_bundle.sh` |
| The `User-Agent` sent to servers | `hal/tdeck/Net.cpp` and `hal/sdl/Net.cpp` |
