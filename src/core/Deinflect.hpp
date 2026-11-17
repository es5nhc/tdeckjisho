#pragma once
// ============================================================================
//  Deinflect.hpp – from "食べました" back to the dictionary word "食べる"
// ============================================================================
//
//  A dictionary lists 食べる, but a text says 食べた, 食べます, 食べない, 食べて...
//  Japanese verbs and adjectives change their ENDING. A look-up that only finds
//  exact words would fail on most of what you read. De-inflection is the reverse
//  of conjugation: take off a known ending, put the dictionary ending back.
//
//      食べました   ending ました  -> る     =>  食べる      (a candidate)
//      書いた       ending いた    -> く     =>  書く
//      高かった     ending かった  -> い     =>  高い
//
//  This is a TABLE of (ending, dictionary ending) pairs, tried against the end
//  of the word. It over-generates on purpose: a candidate that is not a word
//  is simply not found in the dictionary, which is the real test. (It would need
//  a part-of-speech check to be exact; the answer for a reader is "show it, with
//  the form it came from".)
// ============================================================================

#include <string>
#include <string_view>
#include <vector>

namespace deinflect {

// Possible dictionary forms of `word`, most plausible first, without `word` itself.
// Empty for words of fewer than 2 characters.
std::vector<std::string> candidates(std::string_view word);

}  // namespace deinflect
