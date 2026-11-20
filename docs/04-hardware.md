# 04 – Hardware

Everything in this chapter lives in `src/hal/tdeck/`. The rest of the program only sees
`src/hal/Hal.hpp`.

## The T-Deck in one table

| Part | Chip | Connected by | Code |
|---|---|---|---|
| Main processor | ESP32-S3, 2 cores at 240 MHz, 8 MB PSRAM, 16 MB flash | – | – |
| Display | ST7789, 320 × 240, RGB565 | SPI (shared) | `Board.cpp` |
| SD card | micro-SD, FAT32 | SPI (shared) | `Storage.cpp` |
| Keyboard | its own ESP32-C3 | I2C, address `0x55` | `Input.cpp` |
| Touch panel | GT911 | I2C (same bus), `0x5D` or `0x14` | `Input.cpp` |
| Trackball | four hall sensors + a button | GPIO interrupts | `Input.cpp` |
| Battery | Li-ion, through a 1:2 voltage divider | ADC | `Board.cpp` |
| LoRa radio | SX1262 | SPI (shared) | not used, but its CS must be parked |

## Pins

| GPIO | Use |
|---|---|
| 10 | `BOARD_POWERON`: must be HIGH, or the keyboard, SD card, display and radio stay unpowered |
| 40 / 41 / 38 | SPI SCK / MOSI / MISO, **shared** by display, SD card and radio |
| 12 | display CS |
| 11 | display DC (data/command) |
| 42 | backlight (pulse-counting chip, not PWM) |
| 39 | SD card CS |
| 9 | radio CS |
| 18 / 8 | I2C SDA / SCL (keyboard and touch) |
| 16 | touch interrupt (read as an input; the code polls instead) |
| 3 / 15 / 1 / 2 | trackball up / down / left / right |
| 0 | trackball press (also the BOOT button) |
| 4 | battery voltage |

These come from LilyGO's schematic and examples. If a trackball direction seems
swapped on your device, check `kPinUp` and friends at the top of `Input.cpp` first.

## Start-up order

`hal::begin()` in `Board.cpp`, step by step, because the order matters:

1. **Serial.** Wait up to 2 s for a serial monitor, so early log lines aren't lost.
2. **Power on** (GPIO 10 HIGH), and set **every** SPI chip-select HIGH: SD and radio.
   Then wait 500 ms, because the keyboard's own processor needs time to boot.
3. **Display.** LovyanGFX sets up the SPI bus here.
4. **SD card.** It joins the bus the display just set up.
5. **Keyboard, trackball and touch.**
6. The ADC for the battery. (The time zone comes later, from the settings:
   `App::begin()` calls `hal::setTimeZone()`.)

## The shared SPI bus

Three chips listen on the same three SPI wires. Each one has its own **chip-select
(CS)** line and only listens while that line is LOW. Two rules follow:

- **Before anything starts, every CS must be HIGH.** Otherwise, for example, the SD card
  takes the display's traffic for commands meant for itself and locks up. That's why
  `hal::begin()` parks the SD and radio CS lines before the display starts.
- **Only one chip talks at a time.** The display is configured with `bus_shared = true`
  and releases the bus after each transfer. The app also draws a whole frame in one
  burst (chapter 01) and decodes pictures into RAM rather than straight onto the panel,
  so an SD read never ends up in the middle of display traffic.

## The display

`TDeckDisplay` in `Board.cpp` is the LovyanGFX configuration: an `ST7789` panel and an
SPI bus at 40 MHz for writing. The panel is natively portrait (240 × 320).
`setRotation(1)` turns it to landscape with the keyboard below. It needs
`invert = true`, or every colour comes out inverted.

**The backlight is not PWM.** Pin 42 goes to a backlight chip with 16 levels that counts
pulses (see LilyGO's T-Deck `UnitTest` example):

| Pin 42 | The chip |
|---|---|
| HIGH | on, at the current level; switched on from off, it starts at 16 |
| a short LOW → HIGH pulse | one level down; below 1 it wraps around to 16 |
| LOW for more than ~3 ms | off |

So `backlightSteps()` in `Board.cpp` remembers the chip's level and sends the number of
pulses to the wanted one, and `hal::setBacklight()` maps 0–255 onto the 16 steps (the
dim level 40 is step 3). The app never asks for more than the `brightness` setting
(Home → Settings, 10–100 %), and dims to half of that, at most 40. At start-up the pin is held LOW first, so the chip is off and
its level known, whatever Launcher left it at. LovyanGFX's `Light_PWM` must not drive
this pin: 44,000 pulses a second make the level spin round, and the display came back
from sleep at a random brightness, often about half.

**Sleep and image retention.** An LCD that shows the same still picture for hours can
keep a faint after-image, even with the backlight off, because the panel still drives
its pixels. So after the idle timeout `hal::displayOff()` paints the panel black, then
switches the backlight off **and** sends the ST7789 its "sleep in" command. Any key or
tap wakes it, and the app redraws a full frame.

The CPU itself keeps running. Real deep sleep would save much more power, but the ESP32
needs a pin to wake it up. The keyboard's interrupt line or the trackball's button could
be that pin, but this isn't done yet.

## The SD card

`storageBegin()` in `Storage.cpp` uses Arduino's `SD` library on the shared bus and tries
20 MHz, then 10, then 4. Some cards and some wiring only work at a lower clock. The log
says which speed worked: `[sd] card OK at 20 MHz`.

- Format the card as **FAT32**. On Arduino-ESP32 2.x, exFAT also works for cards over
  32 GB. Long and UTF-8 file names are fine.
- At most **10 files** can be open at once (`max_files`; the default is 5). The
  dictionary keeps 3 open, and the dictionary builder needs 4 more. If `open()` starts
  failing for no visible reason, this limit is the first suspect.
- Every SD operation costs about a millisecond, whatever its size. The dictionary is
  designed around that (chapter 02). `hal::ioStats()` counts operations, and the
  simulator can imitate the delay (chapter 07).
- FAT can't rename onto an existing name, and can't rename a folder while files in it
  are open. `hal::renamePath()` and `Dictionary::close()` exist because of that.

## Keyboard

The keyboard has its own ESP32-C3 that scans the keys and acts as an I2C device at
address `0x55`. Reading one byte gives the last key as ASCII, or 0 when nothing was
pressed. `pollKey()` asks at most every 10 ms, because I2C traffic isn't free.

Enter arrives as `0x0D` and the `del` key as `0x08`. `src/hal/Keys.hpp` uses exactly
those codes, so keyboard bytes need no translation. The trackball gets codes above
`0xFF`, which can't collide with any byte the keyboard sends.

## Trackball

Rolling the ball produces short **pulses** on four pins. Looking at the pins once per
loop would miss most of them, so each pin has an **interrupt**: the CPU stops whatever
it's doing the moment the pin changes, runs a tiny function that adds one to a counter,
and carries on. `pollKey()` later turns counters into key events.

Interrupt rules, as the code follows them:

- Keep the function tiny: no `Serial`, no `malloc`, no `delay`.
- Mark it `IRAM_ATTR`, so it is placed in internal RAM and can run while the flash chip
  is busy.
- Declare shared counters `volatile`, so the compiler re-reads them every time.
- Read-and-subtract with interrupts briefly off (`consume()`), so an interrupt can't
  change the counter between the read and the write.

**Sensitivity:** `kTrackballThreshold` (2) pulses make one key. 1 makes lists jump
around, 3 feels deliberate. The click is debounced (a change is accepted only 30 ms
after the previous one) and fires on press, not on release.

## Touch

The GT911 sits on the **same I2C bus** as the keyboard and is read through the same
Arduino `Wire` object, from the same loop. Two chips on one bus are normal, because
every transfer starts with the chip's address. What must not happen is two *drivers* for
one bus, for example Arduino's `Wire` for the keyboard plus LovyanGFX's own touch
driver.

| Register | Meaning |
|---|---|
| `0x8140` | product id, `"911"`: checks the chip is there |
| `0x814E` | status: bit 7 = a new report, bits 0–3 = number of fingers |
| `0x814F` | first finger: id, x (2 bytes), y (2 bytes), size |

After reading, the code writes 0 to `0x814E`, or the chip never reports again. A finger
held down counts as **one** tap. The panel's raw coordinates are turned into screen
coordinates with LilyGO's own settings (x and y swapped, one axis mirrored).
**Settings → Touch test** (Home → `c`, then `p`) shows the raw and converted points, so you can check the
mapping on your own device.

## Battery and clock

`batteryMillivolts()` averages 16 ADC readings and multiplies the result by 2.11: 2
because the divider halves the voltage, plus a correction because the T-Deck's ADC reads
low (see below). It takes at most one new measurement a second and smooths over about
eight seconds, so the status bar doesn't flicker when the backlight or Wi-Fi changes the
load. Below 1 V the code assumes no battery is connected.

`batteryPercent()` turns that voltage into a percentage with `battery::percentFromMillivolts()`
(`core/Battery.cpp`). A Li-ion cell's voltage isn't linear in its charge: it falls
quickly from 4.2 V, stays flat around 3.6–3.9 V for most of the charge, then drops
steeply below about 3.5 V. So the code uses a table of eleven points, one every 10 %,
and draws straight lines between them:

| % | 100 | 90 | 80 | 70 | 60 | 50 | 40 | 30 | 20 | 10 | 0 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| mV | 4190 | 4050 | 3990 | 3890 | 3800 | 3720 | 3630 | 3530 | 3420 | 3300 | 3100 |

**Source:** the default LiPo curve (`OCV_ARRAY`) in the Meshtastic firmware's
[`src/power.h`](https://github.com/meshtastic/firmware/blob/6d41e279f1f51bd59f687b9d441c1bf47b1594fc/src/power.h)
(checked 2026-11-18). Meshtastic's T-Deck variant doesn't override it. We took only these
numbers, not any code (Meshtastic is GPL-3.0). We found no curve measured on the T-Deck's
own battery, so this is a generic LiPo curve, not a calibration. The numbers are
*open-circuit* voltages, measured with the cell at rest. In use the voltage sags a little,
and on USB the charger pushes it up, so the percentage reads slightly low in use and too
high while charging.

The 2.11 factor also comes from Meshtastic: their T-Deck variant uses
`ADC_MULTIPLIER 2.11`, "2.0 + 10% for correction of display undervoltage"
([`variants/esp32s3/t-deck/variant.h`](https://github.com/meshtastic/firmware/blob/6d41e279f1f51bd59f687b9d441c1bf47b1594fc/variants/esp32s3/t-deck/variant.h),
same commit). The **About** screen (Home → `a`) shows the measured voltage next to the
percentage, so if your device reads differently from a multimeter on the battery, change
the factor in `Board.cpp`.

The clock starts at 1970 on every boot. Once Wi-Fi connects, SNTP sets it from
`pool.ntp.org`, and the status bar shows the time only after that
(`hal::timeValid()`). The time zone is a POSIX `TZ` string, built from the `tz_min`
setting by `core/TimeZone.cpp` (chapter 06). `configTzTime()` sets `TZ` too, so
`hal/tdeck/Net.cpp` passes it the one already set.

## Launcher and the flash layout

The usual setup is [Launcher](https://github.com/bmorcelli/Launcher) on the T-Deck, with
TDeckJisho installed as an app through Launcher's Wi-Fi interface or from the SD card.

The flash is laid out by `default_16MB.csv` (`board_build.partitions`):

| Partition | Size | Use |
|---|---|---|
| `nvs` | 20 KB | key-value store: the saved Wi-Fi networks (chapter 05) |
| `otadata` | 8 KB | which app slot to boot |
| `app0`, `app1` | 6.25 MB each | two app slots |
| `spiffs` | 3.4 MB | not used by TDeckJisho |
| `coredump` | 64 KB | crash dumps |

The firmware currently uses about 59 % of one app slot, mostly fonts. The build prints
the exact figure as `Flash: [======    ] 58.7%`.

> **Don't run `pio run -e tdeck -t upload` on a T-Deck that runs Launcher.** It writes
> the bootloader, partition table and app, and replaces Launcher. Without Launcher it's
> the normal way to flash.

## Serial logs and crashes

The T-Deck's USB port is the ESP32-S3's built-in USB-serial, so `Serial` output arrives
over the same cable without a separate UART chip (`ARDUINO_USB_CDC_ON_BOOT=1`).

```sh
pio device monitor -e tdeck
```

It only reads the port, so it's safe with Launcher. The log lines are tagged by area:
`[sd]`, `[wifi]`, `[http]`, `[tls]`, `[build]`, `[load]` (how long a file or page
took), `[search]`. Lines like `[123456][E][ssl_client.cpp:37]` come from the Arduino
core and ESP-IDF. `CORE_DEBUG_LEVEL=1` in `platformio.ini` shows only their errors, and
raising it to 3 or 4 shows much more.

If the device crashes, the monitor's `esp32_exception_decoder` filter turns the
addresses in the backtrace into file names and line numbers. It needs the `.elf` that
matches the firmware on the device, `.pio/build/tdeck/tdeckjisho-<version>.elf`. If
you've rebuilt since installing, the decoded lines will be wrong.
