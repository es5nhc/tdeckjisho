#pragma once
// ============================================================================
//  RichView.hpp – draws a laid-out rt::Doc and scrolls through it
// ============================================================================
//
//  Glue between the pure text engine (text/Layout) and LovyanGFX:
//    * FontMetrics implements rt::Metrics with real efont glyph widths
//    * RichView keeps the Doc, its Lines, the scroll position, and draws.
// ============================================================================

#include <cstdint>
#include <vector>

#include <LovyanGFX.hpp>

#include "text/Cursor.hpp"
#include "text/Layout.hpp"
#include "ui/ImageCache.hpp"
#include "ui/Screen.hpp"

// rt::Metrics backed by LovyanGFX fonts, with a small width cache.
class FontMetrics : public rt::Metrics {
public:
    // rubyBand: the document contains furigana groups, so reserve a strip
    // above every line for them (only while furigana are shown).
    void configure(int bodySize, bool showRuby, bool rubyBand);
    int band() const { return _band; }
    int bodySize() const { return _size; }

    // rt::Metrics interface. `override` asks the compiler to verify that we
    // really override a virtual function (catches typos in the signature).
    int advance(uint32_t cp, char block, uint8_t style) override;
    int lineHeight(char block) override;
    bool showRuby() const override { return _ruby; }

    const lgfx::IFont* font(char block, uint8_t style) const;
    bool bold(char block, uint8_t style) const;     // draw "fake bold"?
    // Distance from the top of a line to the text baseline.
    int baseline(char block) const;

private:
    int fontSlot(char block, uint8_t style) const;
    int textHeight(char block) const;   // line height without the furigana band

    int _size = 16;
    bool _ruby = true;
    int _band = 0;                       // pixels above each line for furigana
    const lgfx::IFont* _fonts[7] = {};   // body, bold, h1, h2, small, ruby, mono

    // A "direct-mapped cache": 1024 slots, the slot is chosen by hashing the
    // (font, code point) pair. A miss simply overwrites the slot. Measuring
    // a glyph walks the font tables; the cache turns that into one array
    // lookup for the ~2000 characters a typical text actually uses.
    struct CacheSlot { uint32_t key = 0; int16_t adv = 0; };
    CacheSlot _cache[1024];
};

class RichView {
public:
    // Takes the Doc by value and moves it in: the caller's copy is "used up".
    void setDoc(rt::Doc doc) {
        _doc = std::move(doc);
        _lines.clear();
        _top = 0;
        _images.clear();
        _cursorOn = false;
        _hasRuby = _doc.text.find(rt::RUBY) != std::string::npos;
    }
    const rt::Doc& doc() const { return _doc; }
    // Does the text carry furigana (ruby)? We never derive readings ourselves, so
    // without any in the source the furigana on/off switch would do nothing.
    bool hasRuby() const { return _hasRuby; }

    // (Re)compute the lines for a given width. Keeps the reading position.
    void layout(int width, int bodySize, bool showRuby);
    bool needsLayout() const { return _lines.empty() && !_doc.text.empty(); }

    void draw(lgfx::LovyanGFX& g, const Rect& r);

    void scrollLines(int delta);
    void pageDown(int h);
    void pageUp(int h);
    void toTop() { _top = 0; }
    void toEnd(int h);

    // Reading position as a BYTE OFFSET into the text. Unlike a line number,
    // it stays valid when the font size (and so the line breaks) change.
    uint32_t position() const;
    void setPosition(uint32_t offset);
    int percent() const;

    // ---- the text cursor (word look-up) ---------------------------------------------------------
    // A cursor stands on one character (or one furigana group) of the text. It is a BYTE OFFSET
    // into doc.text, like the reading position, so it survives a change of font size.
    bool cursorActive() const { return _cursorOn; }
    void cursorStart();                           // on the first character of the first visible line
    void cursorStop() { _cursorOn = false; _markEnd = 0; }
    void cursorMove(int dx, int dy);              // dx: characters left/right, dy: lines up/down
    // Place the cursor under a screen point (a tap). False if there is no text there.
    bool cursorFromPoint(int x, int y);
    // The plain text from the cursor on, for a look-up (see Dictionary::lookupAt).
    std::string cursorText(size_t maxChars, size_t* endOffset = nullptr) const {
        return rt::plainTextFrom(_doc, _cursor, maxChars, endOffset);
    }
    // The text just before the cursor, in the same paragraph (see rt::plainTextBefore).
    std::string cursorTextBefore(size_t maxChars) const { return rt::plainTextBefore(_doc, _cursor, maxChars); }
    // Move the cursor to the start of the Latin word it is in (no effect on Japanese text).
    void cursorToWordStart() { _cursor = static_cast<uint32_t>(rt::latinWordStart(_doc, _cursor)); }
    // Underline [cursor, endOffset): the part of the text a look-up matched. 0 = no mark.
    void cursorMark(size_t endOffset) { _markEnd = static_cast<uint32_t>(endOffset); }
    // Screen y of the top of the cursor's line, or -1 if it is not on screen.
    int cursorScreenY() const;

private:
    size_t lineIndexOf(uint32_t offset) const;    // the line a byte offset is on
    void ensureCursorVisible();
    void drawCursorOnLine(lgfx::LovyanGFX& g, size_t lineIdx, int x0, int y);
    void drawLine(lgfx::LovyanGFX& g, const rt::Line& line, int x0, int y, int w);
    // Draw one BASE..RUBY..RUBY group starting at text[i]; returns its width.
    int drawRubyGroup(lgfx::LovyanGFX& g, size_t i, size_t r1, size_t r2, char block,
                      uint8_t& style, int x, int lineTop, int baseY);

    rt::Doc _doc;
    std::vector<rt::Line> _lines;
    size_t _top = 0;
    FontMetrics _m;
    ImageCache _images;          // decoded pictures of THIS document (freed with it)

    bool _hasRuby = false;
    bool _cursorOn = false;
    uint32_t _cursor = 0;        // byte offset of the unit the cursor stands on
    uint32_t _markEnd = 0;       // end of the matched span to underline (0 = none)
    int _wantX = -1;             // "goal column": remembered x while moving up/down over short lines
    int _viewH = 200;            // the height and place of the last draw(), for scrolling and taps
    Rect _lastRect{0, 0, 320, 200};
};
