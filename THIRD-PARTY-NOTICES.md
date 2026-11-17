# Third-party notices

TDeckJisho's own source code is under the MIT licence ([LICENSE](LICENSE)). The firmware
and the SD-card bundle also contain, or are built from, the following. None of them is
stored in this repository except where noted; PlatformIO downloads LovyanGFX, and you
download JMdict yourself.

## Compiled into the firmware

| Component | Licence | Where its full text is |
|---|---|---|
| [LovyanGFX](https://github.com/lovyan03/LovyanGFX) (graphics, panel and touch drivers, JPEG/PNG/QOI decoders, miniz inflate) | FreeBSD (2-clause BSD), with parts under BSD and MIT licences from Adafruit and others | `license.txt` in the LovyanGFX sources (`.pio/libdeps/tdeck/LovyanGFX/license.txt` after a build) |
| lgfxJapanGothicP fonts (in LovyanGFX), derived from IPA Gothic | IPA Font License Agreement v1.0 | `src/lgfx/Fonts/IPA/IPA_Font_License_Agreement_v1.0.txt` in LovyanGFX |
| efont Unicode bitmap fonts (in LovyanGFX) | BSD-style | `src/lgfx/Fonts/efont/COPYRIGHT.txt` in LovyanGFX |
| Arduino-ESP32 core and ESP-IDF (via PlatformIO `espressif32@6.9.0`) | LGPL-2.1 (Arduino core), Apache-2.0 (ESP-IDF) | in the PlatformIO packages |
| `src/hal/tdeck/CaVerify.cpp` replaces the core's `WiFiClientSecure/src/esp_crt_bundle.c` (© Espressif Systems) and follows its structure | Apache-2.0 (the original); our version is MIT | `LICENSE` in the Arduino-ESP32 package |

When you distribute a firmware binary, include these licence texts with it (the IPA Font
License requires it for the font). The Arduino core is linked statically, so under the
LGPL a binary should come with a way to rebuild it: this repository and its build
instructions are that. See
[docs/07-testing-and-release.md](docs/07-testing-and-release.md#releasing).

## Built at compile time, not in the repository

| Component | Licence | Notes |
|---|---|---|
| Root CA certificates (`tools/bundle/cacert.pem`, from your OS's Mozilla bundle) | MPL-2.0 | git-ignored; `tools/make_ca_bundle.py` compiles it into the firmware |

## Dictionary data

| Component | Licence | Notes |
|---|---|---|
| [JMdict](https://www.edrdg.org/wiki/index.php/JMdict-EDICT_Dictionary_Project) (JMdict_e) © Electronic Dictionary Research and Development Group | CC BY-SA 4.0, under the [EDRDG licence](https://www.edrdg.org/edrdg/licence.html) | not in the repository; the converted files on the SD card stay under CC BY-SA 4.0. Credits: `tools/bundle/JMDICT-CREDITS.txt`, the About screen |
| `tools/sample_jmdict.xml` | treated as CC BY-SA 4.0 | a hand-written 52-entry file in JMdict's format, for tests; its glosses follow JMdict's |

## Sample content in `sdcard/`

The sample books, the drawings and the photo (`books/img/sakura*.jpg`) are the author's own
work and are covered by the MIT licence. The photo is a low-resolution copy made for this
software; the licence covers only this copy, not the full-size original.
`config/feeds.txt` only lists the addresses of public news feeds; their content belongs to
their publishers.
