#include "text/Layout.hpp"

#include <algorithm>

#include "core/Utf8.hpp"

namespace rt {

namespace {

// ---------------------------------------------------------------------------
//  Japanese line-breaking rules (禁則処理, "kinsoku shori"), simplified.
//
//  Japanese has no spaces, so a line may break between almost any two
//  characters – EXCEPT that a line must not START with closing punctuation or
//  small kana, and must not END with an opening bracket. Latin text breaks
//  only at spaces. Mixed text follows both rules.
// ---------------------------------------------------------------------------
bool noBreakBefore(uint32_t c) {
    switch (c) {
        case 0x3001: case 0x3002:                 // 、 。
        case 0xFF0C: case 0xFF0E:                 // ， ．
        case 0x30FB: case 0xFF1A: case 0xFF1B:    // ・ ： ；
        case 0xFF01: case 0xFF1F:                 // ！ ？
        case 0x30FC: case 0x301C:                 // ー 〜
        case 0x300D: case 0x300F: case 0xFF09:    // 」 』 ）
        case 0x3011: case 0x3015: case 0x3009: case 0x300B:  // 】 〕 〉 》
        case 0x3041: case 0x3043: case 0x3045: case 0x3047: case 0x3049:  // ぁぃぅぇぉ
        case 0x3063: case 0x3083: case 0x3085: case 0x3087: case 0x308E:  // っゃゅょゎ
        case 0x30A1: case 0x30A3: case 0x30A5: case 0x30A7: case 0x30A9:  // ァィゥェォ
        case 0x30C3: case 0x30E3: case 0x30E5: case 0x30E7: case 0x30EE:  // ッャュョヮ
        case ',': case '.': case ';': case ':': case '!': case '?': case ')': case ']':
            return true;
        default:
            return false;
    }
}

bool noBreakAfter(uint32_t c) {
    switch (c) {
        case 0x300C: case 0x300E: case 0xFF08:    // 「 『 （
        case 0x3010: case 0x3014: case 0x3008: case 0x300A:  // 【 〔 〈 《
        case '(': case '[':
            return true;
        default:
            return false;
    }
}

constexpr int kParagraphGap = 4;   // extra pixels below each paragraph

}  // namespace

int blockIndent(char block) {
    switch (block) {
        case BULLET: return 14;
        case QUOTE:  return 10;
        case PRE:    return 6;
        case IMAGE:  return 14;     // room for the "missing picture" icon
        default:     return 0;
    }
}

void imageFit(const Image& img, int availW, int& dw, int& dh) {
    dw = img.w;
    dh = img.h;
    if (dw > availW) { dh = dh * availW / dw; dw = availW; }          // too wide
    if (dh > kMaxImageH) { dw = dw * kMaxImageH / dh; dh = kMaxImageH; }  // too tall
    dw = std::max(dw, 1);
    dh = std::max(dh, 1);
}

bool findRubyGroup(const std::string& t, size_t i, size_t end, size_t& r1, size_t& r2) {
    r1 = r2 = std::string::npos;
    for (size_t j = i + 1; j < end; ++j) {
        if (t[j] == BASE) return false;               // another group starts first
        if (t[j] != RUBY) continue;
        if (r1 == std::string::npos) r1 = j;
        else { r2 = j; return true; }
    }
    return false;
}

std::vector<Line> layout(const Doc& doc, int width, Metrics& m) {
    std::vector<Line> lines;
    const std::string& t = doc.text;
    const size_t n = t.size();
    // A guess that avoids most re-allocations (≈ 40 bytes per line).
    lines.reserve(n / 40 + 8);

    // Helper lambda that appends one finished line. `[&]` captures every
    // local variable by reference, so it can see `lines`.
    auto emit = [&](size_t a, size_t b, uint8_t style, char block, int h, bool first) {
        Line l;
        l.start = static_cast<uint32_t>(a);
        l.end = static_cast<uint32_t>(b);
        l.style = style;
        l.block = block;
        l.height = static_cast<uint8_t>(std::min(h, 255));
        l.firstInPara = first;
        lines.push_back(l);
    };

    size_t p = 0;                                   // start of current paragraph
    int imageNo = -1;                               // index of the last IMAGE paragraph
    while (p < n) {
        size_t pe = t.find('\n', p);                // paragraph end
        if (pe == std::string::npos) pe = n;

        char block = 0;
        size_t s = p;
        if (s < pe && isBlockCode(t[s])) block = t[s++];
        const int lh = m.lineHeight(block);
        const int avail = std::max(20, width - blockIndent(block));

        bool firstLine = true;
        // A picture: one tall line for the picture itself, then the caption
        // (the paragraph's text) laid out as small text below it. The picture
        // line starts at `p` (the block code), BEFORE the caption, so a saved
        // reading position that points at it still finds it.
        if (block == IMAGE) {
            ++imageNo;
            if (imageNo < static_cast<int>(doc.images.size()) && doc.images[imageNo].w) {
                int dw, dh;
                imageFit(doc.images[imageNo], width, dw, dh);
                emit(p, p, 0, block, dh + 6, true);
                lines.back().image = static_cast<int16_t>(imageNo);
                firstLine = false;                  // the caption is not "first"
            }
        }

        if (block == RULE || s >= pe) {
            // Horizontal rule, or an empty paragraph (a blank line in a text
            // file): a short gap with no text.
            emit(s, s, 0, block, block == RULE ? 10 : lh / 2, true);
        } else {
            // A view that ends at the paragraph end, so utf8::next can never
            // read into the next paragraph.
            const std::string_view para(t.data(), pe);

            uint8_t style = 0;          // style while scanning
            uint8_t lineStyle = 0;      // style at the start of the current line
            size_t lineStart = s;
            int x = 0;                  // pixels used on the current line
            size_t lastBreak = std::string::npos;   // best place to break so far
            uint8_t styleAtBreak = 0;
            uint32_t prev = 0;          // previous code point on this line
            bool first = firstLine;

            size_t i = s;
            while (i < pe) {
                const char c = t[i];
                if (isInlineCode(c) && c != BASE) { style = applyCode(style, c); ++i; continue; }

                // ---- measure the next UNIT: one character, or a whole ruby
                //      group BASE..RUBY..RUBY (which must never be split) ----
                const size_t ci = i;          // where the unit starts
                uint32_t firstCp = 0, lastCp = 0;
                int adv = 0;
                size_t next;                  // where the unit ends
                uint8_t styleAfter = style;
                size_t r1, r2;
                if (c == BASE) {
                    if (!findRubyGroup(t, i, pe, r1, r2)) { ++i; continue; }   // stray BASE
                    int baseW = 0, rubyW = 0;
                    for (size_t k = i + 1; k < r1;) {             // the base text
                        if (isInlineCode(t[k])) { styleAfter = applyCode(styleAfter, t[k]); ++k; continue; }
                        const uint32_t cp = utf8::next(para, k);
                        if (!firstCp) firstCp = cp;
                        lastCp = cp;
                        baseW += m.advance(cp, block, styleAfter);
                    }
                    for (size_t k = r1 + 1; k < r2;) {            // the ruby text
                        if (isInlineCode(t[k])) { ++k; continue; }
                        rubyW += m.advance(utf8::next(para, k), block, styleAfter | S_RUBY);
                    }
                    adv = m.showRuby() ? std::max(baseW, rubyW) : baseW;
                    next = r2 + 1;
                } else {
                    size_t k = i;
                    firstCp = lastCp = utf8::next(para, k);
                    const bool rubyHidden = (style & S_RUBY) && !m.showRuby();
                    adv = rubyHidden ? 0 : m.advance(firstCp, block, style);
                    next = k;
                }

                // Is it allowed to break the line just BEFORE this unit?
                // Never inside inline furigana (it must stay next to its kanji).
                if (ci > lineStart && !(style & S_RUBY) && firstCp != ' ') {
                    if (prev == ' ') {
                        lastBreak = ci; styleAtBreak = style;          // after a space
                    } else if ((utf8::isCJK(firstCp) || utf8::isCJK(prev)) &&
                               !noBreakBefore(firstCp) && !noBreakAfter(prev)) {
                        lastBreak = ci; styleAtBreak = style;          // between CJK
                    }
                }

                if (x + adv > avail && ci > lineStart) {
                    // Overflow. Break at the last opportunity, or – for one
                    // enormous word with no opportunity – right here.
                    size_t brk = ci;
                    uint8_t bs = style;
                    if (lastBreak != std::string::npos && lastBreak > lineStart) {
                        brk = lastBreak;
                        bs = styleAtBreak;
                    }
                    emit(lineStart, brk, lineStyle, block, lh, first);
                    first = false;
                    // Restart scanning at the break. Units between `brk` and
                    // `ci` will be measured again for the new line.
                    i = brk;
                    style = bs;
                    while (i < pe && t[i] == ' ') ++i;   // no leading spaces
                    lineStart = i;
                    lineStyle = style;
                    x = 0;
                    lastBreak = std::string::npos;
                    prev = 0;
                    continue;
                }
                x += adv;
                prev = lastCp;
                style = styleAfter;
                i = next;
            }
            emit(lineStart, pe, lineStyle, block, lh, first);
        }
        lines.back().height += kParagraphGap;   // space below the paragraph
        p = pe + 1;
    }
    return lines;
}

}  // namespace rt
