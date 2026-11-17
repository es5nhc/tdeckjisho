// ============================================================================
//  hal/sdl/Input.cpp – PC keyboard -> the same key codes the T-Deck produces
// ============================================================================
//
//  Mapping
//      letters, digits, symbols   -> ASCII (what the T-Deck keyboard sends)
//      Enter                      -> key::Enter
//      Backspace, Escape          -> key::Backspace
//      arrow keys                 -> trackball directions
//      Tab                        -> trackball click (key::Select)
//      F12                        -> save a PNG screenshot to ./screenshots/
//
//  THREADS: SDL delivers events in the main thread (see sdl_main.cpp), but
//  hal::pollKey() is called from our app thread. Two threads touching one
//  std::deque at the same time = data race = random corruption. A std::mutex
//  makes each access exclusive; std::lock_guard locks it in its constructor
//  and unlocks in its destructor (RAII again), so we cannot forget to unlock.
//  Rust would force this on you with Mutex<VecDeque<i32>>.
//
//  AUTOMATION: set JISHO_KEYS to a key script and the app types it by itself,
//  e.g.  JISHO_KEYS='d{wait}taberu{wait}{shot}{enter}{wait}{shot}{quit}'
//  Tokens: {up} {down} {left} {right} {enter} {bs} {tab} {wait} {shot} {quit}
//  Used to produce the screenshots in docs/img/, and handy for regression tests.
// ============================================================================

#include <SDL.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>

#include <LovyanGFX.hpp>   // full class definition, needed to call createPng()

#include "hal/Hal.hpp"
#include "hal/Keys.hpp"

namespace {

std::mutex g_mutex;
std::deque<int> g_keys;                    // protected by g_mutex
std::atomic<bool> g_screenshot{false};     // atomic: safe without a mutex
std::deque<std::pair<int, int>> g_taps;    // mouse clicks as screen positions; protected by g_mutex

// Called by SDL for EVERY event, in the thread that pumps events.
// Must be fast and must only touch thread-safe data.
int eventWatch(void*, SDL_Event* ev) {
    int k = key::None;
    if (ev->type == SDL_TEXTINPUT) {
        // TEXTINPUT gives the character the OS keyboard layout produced
        // (shift, AltGr...). We only accept ASCII, like the T-Deck keyboard.
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const char* p = ev->text.text; *p; ++p)
            if (key::isPrintable(static_cast<uint8_t>(*p))) g_keys.push_back(*p);
        return 0;
    }
    if (ev->type == SDL_MOUSEBUTTONDOWN && ev->button.button == SDL_BUTTON_LEFT) {
        // A click arrives in WINDOW pixels; the window shows the 320x240 screen scaled (2x, or
        // whatever zoom you chose), so scale back to screen coordinates.
        int ww = 0, wh = 0;
        if (SDL_Window* w = SDL_GetWindowFromID(ev->button.windowID)) SDL_GetWindowSize(w, &ww, &wh);
        if (ww > 0 && wh > 0) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_taps.emplace_back(ev->button.x * 320 / ww, ev->button.y * 240 / wh);
        }
        return 0;
    }
    if (ev->type != SDL_KEYDOWN) return 0;
    switch (ev->key.keysym.sym) {
        case SDLK_RETURN:
        case SDLK_KP_ENTER:  k = key::Enter; break;
        case SDLK_BACKSPACE:
        case SDLK_ESCAPE:    k = key::Backspace; break;
        case SDLK_UP:        k = key::Up; break;
        case SDLK_DOWN:      k = key::Down; break;
        case SDLK_LEFT:      k = key::Left; break;
        case SDLK_RIGHT:     k = key::Right; break;
        case SDLK_TAB:       k = key::Select; break;
        case SDLK_F12:       g_screenshot = true; return 0;
        default:             return 0;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_keys.push_back(k);
    return 0;
}

void saveScreenshot() {
    static int n = 0;
    std::filesystem::create_directories("screenshots");
    char name[64];
    snprintf(name, sizeof name, "screenshots/shot-%02d.png", ++n);
    size_t len = 0;
    // LovyanGFX can encode whatever is on the display as PNG.
    void* png = hal::display().createPng(&len, 0, 0, 320, 240);
    if (!png) return;
    if (FILE* f = std::fopen(name, "wb")) {
        std::fwrite(png, 1, len, f);
        std::fclose(f);
        hal::log("[sdl] saved %s\n", name);
    }
    free(png);
}

// ---- the automation script (JISHO_KEYS) -----------------------------------
std::string g_script;
size_t g_scriptPos = 0;
uint32_t g_nextScriptAt = 1500;   // give the app time to start

int nextScriptKey() {
    if (g_scriptPos >= g_script.size() || hal::millis() < g_nextScriptAt) return key::None;
    g_nextScriptAt = hal::millis() + 60;
    if (g_script[g_scriptPos] != '{') return g_script[g_scriptPos++];

    const size_t end = g_script.find('}', g_scriptPos);
    const std::string tok = g_script.substr(g_scriptPos + 1, end - g_scriptPos - 1);
    g_scriptPos = end + 1;
    if (tok == "up") return key::Up;
    if (tok == "down") return key::Down;
    if (tok == "left") return key::Left;
    if (tok == "right") return key::Right;
    if (tok == "enter") return key::Enter;
    if (tok == "bs") return key::Backspace;
    if (tok == "tab") return key::Select;
    if (tok == "wait") g_nextScriptAt = hal::millis() + 1200;
    if (tok == "shot") saveScreenshot();
    if (tok == "quit") std::exit(0);
    // {tap:160,120} = a click at that screen position (for scripted runs and screenshots)
    if (tok.rfind("tap:", 0) == 0) {
        int x = 0, y = 0;
        if (std::sscanf(tok.c_str() + 4, "%d,%d", &x, &y) == 2) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_taps.emplace_back(x, y);
        }
    }
    return key::None;
}

}  // namespace

namespace hal {

void inputBegin() {
    SDL_AddEventWatch(eventWatch, nullptr);
    if (const char* s = std::getenv("JISHO_KEYS")) g_script = s;
}

int pollKey() {
    if (g_screenshot.exchange(false)) saveScreenshot();
    if (int k = nextScriptKey(); k != key::None) return k;   // if-with-initializer (C++17)

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_keys.empty()) return key::None;
    const int k = g_keys.front();
    g_keys.pop_front();
    return k;
}

bool pollTap(int& x, int& y) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_taps.empty()) return false;
    x = g_taps.front().first;
    y = g_taps.front().second;
    g_taps.pop_front();
    return true;
}

const char* touchStatus() { return "PC: mouse click = tap"; }

}  // namespace hal
