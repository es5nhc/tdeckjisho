#pragma once
// ============================================================================
//  Romaji.hpp – type Latin letters, get hiragana (a tiny IME)
// ============================================================================
//
//  The T-Deck keyboard only produces ASCII, but dictionary readings are kana.
//  We convert what the user types ("taberu") into hiragana ("たべる") and
//  search the kana index with that.
//
//  Accepted spellings (several systems are mixed freely):
//    Hepburn      shi chi tsu fu ja  sha cho
//    Kunrei       si  ti  tu  hu zya sya tyo
//    Wapuro       nn n' for ん, x/l prefix for small kana (xtu = っ, lya = ゃ)
//    Double consonant -> small っ:  kitte = きって, matcha = まっちゃ
//    '-' -> ー (long vowel mark in katakana words: ko-hi- = こーひー)
//
//  Searching for a long vowel needs the vowel spelled out as it is written
//  in kana: "toukyou" finds 東京 (とうきょう); "tokyo" does not.
// ============================================================================

#include <string>
#include <string_view>

namespace romaji {

struct Result {
    std::string kana;     // converted part, e.g. "たべ" for input "taber"
    std::string tail;     // trailing letters that do not form a syllable YET ("r")
    bool clean = true;    // false if unconvertible letters appear mid-word ("qqa")
};

Result toHiragana(std::string_view input);

}  // namespace romaji
