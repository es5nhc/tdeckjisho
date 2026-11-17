# TDeckJisho documentation

These pages explain how TDeckJisho works and where to change it. The code comments
explain the details of each file. These pages are the map that connects them: which
part does what, why it was built that way, and which file to open when you want to
change something.

Read them in order the first time. Each one assumes the ones before it.

| Chapter | What it covers |
|---|---|
| [01 – Architecture](01-architecture.md) | One code base, two programs. The HAL, the main loop, screens, drawing, long jobs, memory, the build |
| [02 – The dictionary](02-dictionary.md) | The files on the SD card, how a search works, word look-up in a text (katakana spellings, counters), building the dictionary on the device |
| [03 – Text and the reader](03-text-and-reader.md) | The one document format, the parsers, line breaking, fonts, furigana, pictures |
| [04 – Hardware](04-hardware.md) | The T-Deck's pins, the shared SPI bus, keyboard, trackball, touch, SD card, power, Launcher, serial logs |
| [05 – Network](05-network.md) | Wi-Fi, saved passwords, HTTP and its safety rules, HTTPS certificates, feeds and the web reader |
| [06 – Customising](06-customising.md) | The version, colours, fonts, texts and translations, menu entries, new screens, settings |
| [07 – Testing and release](07-testing-and-release.md) | Unit tests, sanitizers, the converter comparison, benchmarks, simulator tricks, making a release |

`docs/img/` holds the screenshots used in the README, and `docs/security/` holds two
programs that test the parsers with hostile input (see chapter 07).

## Conventions

- Paths like `src/ui/App.cpp` are relative to the project root. Run every command from
  there too: PlatformIO looks for `platformio.ini` in the current folder.
- "The device" is the T-Deck and "the simulator" is the SDL window on the PC.
- SD card paths start with `/tdeckjisho/`. In the simulator that folder is
  `./sdcard/tdeckjisho/`.
- When a page names a number (a buffer size, a limit, a timeout), the constant is named
  too, so you can find it with a search.
