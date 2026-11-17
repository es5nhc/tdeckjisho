#pragma once
// ============================================================================
//  Cursor.hpp – a text cursor over a laid-out document, in pure C++
// ============================================================================
//
//  Selecting a word to look up needs two translations the layout does not give:
//
//    pixel  <->  character      "which character is under x = 120 on this line?"
//    offset ->   next / previous character, skipping what is not text
//
//  The layout (text/Layout.hpp) stores only BYTE RANGES per line, not where each
//  character sits. A line's characters are measured again here, with the same
//  Metrics the layout and the renderer use, so all three agree.
//
//  A "unit" is what the cursor can stand on: one character, or a whole furigana
//  group BASE 漢字 RUBY かんじ RUBY (the cursor stands on the kanji; the reading is not
//  text you look up). Cursor positions are always the START byte of a unit.
//
//  Nothing here touches the display or the SD card, so tests/test_core.cpp can
//  check it with FakeMetrics.
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "text/Layout.hpp"
#include "text/RichText.hpp"

namespace rt {

struct Unit {
    uint32_t start = 0, end = 0;   // byte range in doc.text
    int x = 0, w = 0;              // pixels from the start of the line's text (after the indent)
    bool selectable = true;        // false: inline furigana text, shown but never looked up
};

// All units of one laid-out line, left to right. Empty for a picture or a rule.
std::vector<Unit> lineUnits(const Doc& doc, const Line& line, Metrics& m);

// Index of the unit that covers pixel x (the first/last one if x is outside the line);
// -1 if the line has no selectable unit. For moving the cursor up/down: keep the column.
int unitAtX(const std::vector<Unit>& units, int x);
// The same for a TAP: only a unit that is really under x (give or take `slack` pixels for
// a fingertip); -1 for the empty space after the end of a line or in the margin.
int unitUnderX(const std::vector<Unit>& units, int x, int slack = 3);

constexpr size_t kNoOffset = static_cast<size_t>(-1);

// The first unit start at or after `from` (skipping codes, line ends and furigana text).
size_t firstUnitAtOrAfter(const Doc& doc, size_t from);
// The unit start after the one at `off`, crossing paragraphs; kNoOffset at the end.
size_t nextOffset(const Doc& doc, size_t off);
// The unit start before `off`, crossing paragraphs; kNoOffset at the start.
size_t prevOffset(const Doc& doc, size_t off);

// If `off` is inside a Latin word (ASCII letters), the offset of the word's first letter; else `off`.
// For a look-up of "practice" when the cursor stands on its "a".
size_t latinWordStart(const Doc& doc, size_t off);

// The plain text from `off`: up to `maxChars` characters, WITHOUT control codes and furigana
// readings, ending at the paragraph end. If `endOffset` is given it receives the byte offset
// just after the last character taken (to mark the span on screen).
std::string plainTextFrom(const Doc& doc, size_t off, size_t maxChars, size_t* endOffset = nullptr);

// The plain text just BEFORE `off`, in the same paragraph: up to about `maxChars` characters
// (a furigana group counts as one step back). For the look-up: 3匹 with the cursor on 匹 -> "3".
std::string plainTextBefore(const Doc& doc, size_t off, size_t maxChars);

}  // namespace rt
