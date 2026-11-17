#pragma once
// ============================================================================
//  JmdictEncode.hpp – one JMdict entry -> entries.dat record + index keys
// ============================================================================
//
//  A line-by-line port of convert() in tools/jmdict2jisho.py. The goal is
//  BYTE-IDENTICAL output: tests/build_compare.sh builds the full dictionary
//  with both programs and runs `cmp`. That only works if every rule matches,
//  including the parts where Python quietly does something clever:
//
//    * g.lower() is UNICODE lower-casing. Two non-ASCII characters lower-case
//      to ASCII letters: K (U+212A KELVIN SIGN) -> "k", İ (U+0130) -> "i̇".
//      The regex [a-z0-9]+ then finds words the ASCII-only toLower() misses.
//    * s.strip() removes Unicode white space too (U+3000, U+00A0...).
//    * re.sub(r"\([^)]*\)", "", g) removes "(...)" WITHOUT nesting:
//      "a (b (c) d) e" -> "a  d) e".
//  pyLower(), pyStrip() and removeParens() below reproduce exactly that.
// ============================================================================

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dict/JmdictXml.hpp"

namespace build {

// ---- index format (must match tools/jmdict2jisho.py and Dictionary.cpp) ----
constexpr size_t kKeySize = 43;
constexpr size_t kRecordSize = kKeySize + 1 + 4;   // key | u8 score | u32 offset = 48
constexpr uint32_t kBlockRecords = 256;
constexpr uint16_t kIndexVersion = 2;

struct KeyScore {
    std::string key;     // already truncated to kKeySize bytes
    uint8_t score;
};

// Fill one 48-byte index record.
void makeRecord(uint8_t* rec, std::string_view key, uint8_t score, uint32_t offset);

// Build the entries.dat payload and the index keys of one entry.
// Returns false when the converter skips the entry (no reading, or no
// sense with an English gloss), exactly like `continue` in the Python loop.
bool encodeEntry(const JmEntry& e, std::string& payload, std::vector<KeyScore>& kana,
                 std::vector<KeyScore>& kanji, std::vector<KeyScore>& eng);

// Exposed for the unit tests.
std::string pyLower(std::string_view s);
std::string_view pyStrip(std::string_view s);
std::string glossCore(std::string_view gloss);
int priority(const std::vector<std::string>& pri);
bool isCommon(const std::vector<std::string>& pri);

}  // namespace build
