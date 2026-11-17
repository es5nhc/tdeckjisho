// ============================================================================
//  hal/sdl/sdl_main.cpp – a main() that imitates the Arduino runtime
// ============================================================================
//
//  LovyanGFX's SDL panel needs the MAIN thread for window/event handling
//  (SDL requires that on many systems). Panel_sdl::main() therefore:
//    * starts OUR code (setup + loop) in a second thread, and
//    * keeps pumping SDL events and refreshing the window in the main thread.
//  When the window is closed, *running becomes false and our loop exits.
//
//  Consequence: keyboard events arrive in a DIFFERENT thread than the one
//  that reads them. hal/sdl/Input.cpp handles that with a mutex.
// ============================================================================

#include <lgfx/v1/platforms/sdl/Panel_sdl.hpp>

void setup();
void loop();

// LeakSanitizer reports memory still allocated when the program ends, and then makes the
// program exit with an error. SDL and X11 keep a few bytes on purpose until exit; these
// lines tell the checker to ignore leaks whose allocation went through those libraries,
// so a leak in OUR code is still reported. (The sanitizer runtime looks for a function
// with exactly this name; `extern "C"` keeps the name unmangled.)
extern "C" const char* __lsan_default_suppressions() {
    return "leak:libSDL2\n"
           "leak:libX11\n";
}

static int userMain(bool* running) {
    setup();
    while (*running) loop();
    return 0;
}

int main(int, char**) {
    // Panel_sdl has built-in shortcuts: 'r'/'l' rotate the window and '1'-'6'
    // change the zoom – when pressed WITHOUT modifiers by default! That is
    // fatal for a program where you type "ringo". Require Ctrl+Alt instead.
    lgfx::Panel_sdl::setShortcutKeymod(static_cast<SDL_Keymod>(KMOD_LCTRL | KMOD_LALT));
    return lgfx::Panel_sdl::main(userMain);
}
