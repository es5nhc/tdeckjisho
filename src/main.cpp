// ============================================================================
//  main.cpp – the same 15 lines run on the T-Deck and on the PC
// ============================================================================
//
//  Arduino convention: the framework calls setup() once, then loop() forever.
//  On the PC, hal/sdl/sdl_main.cpp provides a main() that does the same.
//
//  Everything interesting happens in App (src/ui/App.cpp).
// ============================================================================

#include "hal/Hal.hpp"
#include "ui/App.hpp"

#if defined(ARDUINO)
#include <Arduino.h>
// The Arduino loop() runs in a FreeRTOS task with an 8 KB stack by default.
// TLS (HTTPS) handshakes plus our parsers need more. This macro (from the
// Arduino-ESP32 core) sets the loop task's stack size to 16 KB.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);
#endif

// A global object: constructed before setup() runs. Its constructor must not
// touch hardware (hardware is not initialised yet) – that's why App has a
// separate begin().
static App app;

void setup() {
    hal::begin();    // power, display, keyboard, SD card
    app.begin();     // settings, dictionary, Wi-Fi, home screen
}

void loop() {
    app.tick();
    hal::sleepMs(5); // be nice: let the Wi-Fi/other tasks run, save power
}
