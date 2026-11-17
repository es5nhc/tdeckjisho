#pragma once
// ============================================================================
//  Utf8.hpp – just enough UTF-8 handling for a dictionary and a text reader
// ============================================================================
//
//  In Python `str` is a sequence of code points and you never see bytes.
//  In C++ `std::string` is a sequence of BYTES. Japanese "日" is 3 bytes
//  (E6 97 A5), Estonian "õ" is 2 bytes (C3 B5), "a" is 1 byte.
//
//  So `s.size()` is the number of bytes, `s[1]` may be the middle of a
//  character, and cutting a string at an arbitrary byte can produce garbage.
//  These helpers walk the bytes and give you whole code points.
//
//  UTF-8 encoding, in one table (x = payload bits):
//      U+0000..U+007F    0xxxxxxx                              1 byte
//      U+0080..U+07FF    110xxxxx 10xxxxxx                     2 bytes
//      U+0800..U+FFFF    1110xxxx 10xxxxxx 10xxxxxx            3 bytes (all CJK)
//      U+10000..         11110xxx 10xxxxxx 10xxxxxx 10xxxxxx   4 bytes (emoji)
//
//  A very useful property: bytes 0x00..0x7F NEVER occur inside a multi-byte
//  character. That is why our rich-text format (text/RichText.hpp) can use
//  control bytes 0x01..0x1F as formatting codes safely.
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace utf8 {

constexpr uint32_t kReplacement = 0xFFFD;   // the "�" character

// Decode the code point that starts at s[i] and move i past it.
// Invalid bytes decode as U+FFFD and advance by one byte, so a loop like
//     for (size_t i = 0; i < s.size();) { uint32_t cp = utf8::next(s, i); }
// always terminates, even on binary garbage.
uint32_t next(std::string_view s, size_t& i);

// Start of the character that ends just before byte `i` (0 if i is 0 or inside the first
// character). The mirror image of next(): used to move a cursor LEFT.
size_t prev(std::string_view s, size_t i);

// Append the UTF-8 encoding of `cp` to `out`.
void append(std::string& out, uint32_t cp);

// True if the whole string is well-formed UTF-8.
bool isValid(std::string_view s);

// Largest prefix of `s` that is at most `maxBytes` long and does not cut a
// character in half.
std::string_view truncateBytes(std::string_view s, size_t maxBytes);

// Number of code points (what Python's len() would return).
size_t length(std::string_view s);

// ----- character classes used by line breaking and romaji conversion -------
inline bool isHiragana(uint32_t c) { return c >= 0x3041 && c <= 0x309F; }
inline bool isKatakana(uint32_t c) { return c >= 0x30A0 && c <= 0x30FF; }
inline bool isKanji(uint32_t c) {
    return (c >= 0x4E00 && c <= 0x9FFF)    // CJK Unified Ideographs
        || (c >= 0x3400 && c <= 0x4DBF)    // Extension A
        || (c >= 0xF900 && c <= 0xFAFF)    // Compatibility ideographs
        || c == 0x3005 || c == 0x3006      // 々 〆 (iteration / shime marks)
        || c == 0x30F6;                    // ヶ as in 一ヶ月
}
// "Wide" characters: no spaces between words, a line may break between any
// two of them (with exceptions, see text/Layout.cpp).
inline bool isCJK(uint32_t c) {
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0xFF00 && c <= 0xFFEF);
}

// カタカナ -> ひらがな (dictionary keys are stored in hiragana only).
std::string katakanaToHiragana(std::string_view s);

}  // namespace utf8
