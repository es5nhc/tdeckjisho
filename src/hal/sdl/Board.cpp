// ============================================================================
//  hal/sdl/Board.cpp – display, time and logging on the PC
// ============================================================================

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <thread>

#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/sdl/Panel_sdl.hpp>

#include "hal/Hal.hpp"

namespace hal {
void inputBegin();   // defined in hal/sdl/Input.cpp
}

namespace {

// The same idea as LilyGo_T_Deck in hal/tdeck/Board.cpp: an LGFX_Device that
// owns its panel. 320x240, shown at 2x so it is readable on a PC monitor.
class SdlDisplay : public lgfx::LGFX_Device {
    lgfx::Panel_sdl _panel;

public:
    SdlDisplay() {
        auto cfg = _panel.config();
        cfg.memory_width = cfg.panel_width = 320;
        cfg.memory_height = cfg.panel_height = 240;
        _panel.config(cfg);
        _panel.setScaling(2, 2);
        _panel.setWindowTitle("TDeckJisho (SDL)  –  Tab = trackball click, Esc = BS, F12 = screenshot");
        setPanel(&_panel);
    }
};

// A function-local static is created the first time the function runs and
// lives until the program ends ("Meyers singleton"). This avoids the "static
// initialisation order fiasco" of plain globals in different .cpp files.
SdlDisplay& sdlDisplay() {
    static SdlDisplay d;
    return d;
}

}  // namespace

namespace hal {

void begin() {
    sdlDisplay().init();
    sdlDisplay().setRotation(0);
    sdlDisplay().fillScreen(TFT_BLACK);
    inputBegin();
    log("[hal] SDL simulator, SD folder: %s\n", storageReady() ? "found" : "MISSING (./sdcard)");
}

uint32_t millis() {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now() - t0).count());
}

void sleepMs(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

void log(const char* fmt, ...) {
    // `...` = C variadic arguments. va_list walks them; vprintf consumes them.
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::fflush(stdout);
}

lgfx::LGFX_Device& display() { return sdlDisplay(); }

// The PC window has no backlight. Your main project fakes it with
// SDL_SetWindowOpacity() – but that is an SDL *window* function, and SDL
// window functions may only be called from the main thread. Our code runs in
// a second thread (see sdl_main.cpp), so that call is a data race: it
// happened to work on your desktop and segfaults under Xvfb.
// Drawing IS safe from our thread (Panel_sdl copies the pixels over), so
// "backlight off" = paint the screen black. App redraws everything on wake.
void setBacklight(uint8_t) {}               // a PC window has no backlight to dim

void displayOff() { sdlDisplay().fillScreen(TFT_BLACK); }
void displayOn() {}                          // App redraws a full frame anyway

int batteryPercent() { return -1; }          // a PC has no battery we care about

bool timeValid() { return true; }            // the PC clock is always set

MemInfo memInfo() { return MemInfo{}; }      // not meaningful on a PC

}  // namespace hal
