// ============================================================================
//  hal/tdeck/Input.cpp – keyboard and touchscreen (I2C), trackball (GPIO interrupts)
// ============================================================================
//
//  KEYBOARD: a separate ESP32-C3 scans the key matrix and acts as an I2C
//  device at address 0x55 (SDA 18, SCL 8). Reading one byte returns the last
//  key pressed as ASCII, or 0 if none. (Same as your main project.)
//
//  TRACKBALL: four hall sensors. Rolling the ball produces PULSES on
//      GPIO 3 = up   GPIO 15 = down   GPIO 1 = left   GPIO 2 = right
//  and pressing it pulls GPIO 0 LOW (GPIO 0 is also the BOOT button).
//  (Pin numbers as in LilyGO's T-Deck examples and Meshtastic's t-deck
//   variant – double-check against utilities.h if a direction seems wrong.)
//
//  Pulses are short. If we only looked at the pins once per loop, we would
//  miss most of them. So we use INTERRUPTS: the CPU calls a tiny function the
//  instant a pin changes, and that function just increments a counter.
//  pollKey() later turns counters into key events.
//
//  Interrupt rules (they run "in between" any two instructions of loop()):
//    * keep them tiny, no Serial, no malloc, no delay
//    * IRAM_ATTR: place the code in internal RAM so it runs even while the
//      flash chip is busy
//    * shared variables must be `volatile` (the compiler must re-read them
//      every time, they can change behind its back)
// ============================================================================

#include <Arduino.h>
#include <Wire.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#include "hal/Hal.hpp"
#include "hal/Keys.hpp"

namespace {

constexpr uint8_t kKeyboardAddr = 0x55;
constexpr int kKeyboardSda = 18;
constexpr int kKeyboardScl = 8;

constexpr int kPinUp = 3, kPinDown = 15, kPinLeft = 1, kPinRight = 2, kPinClick = 0;

// How many pulses make one key event. The ball is very sensitive; 1 makes
// lists jump around, 3 feels deliberate. Tune to taste.
constexpr int kTrackballThreshold = 2;

volatile int16_t g_up = 0, g_down = 0, g_left = 0, g_right = 0;

void IRAM_ATTR isrUp() { g_up = g_up + 1; }
void IRAM_ATTR isrDown() { g_down = g_down + 1; }
void IRAM_ATTR isrLeft() { g_left = g_left + 1; }
void IRAM_ATTR isrRight() { g_right = g_right + 1; }

// Take `count` from a counter safely. Disabling interrupts for two
// instructions guarantees the ISR cannot change it between read and write.
bool consume(volatile int16_t& counter) {
    bool fire = false;
    noInterrupts();
    if (counter >= kTrackballThreshold) {
        counter = counter - kTrackballThreshold;
        fire = true;
    }
    interrupts();
    return fire;
}

uint32_t g_lastKbPoll = 0;
bool g_clickWasDown = false;
uint32_t g_clickChangedAt = 0;


// ---- touchscreen: GT911 on the same I2C bus (and the same `Wire` object) as the keyboard ----------
//
// Two chips on one bus are normal: every transfer starts with the chip's address, and only that
// chip answers. What must NOT happen is two DRIVERS for the bus (e.g. Arduino's Wire for the
// keyboard plus LovyanGFX's own I2C touch driver) – so the touch is read here, through `Wire`,
// from the same loop as the keyboard. LilyGO's own UnitTest example does exactly this.
//
// GT911 registers are 16-bit addresses, sent high byte first. The ones we need:
//   0x8140  product id, 4 ASCII bytes "911\0"         (to check the chip is there)
//   0x814E  status: bit 7 = a new report is ready, bits 0–3 = number of fingers
//   0x814F  first finger: 8 bytes (id, x lo, x hi, y lo, y hi, size lo, size hi, reserved)
// After reading, write 0 to 0x814E, or the chip never reports again.
//
// Confirmed on real hardware (2026-11-16). Settings → Touch test (p) checks it again if needed.
constexpr int kTouchInt = 16;                    // interrupt pin: we poll instead, so only an input
constexpr uint16_t kGtProductId = 0x8140, kGtStatus = 0x814E, kGtPoint1 = 0x814F;

// Raw panel → screen. These three settings are LilyGO's own (touch.setSwapXY(true) and
// setMirrorXY(false, true) in their examples), confirmed on the device.
constexpr bool kTouchSwapXY = true;
constexpr bool kTouchMirrorX = false;
constexpr bool kTouchMirrorY = true;
constexpr int kScreenW = 320, kScreenH = 240;

uint8_t g_touchAddr = 0;                         // 0 = no touch chip found
uint32_t g_lastTouchPoll = 0;
bool g_fingerDown = false;
char g_touchStatus[64] = "GT911 not probed";

// Read `n` bytes starting at register `reg`.
bool gtRead(uint16_t reg, uint8_t* buf, size_t n) {
    Wire.beginTransmission(g_touchAddr);
    Wire.write(static_cast<uint8_t>(reg >> 8));          // high byte first
    Wire.write(static_cast<uint8_t>(reg & 0xFF));
    if (Wire.endTransmission(false) != 0) return false;  // false = keep the bus ("repeated start")
    if (Wire.requestFrom(g_touchAddr, static_cast<uint8_t>(n)) != n) return false;
    for (size_t i = 0; i < n; ++i) buf[i] = static_cast<uint8_t>(Wire.read());
    return true;
}

bool gtWrite(uint16_t reg, uint8_t value) {
    Wire.beginTransmission(g_touchAddr);
    Wire.write(static_cast<uint8_t>(reg >> 8));
    Wire.write(static_cast<uint8_t>(reg & 0xFF));
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

// The GT911 answers at 0x5D or 0x14 (chosen by the INT pin level at power-up). Try both.
void touchBegin() {
    pinMode(kTouchInt, INPUT);
    for (uint8_t addr : {uint8_t(0x5D), uint8_t(0x14)}) {
        g_touchAddr = addr;
        char id[5] = {};
        if (gtRead(kGtProductId, reinterpret_cast<uint8_t*>(id), 4) && std::strncmp(id, "911", 3) == 0) {
            std::snprintf(g_touchStatus, sizeof g_touchStatus, "GT911 found at 0x%02X", addr);
            return;
        }
    }
    g_touchAddr = 0;
    std::snprintf(g_touchStatus, sizeof g_touchStatus, "GT911 not found (0x5D, 0x14)");
}

void mapTouch(int rawX, int rawY, int& x, int& y) {
    x = rawX;
    y = rawY;
    if (kTouchSwapXY) std::swap(x, y);
    if (kTouchMirrorX) x = kScreenW - x;
    if (kTouchMirrorY) y = kScreenH - y;
    x = std::clamp(x, 0, kScreenW - 1);
    y = std::clamp(y, 0, kScreenH - 1);
}

}  // namespace

namespace hal {

void inputBegin() {
    Wire.begin(kKeyboardSda, kKeyboardScl);
    for (int pin : {kPinUp, kPinDown, kPinLeft, kPinRight}) pinMode(pin, INPUT_PULLUP);
    pinMode(kPinClick, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(kPinUp), isrUp, FALLING);
    attachInterrupt(digitalPinToInterrupt(kPinDown), isrDown, FALLING);
    attachInterrupt(digitalPinToInterrupt(kPinLeft), isrLeft, FALLING);
    attachInterrupt(digitalPinToInterrupt(kPinRight), isrRight, FALLING);
    touchBegin();                       // after Wire.begin: it talks over the same bus
}

bool pollTap(int& x, int& y) {
    // At most every 20 ms (50 times a second is plenty for a finger) – I2C traffic is not free.
    if (g_touchAddr == 0 || ::millis() - g_lastTouchPoll < 20) return false;
    g_lastTouchPoll = ::millis();

    uint8_t status = 0;
    if (!gtRead(kGtStatus, &status, 1)) return false;
    if ((status & 0x80) == 0) return false;     // bit 7 clear: no new report since we last cleared it
    const int points = status & 0x0F;           // low nibble: how many fingers (0–5)

    uint8_t p[8] = {};
    const bool havePoint = points > 0 && points <= 5 && gtRead(kGtPoint1, p, sizeof p);
    gtWrite(kGtStatus, 0);                       // "I have read it": the chip may write the next report

    if (!havePoint) {                            // a report with 0 points = the finger was lifted
        g_fingerDown = false;
        return false;
    }
    if (g_fingerDown) return false;              // still the same touch: one tap per touch-down
    g_fingerDown = true;

    // Bytes of a point: [0] track id, [1..2] x little-endian, [3..4] y little-endian, [5..6] size.
    const int rawX = p[1] | (p[2] << 8);
    const int rawY = p[3] | (p[4] << 8);
    mapTouch(rawX, rawY, x, y);
    std::snprintf(g_touchStatus, sizeof g_touchStatus, "GT911 @0x%02X  raw %d,%d -> %d,%d", g_touchAddr,
                  rawX, rawY, x, y);
    return true;
}

const char* touchStatus() { return g_touchStatus; }

int pollKey() {
    // ---- trackball directions ---------------------------------------------
    if (consume(g_up)) return key::Up;
    if (consume(g_down)) return key::Down;
    if (consume(g_left)) return key::Left;
    if (consume(g_right)) return key::Right;

    // ---- trackball click, debounced -----------------------------------------
    // A mechanical contact "bounces" for a few ms. Accept a change only if
    // the previous change was more than 30 ms ago.
    const bool down = digitalRead(kPinClick) == LOW;
    if (down != g_clickWasDown && ::millis() - g_clickChangedAt > 30) {
        g_clickWasDown = down;
        g_clickChangedAt = ::millis();
        if (down) return key::Select;   // fire on press, not release
    }

    // ---- keyboard: at most every 10 ms (I2C traffic is not free) ----------
    if (::millis() - g_lastKbPoll < 10) return key::None;
    g_lastKbPoll = ::millis();
    if (Wire.requestFrom(kKeyboardAddr, static_cast<uint8_t>(1)) != 1) return key::None;
    const int k = Wire.read();
    return k > 0 ? k : key::None;   // ASCII; Enter = 0x0D, Backspace = 0x08
}

}  // namespace hal
