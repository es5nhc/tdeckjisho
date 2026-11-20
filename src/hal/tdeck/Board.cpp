// ============================================================================
//  hal/tdeck/Board.cpp – power-up, display, time, battery on the T-Deck
// ============================================================================
//
//  T-Deck pins used in this file (from LilyGO's schematic / utilities.h):
//
//    GPIO 10   BOARD_POWERON  – must be HIGH or keyboard, SD, radio, display
//                               peripherals stay unpowered
//    GPIO 40/41/38  SPI SCK / MOSI / MISO – SHARED by display, SD card, LoRa
//    GPIO 12   display CS      GPIO 11  display DC     GPIO 42  backlight
//    GPIO 39   SD card CS      GPIO 9   LoRa radio CS
//    GPIO 4    battery voltage (through a 1:2 resistor divider)
//
//  SHARED SPI BUS: three chips listen on the same SCK/MOSI/MISO wires. Each
//  has its own chip-select (CS) line; a chip only listens while its CS is
//  LOW. Before we start anything, ALL CS lines must be HIGH – otherwise e.g.
//  the SD card would interpret display traffic as commands and lock up.
// ============================================================================

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <sys/time.h>

#include <cstdarg>
#include <cstdlib>   // std::abs
#include <ctime>

#include <LovyanGFX.hpp>

#include "core/Battery.hpp"
#include "hal/Hal.hpp"

namespace hal {
void inputBegin();       // hal/tdeck/Input.cpp
void storageBegin();     // hal/tdeck/Storage.cpp
}

namespace {

constexpr int kPinPowerOn = 10;
constexpr int kPinSdCs = 39;
constexpr int kPinRadioCs = 9;
constexpr int kPinBattery = 4;
constexpr int kPinBacklight = 42;

// ---- the backlight ---------------------------------------------------------
// The T-Deck's backlight chip is NOT dimmed by PWM. It has 16 levels and counts PULSES
// (LilyGO's T-Deck UnitTest example, setBrightness()):
//
//    pin HIGH            on, at the current level (switched on from off: level 16)
//    short LOW -> HIGH   one level down; below 1 it wraps around to 16
//    LOW for > ~3 ms     off
//
// PWM (LovyanGFX's Light_PWM, which this file used before) is 44,000 such pulses a
// second: dimming made the level spin round, and a following "full" (a constant HIGH)
// froze it wherever it happened to stop. After the dim-then-sleep cycle that was often
// about half brightness. So we count the pulses ourselves, like LilyGO does.
constexpr int kBacklightSteps = 16;
int gBacklightLevel = 0;        // where the chip is now: 0 = off, 1..16

void backlightSteps(int value) {
    if (value <= 0) {
        digitalWrite(kPinBacklight, LOW);
        delay(3);                                // long enough to switch the chip off
        gBacklightLevel = 0;
        return;
    }
    if (value > kBacklightSteps) value = kBacklightSteps;
    if (gBacklightLevel == 0) {                  // from off, the chip starts at full
        digitalWrite(kPinBacklight, HIGH);
        gBacklightLevel = kBacklightSteps;
        delayMicroseconds(30);
    }
    // Pulses only go down, wrapping from 1 to 16: from 3 up to 16 is 3 pulses (2, 1, 16).
    const int pulses = (kBacklightSteps + gBacklightLevel - value) % kBacklightSteps;
    for (int i = 0; i < pulses; ++i) {
        digitalWrite(kPinBacklight, LOW);
        digitalWrite(kPinBacklight, HIGH);
    }
    gBacklightLevel = value;
}

// The display driver configuration – the same as LilyGo_T_Deck in your main
// project (src/platform/tdeck/Display_Tdeck.hpp), with comments.
class TDeckDisplay : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789 _panel;   // the display controller chip
    lgfx::Bus_SPI _bus;          // how we talk to it
    // No lgfx::Light: the backlight is driven by backlightSteps() above, not by PWM.

public:
    TDeckDisplay() {
        {
            auto cfg = _bus.config();
            cfg.spi_host = SPI2_HOST;   // the ESP32-S3's general-purpose SPI peripheral
            cfg.spi_mode = 0;
            cfg.freq_write = 40000000;  // 40 MHz: the ST7789 is happy, SD shares fine
            cfg.freq_read = 16000000;
            cfg.pin_sclk = 40;
            cfg.pin_mosi = 41;
            cfg.pin_miso = 38;
            cfg.pin_dc = 11;            // Data/Command select line of the ST7789
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs = 12;
            cfg.pin_rst = -1;           // no reset pin wired
            cfg.pin_busy = -1;
            cfg.memory_width = cfg.panel_width = 240;    // native portrait...
            cfg.memory_height = cfg.panel_height = 320;  // ...setRotation(1) = landscape
            cfg.invert = true;          // this panel needs colour inversion
            cfg.rgb_order = false;
            cfg.bus_shared = true;      // IMPORTANT: SD card is on the same bus,
                                        // release the bus after every transaction
            _panel.config(cfg);
        }
        setPanel(&_panel);
    }
};

TDeckDisplay& tdeckDisplay() {
    static TDeckDisplay d;   // created on first use, see hal/sdl/Board.cpp
    return d;
}

}  // namespace

namespace hal {

void begin() {
    Serial.begin(115200);
    // USB-CDC: wait up to 2 s for the serial monitor so early logs aren't lost.
    const uint32_t t0 = ::millis();
    while (!Serial && ::millis() - t0 < 2000) delay(10);

    // 1. Power the peripherals and park every SPI chip-select HIGH.
    pinMode(kPinPowerOn, OUTPUT);
    digitalWrite(kPinPowerOn, HIGH);
    pinMode(kPinSdCs, OUTPUT);
    digitalWrite(kPinSdCs, HIGH);
    pinMode(kPinRadioCs, OUTPUT);
    digitalWrite(kPinRadioCs, HIGH);
    // The backlight chip may still be on at a level left by Launcher: switch it off,
    // so it is in a known state (backlightSteps() counts from there).
    pinMode(kPinBacklight, OUTPUT);
    backlightSteps(0);
    delay(500);   // the keyboard's ESP32-C3 needs time to boot after power-on

    // 2. Display (LovyanGFX initialises the SPI bus here).
    tdeckDisplay().init();
    tdeckDisplay().setRotation(1);   // landscape, keyboard below
    tdeckDisplay().fillScreen(TFT_BLACK);
    setBacklight(255);

    // 3. SD card: it joins the SPI bus the display has just set up.
    storageBegin();
    // 4. Keyboard and trackball.
    inputBegin();

    // 5. The time zone is set later, by App::begin() from the settings (hal::setTimeZone).

    analogReadResolution(12);
    log("[hal] T-Deck ready, PSRAM %u KB free\n",
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

// `::millis()` – the leading :: means "the global one" (Arduino's), not
// hal::millis, which would call itself forever.
uint32_t millis() { return ::millis(); }

void sleepMs(uint32_t ms) { delay(ms); }

void log(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    Serial.print(buf);
}

lgfx::LGFX_Device& display() { return tdeckDisplay(); }

// 0..255 onto the chip's 16 steps: 0 is off, anything else at least step 1. 255 -> 16,
// the dim level 40 -> 3.
void setBacklight(uint8_t level) {
    backlightSteps(level == 0 ? 0 : (level * kBacklightSteps + 254) / 255);
}

void displayOff() {
    tdeckDisplay().fillScreen(TFT_BLACK);   // no static image left in the panel
    setBacklight(0);
    tdeckDisplay().sleep();                 // the ST7789 "sleep in" command
}

void displayOn() {
    tdeckDisplay().wakeup();                // "sleep out" (the panel needs ~5 ms)
    setBacklight(255);
}

int batteryMillivolts() {
    // One ADC reading is noisy (tens of mV), and the backlight and Wi-Fi make the
    // voltage itself wobble. So: at most one new measurement a second, each the
    // average of 16 readings, then an exponential moving average over ~8 s.
    static uint32_t lastMs = 0;
    static int32_t avg8 = -1;              // the smoothed value x 8 (keeps the fractions)
    const uint32_t now = millis();
    if (avg8 >= 0 && now - lastMs < 1000) return avg8 / 8;
    lastMs = now;

    uint32_t sum = 0;
    for (int i = 0; i < 16; ++i) sum += analogReadMilliVolts(kPinBattery);
    // The 1:2 divider (2 x 100 kΩ) halves the voltage so 4.2 V fits the ADC range.
    // analogReadMilliVolts() uses the chip's factory calibration, but on the T-Deck it
    // still reads low, so we multiply by 2.11 instead of 2. The factor comes from
    // Meshtastic's T-Deck variant: ADC_MULTIPLIER 2.11, "2.0 + 10% for correction of
    // display undervoltage" (variants/esp32s3/t-deck/variant.h, commit 6d41e27, see
    // docs/04-hardware.md). Integer maths: x 211 / 100.
    const int32_t mv = static_cast<int32_t>(sum / 16) * 211 / 100;
    if (mv < 1000) {                       // no battery connected
        avg8 = -1;
        return -1;
    }
    // First reading, or a jump too big to be discharge (battery swapped, USB
    // plugged in): start again from the new value instead of drifting towards it.
    if (avg8 < 0 || std::abs(mv - avg8 / 8) > 300) avg8 = mv * 8;
    else avg8 += mv - avg8 / 8;            // new = old + (sample - old) / 8, in x8 units
    return avg8 / 8;
}

int batteryPercent() {
    const int mv = batteryMillivolts();
    return mv < 0 ? -1 : battery::percentFromMillivolts(mv);
}

void setTimeZone(const std::string& posixTz) {
    // The C library reads TZ from the environment; tzset() makes it look again.
    // hal/tdeck/Net.cpp passes the same variable on to SNTP.
    setenv("TZ", posixTz.c_str(), 1);
    tzset();
}

bool timeValid() {
    // Before NTP sync the ESP32 clock starts at 1970. Anything after 2024
    // means somebody set it.
    return std::time(nullptr) > 1704067200;   // 2024-01-01
}

MemInfo memInfo() {
    MemInfo m;
    m.freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    m.freePsram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    m.totalPsram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    return m;
}

}  // namespace hal
