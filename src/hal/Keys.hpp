#pragma once
// ============================================================================
//  Keys.hpp – one key-code vocabulary for the whole application
// ============================================================================
//
//  The T-Deck has two input devices:
//    * the BlackBerry-style keyboard (an ESP32-C3 that sends ASCII over I2C)
//    * the trackball (four direction pulses + a click button)
//  The PC has a normal keyboard.
//
//  The HAL (hal/tdeck/Input.cpp, hal/sdl/Input.cpp) translates all of those
//  into the small set of integers below, so the UI code never needs to know
//  which device a key came from.
//
//  Design choice: plain ASCII codes for "normal" keys (so 'a' == 97, Enter ==
//  13, exactly what the T-Deck keyboard already sends), and values above 0xFF
//  for the trackball. Values above 0xFF can never collide with a byte the
//  keyboard sends.
//
//  Python analogy: this is like a module full of constants. We use `constexpr`
//  so the compiler substitutes the numbers directly – no memory is used.
//  Rust analogy: a set of `pub const KEY_UP: i32 = 0x101;`.
// ============================================================================

namespace key {

constexpr int None      = 0;      // "no key pressed" – returned by hal::pollKey()
constexpr int Backspace = 0x08;   // T-Deck "del" key; Backspace/Escape on the PC
constexpr int Enter     = 0x0D;   // carriage return, what the T-Deck sends

// Trackball (arrow keys and Tab on the PC)
constexpr int Up     = 0x101;
constexpr int Down   = 0x102;
constexpr int Left   = 0x103;
constexpr int Right  = 0x104;
constexpr int Select = 0x105;     // trackball press (GPIO0); Tab on the PC

// `inline` on a function in a header means "the same definition may appear in
// several .cpp files, that is fine". Without it the linker would complain
// about multiple definitions.
inline bool isPrintable(int k) { return k >= 0x20 && k < 0x7F; }

}  // namespace key
