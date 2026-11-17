#pragma once
// ============================================================================
//  Layout.hpp – break a rt::Doc into screen lines
// ============================================================================
//
//  Input : a Doc, a width in pixels, and a Metrics object that knows how wide
//          each character is in each font.
//  Output: a vector<Line>. A Line does not contain text – only the byte range
//          [start, end) in doc.text plus the style that is active at `start`.
//          Drawing a line = walking that byte range again (ui/RichView.cpp).
//
//  Storing 16-byte Line records instead of copying the text keeps a 1 MB
//  book's layout at roughly 25 000 lines * 16 B = 400 KB, in ONE allocation.
//
//  Metrics is an abstract interface (pure virtual functions) on purpose:
//  the layout code then does not depend on LovyanGFX at all, so it can be
//  unit-tested on the PC with a fake "every glyph is 8 px" implementation
//  (tests/test_core.cpp does exactly that). Rust: a trait object `&dyn Metrics`.
// ============================================================================

#include <cstdint>
#include <vector>

#include "text/RichText.hpp"

namespace rt {

class Metrics {
public:
    virtual ~Metrics() = default;
    // Horizontal advance of code point `cp` in the font for (block, style).
    virtual int advance(uint32_t cp, char block, uint8_t style) = 0;
    // Height of one line of a paragraph of type `block`.
    virtual int lineHeight(char block) = 0;
    // Furigana visible? If not, ruby text is given zero width.
    virtual bool showRuby() const = 0;
};

struct Line {
    uint32_t start = 0;        // byte offset into doc.text (inclusive)
    uint32_t end = 0;          // byte offset (exclusive)
    uint8_t style = 0;         // StyleBits active at `start`
    char block = 0;            // paragraph type (0 = normal)
    uint8_t height = 0;        // pixels, including paragraph spacing
    bool firstInPara = false;  // bullets are drawn only on the first line
    int16_t image = -1;        // >= 0: this line IS picture doc.images[image]
};

// Pictures are shrunk (never enlarged) to the text width and at most this
// tall, so a picture plus its caption always fits on one screen.
constexpr int kMaxImageH = 150;
// Display size of a picture inside `availW`, keeping its aspect ratio.
void imageFit(const Image& img, int availW, int& dw, int& dh);

// Indentation (pixels) for a paragraph type – shared by layout and drawing.
int blockIndent(char block);

// If text[i] is a BASE code that starts a complete ruby group before `end`,
// set r1/r2 to the positions of the opening and closing RUBY codes and
// return true. A BASE without a complete group is simply ignored.
// Shared by the layout and the renderer so both agree on what a group is.
bool findRubyGroup(const std::string& text, size_t i, size_t end, size_t& r1, size_t& r2);

std::vector<Line> layout(const Doc& doc, int width, Metrics& m);

}  // namespace rt
