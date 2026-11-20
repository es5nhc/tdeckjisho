#pragma once
// ============================================================================
//  App.hpp – owns the screen stack, the services, and the main loop step
// ============================================================================
//
//  One App object exists (created in main.cpp). Each loop() iteration calls
//  App::tick(), which:
//
//     1. reads all pending keys and hands them to the top screen
//     2. lets the top screen do background work (update)
//     3. applies queued navigation (push/pop) – see "deferred" below
//     4. turns the backlight off after a period of inactivity
//     5. if anything changed, redraws EVERYTHING into an off-screen sprite
//        and copies it to the display in one go
//
//  Why draw into a sprite (an image in RAM) instead of the display?
//    * no flicker: the user never sees a half-drawn frame
//    * screens can simply redraw everything – no "erase the old text" logic
//    * the SPI bus is shared with the SD card. Drawing happens in one short
//      burst (pushSprite), so display traffic never interleaves with SD reads.
//  Cost: 320*240*2 = 150 KB, which lives in PSRAM on the device.
//
//  Why are push()/pop() DEFERRED (queued and applied later)?
//    A screen often pops itself from inside its own onKey():
//        void onKey(App& app, int k) { if (k == key::Backspace) app.pop(); }
//    If pop() destroyed the screen immediately, onKey would continue running
//    on a deleted object ("use after free") – undefined behaviour, a classic
//    C++ crash that may only show up sometimes. Queueing the operation and
//    applying it after onKey() returns makes this impossible. (In Rust the
//    borrow checker would refuse to compile the unsafe version.)
// ============================================================================

#include <memory>
#include <string>
#include <vector>

#include <LovyanGFX.hpp>

#include "core/Settings.hpp"
#include "dict/Dictionary.hpp"
#include "hal/Hal.hpp"
#include "ui/Screen.hpp"

class App {
public:
    void begin();    // call once from setup()
    void tick();     // call from every loop()

    // ---- navigation (all deferred) --------------------------------------
    // std::unique_ptr = "I own this object; it is deleted when I am".
    // Ownership moves INTO the App's stack. (Rust: Box<dyn Screen>.)
    void push(std::unique_ptr<Screen> s);
    void pop();
    void replace(std::unique_ptr<Screen> s);   // pop + push in one step

    void redraw() { _dirty = true; }
    void toast(const std::string& msg, uint32_t ms = 2500);

    // ---- services --------------------------------------------------------
    Dictionary& dict() { return _dict; }
    Settings& settings() { return _settings; }
    Settings& positions() { return _positions; }   // reading positions per file
    Settings& quizProgress() { return _quiz; }     // quiz answer streaks per question
    uint32_t now() const { return _now; }

    // ---- power -----------------------------------------------------------
    // "Full" brightness 0..255, from the setting `brightness` (percent). Waking up and
    // un-dimming go back to THIS, not to 255: a dimmer screen is the cheapest battery saving.
    uint8_t fullBacklight() const;
    void applyBacklight() { hal::setBacklight(fullBacklight()); }   // after changing the setting
    void applyTimeZone();                                            // likewise, `tz_min`

private:
    struct Op {
        enum Kind { Push, Pop, Replace } kind;
        std::unique_ptr<Screen> screen;
    };

    void applyPending();
    bool wakeUp();          // a key or tap arrived; true = it only woke the screen (swallow it)
    void checkWifiGiveUp(); // radio off after `wifi_off_s` seconds without a network
    void startAutoConnect(); // scan for saved networks (at boot, after a lost connection)
    void pollAutoConnect();  // ...and join the strongest one when the scan is done
    void render();
    void drawTopBar(lgfx::LovyanGFX& g);
    void drawBottomBar(lgfx::LovyanGFX& g);
    std::string statusSignature() const;   // changes when the status bar must redraw

    std::vector<std::unique_ptr<Screen>> _stack;
    std::vector<Op> _pending;

    lgfx::LGFX_Sprite _canvas;
    Dictionary _dict;
    Settings _settings;
    Settings _positions;
    Settings _quiz;

    bool _dirty = true;
    uint32_t _now = 0;
    uint32_t _lastInput = 0;
    bool _asleep = false;
    bool _dimmed = false;
    std::string _toast;
    uint32_t _toastUntil = 0;
    uint32_t _lastStatusCheck = 0;
    std::string _lastStatus;
    hal::WifiState _wifiState = hal::WifiState::Off;   // as seen at the last tick
    uint32_t _wifiTryingSince = 0;                     // when it last started Connecting
    bool _autoScanning = false;   // an auto-connect scan is running
    bool _autoUsed = false;       // auto-connect already tried since the last Connected
};
