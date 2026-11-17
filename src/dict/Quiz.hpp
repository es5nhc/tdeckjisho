#pragma once
// ============================================================================
//  Quiz.hpp – multiple-choice questions made from dictionary entries
// ============================================================================
//
//  No drawing here (like everything in dict/): the QuizScreen shows what this makes,
//  and tests/test_core.cpp can run it against the 42-word sample dictionary.
//
//  Three kinds of question, four choices each:
//    Reading   a word written with kanji  ->  which reading is right?
//    JaToEn    a Japanese word            ->  which English meaning is right?
//    EnToJa    an English meaning         ->  which Japanese word is right?
//    Kanji     a reading in hiragana      ->  which kanji spelling is right?  (たべる -> 食べる)
//
//  The wrong choices ("distractors") are other random entries. A candidate is refused
//  when it could ALSO be a right answer (same reading, a shared meaning, the same
//  written form), because "wrong" answers that are correct make a quiz unfair.
// ============================================================================

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "core/Settings.hpp"
#include "dict/Dictionary.hpp"

namespace quiz {

constexpr int kChoices = 4;
constexpr int kQuestionsPerRound = 20;
// A question (one word, one kind of question) answered right this many times IN A ROW is
// considered learned and is not asked any more. A wrong answer starts the count again.
constexpr int kMasteredStreak = 10;

enum class Type { Reading, JaToEn, EnToJa, Kanji };

struct Question {
    Type type = Type::Reading;
    std::string prompt;                 // the word (Reading, JaToEn), the hiragana reading (Kanji) or the meaning (EnToJa)
    std::string options[kChoices];
    std::string readings[kChoices];     // EnToJa: reading of a kanji option, shown only AFTER the answer
    int correct = 0;                    // index of the right option
    uint32_t offset = 0;                // the entry in entries.dat, to show it after the answer
    std::string progressKey;            // identifies the question in the streak store
};

class Generator {
public:
    // `progress` (may be null) stores the streaks: key -> number of right answers in a row.
    Generator(Dictionary& dict, uint32_t seed, Settings* progress = nullptr)
        : _dict(dict), _rng(seed), _progress(progress) {}

    // Make the next question. False if the dictionary cannot supply one (too few entries).
    bool next(Question& q);
    // Tell the generator how the question was answered: updates (and saves) the streak.
    void recordAnswer(const Question& q, bool correct);
    int streak(const std::string& progressKey) const;
    // Forget which words were asked (a new round).
    void newRound() { _used.clear(); }

private:
    bool tryBuild(Type type, Question& q);
    bool pickEntry(DictEntry& e, uint32_t& offset, bool common, bool needKanji, bool avoidUsed);

    Dictionary& _dict;
    std::mt19937 _rng;
    Settings* _progress;
    std::vector<uint32_t> _used;        // offsets asked in this round
};

// Text shown for an entry as an option / prompt (exposed for tests).
std::string meaningText(const DictEntry& e);   // first sense, up to two glosses
std::string wordText(const DictEntry& e);      // the headword: "食べる" or "コーヒー" (no reading: it would give hints away)
std::string wordReading(const DictEntry& e);   // "たべる" for a word with kanji, "" for a kana-only word

}  // namespace quiz
