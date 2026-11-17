#include "ui/RichView.hpp"

#include <algorithm>

#include "core/Utf8.hpp"
#include "ui/Draw.hpp"
#include "ui/Theme.hpp"

using namespace rt;   // lets us write BULLET instead of rt::BULLET in this file

// ============================================================================
//  FontMetrics
// ============================================================================

void FontMetrics::configure(int bodySize, bool showRuby, bool rubyBand) {
    _size = bodySize;
    _ruby = showRuby;
    // efontJA_10 furigana are 10 px tall; a 9 px band plus the line's own top
    // padding is enough. Without furigana in the document: no band at all.
    _band = (showRuby && rubyBand) ? 9 : 0;
    _fonts[0] = theme::fontBody(bodySize);   // normal
    _fonts[1] = theme::fontBody(bodySize);   // bold: same font, drawn "fake bold"
    _fonts[2] = theme::fontH1(bodySize);
    _fonts[3] = theme::fontBody(16);         // H2
    _fonts[4] = theme::fontSmall();
    _fonts[5] = theme::fontRuby();
    _fonts[6] = theme::fontMono(bodySize);   // PRE: fixed-width
    for (auto& c : _cache) c = CacheSlot{};  // fonts changed: forget all widths
}

int FontMetrics::fontSlot(char block, uint8_t style) const {
    if (style & S_RUBY) return 5;
    if (block == PRE) return 6;                        // bold too: fake bold of mono
    if (block == H1) return 2;
    if (block == H2) return 3;
    if (block == SMALL || block == IMAGE) return 4;   // captions: small print
    return (style & S_BOLD) ? 1 : 0;
}

const lgfx::IFont* FontMetrics::font(char block, uint8_t style) const {
    return _fonts[fontSlot(block, style)];
}

bool FontMetrics::bold(char block, uint8_t style) const {
    return !(style & S_RUBY) && ((style & S_BOLD) || block == H1 || block == H2);
}

int FontMetrics::advance(uint32_t cp, char block, uint8_t style) {
    const int slot = fontSlot(block, style);
    const uint32_t key = (cp << 3) | static_cast<uint32_t>(slot + 1);   // never 0 (slot+1 <= 7)
    CacheSlot& c = _cache[(cp * 31 + slot) & 1023];                     // & 1023 == % 1024
    if (c.key != key) {                                                  // cache miss:
        c.key = key;                                                     // measure, with
        c.adv = static_cast<int16_t>(draw::advance(_fonts[slot], cp));   // font fallback
    }
    return c.adv;
}

int FontMetrics::lineHeight(char block) {
    return textHeight(block) + (block == SMALL || block == RULE || block == IMAGE ? 0 : _band);
}

int FontMetrics::textHeight(char block) const {
    switch (block) {
        case H1:    return _size >= 16 ? 30 : 22;
        case H2:    return 22;
        case SMALL:
        case IMAGE: return 17;
        case RULE:  return 10;
        default:    return _size + 6;          // e.g. 16 px glyphs, 22 px lines
    }
}

int FontMetrics::baseline(char block) const {
    // Centre the font's box vertically in the line, then go down to its
    // baseline. The furigana band (if any) sits above all of that.
    const lgfx::IFont* f = font(block, 0);
    const int top = (textHeight(block) - draw::height(f)) / 2;
    const int band = (block == SMALL || block == IMAGE) ? 0 : _band;
    return band + top + draw::ascent(f);
}

// ============================================================================
//  RichView
// ============================================================================

void RichView::layout(int width, int bodySize, bool showRuby) {
    const uint32_t pos = _lines.empty() ? 0 : position();
    const bool hasGroups = _doc.text.find(rt::BASE) != std::string::npos;
    _m.configure(bodySize, showRuby, hasGroups);
    _lines = rt::layout(_doc, width, _m);
    setPosition(pos);
}

uint32_t RichView::position() const {
    return _lines.empty() ? 0 : _lines[std::min(_top, _lines.size() - 1)].start;
}

void RichView::setPosition(uint32_t offset) {
    // Find the last line that starts at or before `offset`. Lines are sorted
    // by start, so binary search (std::upper_bound) does it in log2(n) steps.
    auto it = std::upper_bound(_lines.begin(), _lines.end(), offset,
                               [](uint32_t off, const Line& l) { return off < l.start; });
    _top = (it == _lines.begin()) ? 0 : static_cast<size_t>(it - _lines.begin() - 1);
}

int RichView::percent() const {
    if (_lines.size() <= 1) return 100;
    return static_cast<int>(_top * 100 / (_lines.size() - 1));
}

void RichView::scrollLines(int delta) {
    const long t = static_cast<long>(_top) + delta;
    _top = static_cast<size_t>(std::max(0L, std::min(t, static_cast<long>(_lines.size()) - 1)));
}

void RichView::pageDown(int h) {
    // Advance past every line that is fully visible now; the first partly
    // visible line becomes the new top, so nothing is skipped.
    int used = 0;
    size_t i = _top;
    while (i < _lines.size() && used + _lines[i].height <= h) used += _lines[i++].height;
    if (i >= _lines.size()) return;     // already showing the end
    _top = std::max(i, _top + 1);
}

void RichView::pageUp(int h) {
    int used = 0;
    while (_top > 0 && used + _lines[_top - 1].height <= h) used += _lines[--_top].height;
}

void RichView::toEnd(int h) {
    _top = _lines.size();
    pageUp(h);
}

void RichView::draw(lgfx::LovyanGFX& g, const Rect& r) {
    const int margin = 6;
    _viewH = r.h;
    _lastRect = r;
    int y = r.y + 2;
    for (size_t i = _top; i < _lines.size(); ++i) {
        // Only draw lines that fit completely; a half-visible line at the
        // bottom is distracting, and pageDown() will show it in full next.
        // (Except the very first line, in case one line is taller than r.h.)
        // Pictures are the exception: one picture is one line up to 156 px tall, so
        // "wait until it fits" leaves a big empty hole at the bottom. Draw it anyway;
        // the clip rect set in App::render() cuts off the part below the content area.
        if (i > _top && _lines[i].image < 0 && y + _lines[i].height - 4 > r.y + r.h) break;
        if (y >= r.y + r.h) break;          // a picture line that starts below the area
        drawLine(g, _lines[i], r.x + margin, y, r.w - 2 * margin);
        if (_cursorOn) drawCursorOnLine(g, i, r.x + margin, y);
        y += _lines[i].height;
    }
    // Progress bar on the right edge.
    if (_lines.size() > 1) {
        const int ty = r.y + (r.h - 12) * percent() / 100;
        g.fillRect(r.x + r.w - 2, ty, 2, 12, theme::kRule);
    }
}

// ============================================================================
//  The text cursor
// ============================================================================

size_t RichView::lineIndexOf(uint32_t offset) const {
    // The last line that starts at or before `offset` (lines are sorted by start).
    auto it = std::upper_bound(_lines.begin(), _lines.end(), offset,
                               [](uint32_t off, const Line& l) { return off < l.start; });
    return it == _lines.begin() ? 0 : static_cast<size_t>(it - _lines.begin() - 1);
}

void RichView::cursorStart() {
    _wantX = -1;
    _markEnd = 0;
    for (size_t i = _top; i < _lines.size(); ++i) {
        if (_lines[i].image >= 0) continue;
        const size_t o = rt::firstUnitAtOrAfter(_doc, _lines[i].start);
        if (o != rt::kNoOffset && o < _lines[i].end) {
            _cursor = static_cast<uint32_t>(o);
            _cursorOn = true;
            return;
        }
    }
}

void RichView::cursorMove(int dx, int dy) {
    if (!_cursorOn) return;
    _markEnd = 0;
    if (dx != 0) {
        const size_t n = dx > 0 ? rt::nextOffset(_doc, _cursor) : rt::prevOffset(_doc, _cursor);
        if (n != rt::kNoOffset) _cursor = static_cast<uint32_t>(n);
        _wantX = -1;                                   // a horizontal move forgets the goal column
    }
    if (dy != 0) {
        const size_t here = lineIndexOf(_cursor);
        if (_wantX < 0) {                              // remember where we were, to come back to it
            for (const rt::Unit& u : rt::lineUnits(_doc, _lines[here], _m))
                if (u.start == _cursor) { _wantX = u.x; break; }
            if (_wantX < 0) _wantX = 0;
        }
        for (long j = static_cast<long>(here) + dy; j >= 0 && j < static_cast<long>(_lines.size()); j += dy) {
            const auto units = rt::lineUnits(_doc, _lines[j], _m);
            const int k = rt::unitAtX(units, _wantX);
            if (k >= 0) { _cursor = units[k].start; break; }   // pictures and blank lines have no units
        }
    }
    ensureCursorVisible();
}

void RichView::ensureCursorVisible() {
    const size_t line = lineIndexOf(_cursor);
    if (line < _top) { _top = line; return; }
    int used = 0;
    for (size_t i = _top; i <= line && i < _lines.size(); ++i) used += _lines[i].height;
    while (_top < line && used > _viewH - 4) used -= _lines[_top++].height;   // scroll down just enough
}

int RichView::cursorScreenY() const {
    if (!_cursorOn) return -1;
    const size_t line = lineIndexOf(_cursor);
    if (line < _top) return -1;
    int y = _lastRect.y + 2;
    for (size_t i = _top; i < line; ++i) y += _lines[i].height;
    return y < _lastRect.y + _lastRect.h ? y : -1;
}

bool RichView::cursorFromPoint(int x, int y) {
    const int margin = 6;
    int top = _lastRect.y + 2;
    for (size_t i = _top; i < _lines.size(); ++i) {
        if (y < top + _lines[i].height) {
            const auto units = rt::lineUnits(_doc, _lines[i], _m);
            const int k = rt::unitUnderX(units, x - (_lastRect.x + margin + rt::blockIndent(_lines[i].block)));
            if (k < 0) return false;                   // empty space, a picture, a rule, a blank line
            _cursor = units[k].start;
            _cursorOn = true;
            _wantX = -1;
            _markEnd = 0;
            return true;
        }
        top += _lines[i].height;
        if (top > _lastRect.y + _lastRect.h) break;
    }
    return false;
}

void RichView::drawCursorOnLine(lgfx::LovyanGFX& g, size_t li, int x0, int y) {
    const Line& line = _lines[li];
    // Only lines that hold the cursor or part of the marked span need their units measured.
    const uint32_t spanEnd = std::max<uint32_t>(_markEnd, _cursor + 1);
    if (line.end <= _cursor || line.start >= spanEnd) return;
    const int lh = _m.lineHeight(line.block);
    const bool noBand = line.block == SMALL || line.block == RULE || line.block == IMAGE;
    const int band = noBand ? 0 : _m.band();
    const int xs = x0 + rt::blockIndent(line.block);
    for (const rt::Unit& u : rt::lineUnits(_doc, line, _m)) {
        if (!u.selectable || u.w <= 0) continue;
        if (u.start >= _cursor && u.start < _markEnd)               // the matched span: an underline
            g.fillRect(xs + u.x, y + lh - 2, u.w, 2, theme::kAccent);
        if (u.start == _cursor)                                     // the cursor: a box
            g.drawRect(xs + u.x - 1, y + band, u.w + 2, lh - band, theme::kAccent);
    }
}

void RichView::drawLine(lgfx::LovyanGFX& g, const Line& line, int x0, int y, int w) {
    const char block = line.block;
    const int lh = _m.lineHeight(block);

    // ---- pictures ------------------------------------------------------------
    if (line.image >= 0) {
        const rt::Image& img = _doc.images[line.image];
        int dw, dh;
        rt::imageFit(img, w, dw, dh);
        _images.draw(g, img, line.image, x0 + (w - dw) / 2, y + 3, dw, dh);   // centred
        return;
    }
    // A picture that can't be shown: a little "broken picture" icon before
    // its caption (the caption says what it was; the log says why).
    if (block == IMAGE && line.firstInPara) {
        g.drawRect(x0, y + 3, 11, 10, theme::kDim);
        g.drawLine(x0 + 2, y + 10, x0 + 5, y + 6, theme::kDim);
        g.drawLine(x0 + 5, y + 6, x0 + 8, y + 10, theme::kDim);
    }

    // ---- block decorations -------------------------------------------------
    if (block == RULE) { g.drawFastHLine(x0 + 8, y + 4, w - 16, theme::kRule); return; }
    if (block == QUOTE) g.fillRect(x0 + 2, y, 2, line.height, theme::kRule);
    if (block == BULLET && line.firstInPara) g.fillCircle(x0 + 5, y + lh / 2, 2, theme::kAccent);
    if (block == PRE) g.fillRect(x0, y, w, line.height, theme::kPreColor);

    int x = x0 + rt::blockIndent(block);
    const int base = y + _m.baseline(block);
    uint8_t style = line.style;
    std::string run;               // characters waiting to be drawn in `style`
    int runW = 0;

    // Draw the pending run in the current style and move x past it.
    // draw::run() handles fallback glyphs (š ž „) and fake bold.
    auto flush = [&] {
        if (run.empty()) return;
        const bool hidden = (style & S_RUBY) && !_m.showRuby();
        if (!hidden) {
            uint16_t col = theme::kText;
            if (block == H1 || block == H2 || (style & S_ACCENT)) col = theme::kAccent;
            if (block == SMALL || block == IMAGE || (style & (S_DIM | S_RUBY))) col = theme::kDim;
            if (style & S_LINK) col = theme::kLink;
            draw::run(g, _m.font(block, style), run, x, base, col, _m.bold(block, style));
            if (style & S_LINK) g.drawFastHLine(x, base + 2, runW, theme::kLink);
            x += runW;
        }
        run.clear();
        runW = 0;
    };

    const std::string& t = _doc.text;
    const std::string_view view(t.data(), line.end);
    for (size_t i = line.start; i < line.end;) {
        const char c = t[i];
        size_t r1, r2;
        if (c == BASE && findRubyGroup(t, i, line.end, r1, r2)) {
            flush();               // furigana group: base with ruby above it
            x += drawRubyGroup(g, i, r1, r2, block, style, x, y, base);
            i = r2 + 1;
            continue;
        }
        if (isInlineCode(c)) {     // style change: draw what we have, then switch
            flush();
            style = applyCode(style, c);
            ++i;
            continue;
        }
        const size_t at = i;
        const uint32_t cp = utf8::next(view, i);
        run.append(t, at, i - at);                    // copy the character's bytes
        runW += _m.advance(cp, block, style);
    }
    flush();
}

int RichView::drawRubyGroup(lgfx::LovyanGFX& g, size_t i, size_t r1, size_t r2, char block,
                            uint8_t& style, int x, int lineTop, int baseY) {
    const std::string& t = _doc.text;
    // Collect the base and ruby text (without control codes) and their widths.
    std::string baseText, rubyText;
    int baseW = 0, rubyW = 0;
    {
        const std::string_view view(t.data(), r1);
        for (size_t k = i + 1; k < r1;) {
            if (isInlineCode(t[k])) { style = applyCode(style, t[k]); ++k; continue; }
            const size_t at = k;
            baseW += _m.advance(utf8::next(view, k), block, style);
            baseText.append(t, at, k - at);
        }
    }
    const uint8_t rubyStyle = style | S_RUBY;
    {
        const std::string_view view(t.data(), r2);
        for (size_t k = r1 + 1; k < r2;) {
            if (isInlineCode(t[k])) { ++k; continue; }
            const size_t at = k;
            rubyW += _m.advance(utf8::next(view, k), block, rubyStyle);
            rubyText.append(t, at, k - at);
        }
    }

    const bool show = _m.showRuby();
    const int groupW = show ? std::max(baseW, rubyW) : baseW;

    // Base: centred in the group, on the normal baseline.
    uint16_t col = theme::kText;
    if (block == H1 || block == H2 || (style & S_ACCENT)) col = theme::kAccent;
    if (style & S_LINK) col = theme::kLink;
    draw::run(g, _m.font(block, style), baseText, x + (groupW - baseW) / 2, baseY, col,
              _m.bold(block, style));

    // Ruby: small and dim, centred ABOVE the base, inside the band.
    if (show) {
        const lgfx::IFont* rf = _m.font(block, rubyStyle);
        draw::run(g, rf, rubyText, x + (groupW - rubyW) / 2, lineTop + draw::ascent(rf), theme::kDim);
    }
    return groupW;
}
