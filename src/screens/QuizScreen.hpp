#pragma once
// ============================================================================
//  QuizScreen – 20 multiple-choice questions made from the dictionary
// ============================================================================
//
//  The questions come from dict/Quiz (no drawing there). This screen is a small
//  state machine:
//
//     Asking  --answer-->  Answered  --Enter-->  Asking ... (20 times)  -->  Done
//       ^                  (right = green, your wrong pick = red)            |
//       +------------------------------ Enter: a new round -------------------+
//
//  Keys: a-d answer (letters: digits need the Sym key on the T-Deck), ↑↓ + Enter answer too, a tap on a choice answers.
//  After the answer the prompt gives way to the word's definition (headword, reading, meanings);
//  Enter / tap = next question, `i` = open the full entry.
// ============================================================================

#include <string>

#include "dict/Quiz.hpp"
#include "ui/App.hpp"

class QuizScreen : public Screen {
public:
    // `dict` is App's dictionary, which outlives every screen.
    // `progress` = App::quizProgress(): the streaks of right answers.
    QuizScreen(Dictionary& dict, uint32_t seed, Settings* progress) : _gen(dict, seed, progress) {}

    std::string title() const override;
    std::string hints() const override;
    void onEnter(App& app) override;
    void onKey(App& app, int key) override;
    void onTap(App& app, int x, int y) override;
    void draw(App& app, lgfx::LovyanGFX& g, const Rect& r) override;

private:
    enum class State { Asking, Answered, Done };

    void startRound(App& app);
    void nextQuestion(App& app);
    void answer(App& app, int choice);
    int rowAt(int y) const;
    void drawDefinition(lgfx::LovyanGFX& g, const Rect& r);

    quiz::Generator _gen;
    quiz::Question _q;
    DictEntry _entry;      // the word of _q, loaded when it is answered (shown as its definition)
    bool _haveEntry = false;
    State _state = State::Asking;
    bool _started = false;
    int _number = 0;       // 1-based number of the current question
    int _score = 0;
    int _selected = 0;     // highlighted choice while asking
    int _chosen = -1;      // the choice that was answered
};
