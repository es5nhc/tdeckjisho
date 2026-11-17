#pragma once
// ============================================================================
//  Draw.hpp – text drawing with PER-GLYPH FONT FALLBACK
// ============================================================================
//
//  The UI font is lgfxJapanGothicP (sans-serif, proportional, Japanese +
//  Latin + õäöü) – but it has no š, ž or „. Your coverage survey showed the
//  efontJA family has them. So every glyph is "resolved":
//
//      1. the requested font has it?          -> use it
//      2. its fallback font (efontJA) has it? -> use that, for this glyph only
//      3. neither?                            -> a plain stand-in ("„" -> '"')
//
//  This is how real systems handle text too (a browser walks a font list per
//  character). The trick that makes it work in LovyanGFX: updateFontMetric()
//  returns false for a glyph the font does not contain.
//
//  All text in the app is drawn through run()/text() below, so the fallback
//  applies everywhere: lists, status bar, reader.
// ============================================================================

#include <string>
#include <string_view>

#include <LovyanGFX.hpp>

namespace draw {

struct Glyph {
    const lgfx::IFont* font;   // the font that will actually draw it
    uint32_t cp;               // the code point to draw (may be a stand-in)
    int adv;                   // horizontal advance in pixels
    int dy = 0;                // vertical nudge: the font draws this glyph off the baseline
};

Glyph resolve(const lgfx::IFont* font, uint32_t cp);

inline int advance(const lgfx::IFont* font, uint32_t cp) { return resolve(font, cp).adv; }

// Width of a UTF-8 string (sum of resolved advances).
int textWidth(const lgfx::IFont* font, std::string_view s);

// Distance from the top of the font's box to its baseline.
int ascent(const lgfx::IFont* font);
int height(const lgfx::IFont* font);

// Cut `s` so it fits in maxW pixels, adding "…" if anything was removed.
std::string fit(const lgfx::IFont* font, std::string_view s, int maxW);

// Draw `s` with its BASELINE at y (so different fonts line up).
// bold = "fake bold": the run is drawn twice, 1 px apart, because the
// gothic font has no bold variant. Returns the width drawn.
int run(lgfx::LovyanGFX& g, const lgfx::IFont* font, std::string_view s, int x, int baselineY,
        uint16_t color, bool bold = false);

// Same, but y is the TOP of the text line.
int text(lgfx::LovyanGFX& g, const lgfx::IFont* font, std::string_view s, int x, int y,
         uint16_t color, bool bold = false);

// Right-aligned so the text ends at x.
void textRight(lgfx::LovyanGFX& g, const lgfx::IFont* font, std::string_view s, int x, int y,
               uint16_t color);

// Several lines of centred text (for "no results", errors, loading...).
void message(lgfx::LovyanGFX& g, int x, int y, int w, int h, std::string_view msg,
             uint16_t color);

}  // namespace draw
