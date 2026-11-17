#include "ui/Draw.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "core/Str.hpp"
#include "core/Utf8.hpp"
#include "ui/Theme.hpp"

namespace draw {

namespace {

// Characters that are legitimately invisible (zero width).
bool isZeroWidth(uint32_t cp) {
    return (cp >= 0x0300 && cp <= 0x036F)      // combining accents
        || (cp >= 0x200B && cp <= 0x200F)      // zero-width space/joiners, LRM/RLM
        || (cp >= 0xFE00 && cp <= 0xFE0F)      // variation selectors
        || cp == 0xFEFF;                       // byte-order mark
}

// A plain stand-in for characters no font has.
uint32_t substitute(uint32_t cp) {
    switch (cp) {
        case 0x201E: case 0x201C: case 0x201D: case 0x00AB: case 0x00BB: return '"';
        case 0x201A: case 0x2018: case 0x2019: case 0x2039: case 0x203A: return '\'';
        case 0x2013: case 0x2014: case 0x2212: return '-';
        case 0x00A0: case 0x2009: case 0x202F: return ' ';
        default: return '?';
    }
}

// Advance of cp in font, or -1 if the font has no such glyph.
int glyphAdvance(const lgfx::IFont* font, uint32_t cp) {
    if (!font || cp > 0xFFFF) return -1;           // these fonts are 16-bit only (no emoji)
    lgfx::FontMetrics m;
    font->getDefaultMetric(&m);                    // start from the font's defaults...
    if (!font->updateFontMetric(&m, static_cast<uint16_t>(cp))) return -1;   // ...then this glyph
    return m.x_advance;
}

// Curly quotes, which turn up in English text, mostly as apostrophes.
bool isLatinQuote(uint32_t cp) {
    return cp == 0x2018 || cp == 0x2019 || cp == 0x201C || cp == 0x201D;
}

// Is an advance of `a` a full-width (kanji-sized) cell in this font? We compare against
// the font's own 'M', the widest ordinary Latin letter.
bool isFullWidth(const lgfx::IFont* font, int a) {
    const int m = glyphAdvance(font, 'M');
    return m > 0 && a > m;
}

// ---- glyphs the font draws a pixel off ---------------------------------------------
// The GothicP bitmap fonts were made from a scalable design, and at 12–24 px some glyphs
// came out a pixel too high or too low: a "t" floating above its neighbours, a 先
// sinking below 日, a squashed 書 that seems to float. The font can't tell us (LovyanGFX
// keeps the glyph's box to itself), so we LOOK: draw the glyph once into a tiny sprite,
// find its top and bottom pixel, compare with ordinary glyphs of the same font, and
// remember the answer.

// Rows of the first and last pixel of `cp`, drawn with its baseline at row 32.
void inkRows(const lgfx::IFont* font, uint32_t cp, int& top, int& bottom) {
    static lgfx::LGFX_Sprite s;               // 1 bit per pixel: 48 x 48 = 288 bytes
    if (!s.width()) { s.setColorDepth(1); s.createSprite(48, 48); }
    std::string t;
    utf8::append(t, cp);
    s.fillScreen(0);
    s.setFont(font);
    s.setTextColor(1);
    s.setTextDatum(lgfx::baseline_left);
    s.drawString(t.c_str(), 4, 32);
    top = bottom = -1;
    for (int y = 0; y < 48; ++y)
        for (int x = 0; x < 48; ++x)
            if (s.readPixel(x, y)) { if (top < 0) top = y; bottom = y; break; }
}

// The middle value of `n` numbers: one odd glyph among the references can't spoil it.
int median(int* v, int n) {
    std::sort(v, v + n);
    return v[n / 2];
}

// What an ORDINARY glyph of this font looks like: the bottom row of letters without a
// descender, and the vertical centre (top + bottom, in half pixels) of kanji.
struct Reference { const lgfx::IFont* font = nullptr; int latinBottom = 0, kanjiCentre = 0; };

const Reference& referenceFor(const lgfx::IFont* font) {
    static Reference refs[8];                 // the few fonts we draw with
    for (Reference& r : refs) if (r.font == font) return r;
    static int next = 0;
    Reference& r = refs[next++ % 8];
    r.font = font;
    int v[10], top, bottom;
    const uint32_t latin[] = {'x', 'n', 'm', 'z', 'u', 'v', 'w'};
    for (int i = 0; i < 7; ++i) { inkRows(font, latin[i], top, bottom); v[i] = bottom; }
    r.latinBottom = median(v, 7);
    const uint32_t kanji[] = {0x65E5, 0x672C, 0x8A9E, 0x56FD, 0x4EBA, 0x4E2D, 0x5927, 0x5B66, 0x5E74};  // 日本語国人中大学年
    for (int i = 0; i < 9; ++i) { inkRows(font, kanji[i], top, bottom); v[i] = top + bottom; }
    r.kanjiCentre = median(v, 9);
    return r;
}

// Letters and digits that sit ON the baseline (no descender: not g j p q y).
bool sitsOnBaseline(uint32_t cp) {
    if (cp < 0x80) return std::isalnum(static_cast<int>(cp)) && !std::strchr("gjpqyQJ", static_cast<int>(cp));
    switch (cp) {
        case 0xC4: case 0xD5: case 0xD6: case 0xDC: case 0x160: case 0x17D:   // Ä Õ Ö Ü Š Ž
        case 0xE4: case 0xF5: case 0xF6: case 0xFC: case 0x161: case 0x17E:   // ä õ ö ü š ž
            return true;
        default:
            return false;
    }
}

// Pixels to move `cp`, drawn in `drawn`, down (negative: up) so that it lines up with
// the text of `font`. Usually drawn == font; for a fallback glyph (š in GothicP comes from
// efontJA) they differ, and the fallback font may sit quite differently on the baseline.
int measureNudge(const lgfx::IFont* font, const lgfx::IFont* drawn, uint32_t cp) {
    const bool latin = sitsOnBaseline(cp);
    const bool kanji = cp >= 0x4E00 && cp <= 0x9FFF;  // CJK ideographs: all the common kanji
    if (!latin && !kanji) return 0;                  // everything else is drawn as the font says
    const Reference& ref = referenceFor(font);
    int top, bottom;
    inkRows(drawn, cp, top, bottom);
    if (top < 0) return 0;
    if (latin) return ref.latinBottom - bottom;
    const int d = ref.kanjiCentre - (top + bottom);   // half pixels
    return (d >= 2 || d <= -2) ? d / 2 : 0;           // off by a pixel or more: move it back
}

// Measuring costs a little drawing, so each answer is remembered: a direct-mapped cache
// like the glyph-width cache in text/Layout.cpp (a collision just measures again).
int baselineNudge(const lgfx::IFont* font, const lgfx::IFont* drawn, uint32_t cp) {
    struct Slot { const lgfx::IFont* font = nullptr; uint32_t cp = 0; int8_t dy = 0; };
    static Slot cache[512];
    Slot& slot = cache[(cp * 31 + reinterpret_cast<uintptr_t>(font) / 8) & 511];
    if (slot.font != font || slot.cp != cp) {
        slot.font = font;
        slot.cp = cp;
        slot.dy = static_cast<int8_t>(measureNudge(font, drawn, cp));   // `drawn` follows from (font, cp)
    }
    return slot.dy;
}

}  // namespace

Glyph resolve(const lgfx::IFont* font, uint32_t cp) {
    if (isZeroWidth(cp)) return {font, cp, 0};
    int a = glyphAdvance(font, cp);
    if (a >= 0 && isLatinQuote(cp) && isFullWidth(font, a)) {
        // The Japanese fonts draw ‘ ’ “ ” full width (they come from JIS X 0208), which
        // turns "Japan’s" into "Japan’   s". In running Latin text the ASCII stand-in
        // looks right, so use that instead.
        const uint32_t sub = substitute(cp);
        const int sa = glyphAdvance(font, sub);
        if (sa >= 0) return {font, sub, sa};
    }
    if (a >= 0) return {font, cp, a, baselineNudge(font, font, cp)};   // 1. the font itself
    const lgfx::IFont* fb = theme::fallback(font);
    a = glyphAdvance(fb, cp);
    if (a >= 0) return {fb, cp, a, baselineNudge(font, fb, cp)};   // 2. its fallback
    const uint32_t sub = substitute(cp);                      // 3. a stand-in
    a = glyphAdvance(font, sub);
    return {font, sub, a < 0 ? 0 : a};
}

int textWidth(const lgfx::IFont* font, std::string_view s) {
    int w = 0;
    for (size_t i = 0; i < s.size();) w += advance(font, utf8::next(s, i));
    return w;
}

int ascent(const lgfx::IFont* font) {
    lgfx::FontMetrics m;
    font->getDefaultMetric(&m);
    return m.baseline;
}

int height(const lgfx::IFont* font) {
    lgfx::FontMetrics m;
    font->getDefaultMetric(&m);
    return m.height;
}

std::string fit(const lgfx::IFont* font, std::string_view s, int maxW) {
    if (textWidth(font, s) <= maxW) return std::string(s);
    const int ell = advance(font, 0x2026);      // "…"
    int w = 0;
    size_t i = 0, lastFit = 0;
    while (i < s.size()) {
        const size_t at = i;
        w += advance(font, utf8::next(s, i));
        if (w + ell > maxW) { lastFit = at; break; }
        lastFit = i;
    }
    return std::string(s.substr(0, lastFit)) + "…";
}

int run(lgfx::LovyanGFX& g, const lgfx::IFont* font, std::string_view s, int x, int baselineY,
        uint16_t color, bool bold) {
    // Split the string into segments that use the same resolved font, and
    // draw each segment with one drawString() call at our own x position.
    g.setTextColor(color);                    // one argument = transparent background
    g.setTextDatum(lgfx::baseline_left);      // y is the baseline, for every font
    const int x0 = x;
    std::string seg;
    const lgfx::IFont* segFont = nullptr;
    int segW = 0;
    int segDy = 0;                            // the segment's baseline nudge (see baselineNudge)

    auto flush = [&] {
        if (seg.empty()) return;
        g.setFont(segFont);
        g.drawString(seg.c_str(), x, baselineY + segDy);
        if (bold) g.drawString(seg.c_str(), x + 1, baselineY + segDy);   // fake bold
        x += segW;
        seg.clear();
        segW = 0;
    };

    for (size_t i = 0; i < s.size();) {
        const Glyph gl = resolve(font, utf8::next(s, i));
        if (gl.font != segFont || gl.dy != segDy) { flush(); segFont = gl.font; segDy = gl.dy; }
        utf8::append(seg, gl.cp);
        segW += gl.adv;
    }
    flush();
    return x - x0;
}

int text(lgfx::LovyanGFX& g, const lgfx::IFont* font, std::string_view s, int x, int y,
         uint16_t color, bool bold) {
    return run(g, font, s, x, y + ascent(font), color, bold);
}

void textRight(lgfx::LovyanGFX& g, const lgfx::IFont* font, std::string_view s, int x, int y,
               uint16_t color) {
    text(g, font, s, x - textWidth(font, s), y, color);
}

void message(lgfx::LovyanGFX& g, int x, int y, int w, int h, std::string_view msg,
             uint16_t color) {
    const auto lines = str::split(msg, '\n');
    const lgfx::IFont* f = theme::fontSmall();
    const int lh = 17;
    int ty = y + (h - static_cast<int>(lines.size()) * lh) / 2;
    for (auto line : lines) {
        const std::string fitted = fit(f, line, w - 8);
        text(g, f, fitted, x + (w - textWidth(f, fitted)) / 2, ty, color);
        ty += lh;
    }
}

}  // namespace draw
