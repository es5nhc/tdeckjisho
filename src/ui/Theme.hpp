#pragma once
// ============================================================================
//  Theme.hpp – colours, fonts and fixed screen geometry in one place
// ============================================================================
//
//  FONTS. The T-Deck must show Estonian (õ ä ö ü š ž) AND Japanese, and the
//  text should be sans-serif. A coverage survey of LovyanGFX's built-in fonts
//  showed:
//      lgfxJapanGothic(P)  – sans-serif, Japanese + õäöü, but NO š/ž
//      efontJA             – has š/ž, but its Latin letters look typewriter-ish
//  So we use BOTH: lgfxJapanGothicP (P = proportional) for everything, and
//  efontJA as a per-glyph FALLBACK for the few characters Gothic lacks
//  (see ui/Draw.cpp). Gothic has no bold variant – bold is drawn as "fake
//  bold" (twice, 1 px apart).
//  These are bitmap fonts compiled into flash; each size costs a few hundred
//  KB. Alternative for later: .vlw smooth fonts from the SD card with
//  loadFont() – any size, real bold, any coverage.
//
//  COLOURS are RGB565: 16 bits per pixel = 5 bits red, 6 green, 5 blue.
//  That is the native format of the ST7789 panel, so no conversion is needed
//  when the sprite is copied to the screen.
// ============================================================================

#include <cstdint>

#include <LovyanGFX.hpp>

namespace theme {

// Pack 8-bit-per-channel RGB into RGB565.  constexpr = computed at compile
// time, so `kBg` below is just the number 0x1082 in the binary.
//   rrrrrggg gggbbbbb
constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// A calm dark palette with a vermilion (朱色) accent.
constexpr uint16_t kBlackColour = rgb(0,0,0);
constexpr uint16_t kWhiteColour = rgb(255,255,255);
constexpr uint16_t kBlueColour  = rgb(0,0,255);
constexpr uint16_t kBg          = kWhiteColour;
constexpr uint16_t kBgBar       = kBlueColour;     // status / hint bars
constexpr uint16_t kText        = kBlackColour;  // warm off-white, easier on the eyes
constexpr uint16_t kDim         = rgb(140, 140, 150);
constexpr uint16_t kAccent      = kBlueColour;
constexpr uint16_t kLink        = kBlueColour;
constexpr uint16_t kSelBg       = kBlueColour;
constexpr uint16_t kRule        = kBlackColour;
constexpr uint16_t kOk          = rgb(0, 127, 11);
constexpr uint16_t kWifiOK      = rgb(0, 255, 0);
constexpr uint16_t kPreColor    = rgb(200,200,200);

// Screen geometry (landscape 320x240).
constexpr int kScreenW = 320;
constexpr int kScreenH = 240;
constexpr int kStatusH = 16;            // top bar
constexpr int kHintH   = 13;            // bottom bar
constexpr int kContentY = kStatusH;
constexpr int kContentH = kScreenH - kStatusH - kHintH;

// Fonts. `const lgfx::IFont*` is a pointer to the font data in flash.
// We keep them behind functions so a later version could swap them for
// SD-card fonts without touching the screens.
inline const lgfx::IFont* fontUi()    { return &fonts::lgfxJapanGothicP_16; }
inline const lgfx::IFont* fontSmall() { return &fonts::lgfxJapanGothicP_12; }
// Furigana: 10 px. Gothic has only 8 (too small) or 12 (too big over 16 px
// kanji); efontJA_10 kana are gothic-style anyway.
inline const lgfx::IFont* fontRuby()  { return &fonts::efontJA_10; }

// Reader body font by size (the user cycles 12 / 16 / 20 with 'f').
inline const lgfx::IFont* fontBody(int size) {
    switch (size) {
        case 12: return &fonts::lgfxJapanGothicP_12;
        case 20: return &fonts::lgfxJapanGothicP_20;
        default: return &fonts::lgfxJapanGothicP_16;
    }
}
// Preformatted text (HTML <pre>, Markdown ``` fences): efontJA, whose Latin
// is fixed-width (half-width cells, a console look), so columns line up.
// efont has no 20 px size; 16 is the closest.
inline const lgfx::IFont* fontMono(int bodySize) {
    switch (bodySize) {
        case 12: return &fonts::efontJA_12;
        default: return &fonts::efontJA_16;
    }
}
inline const lgfx::IFont* fontH1(int bodySize) {
    return bodySize >= 16 ? &fonts::lgfxJapanGothicP_24 : &fonts::lgfxJapanGothicP_16;
}

// The per-glyph fallback for each font (nullptr = none). efont has no 20 px
// size; 16 is the closest – the odd š in 20 px text is slightly small.
inline const lgfx::IFont* fallback(const lgfx::IFont* f) {
    if (f == &fonts::lgfxJapanGothicP_12) return &fonts::efontJA_12;
    if (f == &fonts::lgfxJapanGothicP_16) return &fonts::efontJA_16;
    if (f == &fonts::lgfxJapanGothicP_20) return &fonts::efontJA_16;
    if (f == &fonts::lgfxJapanGothicP_24) return &fonts::efontJA_24;
    return nullptr;
}

}  // namespace theme
