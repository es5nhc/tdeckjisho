#include "screens/QuizScreen.hpp"

#include <algorithm>
#include <vector>

#include "core/I18n.hpp"
#include "core/Utf8.hpp"
#include "hal/Keys.hpp"
#include "screens/Actions.hpp"
#include "ui/Draw.hpp"
#include "ui/Theme.hpp"

using i18n::fmt;
using i18n::Str;
using i18n::tr;

namespace {

constexpr uint16_t kWrongColour = theme::rgb(200, 0, 0);

// Layout inside the content area (y is relative to its top).
constexpr int kHeaderH = 18;
constexpr int kPromptY = kHeaderH;
constexpr int kPromptH = 50;
constexpr int kRowsY = kPromptY + kPromptH;
constexpr int kRowH = 35;
constexpr int kMargin = 4;

// Break `s` into at most `maxLines` lines that fit `maxW` pixels: at a space if there is
// one, anywhere otherwise (Japanese has no spaces). The last line is cut with "…".
std::vector<std::string> wrap(const lgfx::IFont* font, const std::string& s, int maxW, size_t maxLines) {
    std::vector<std::string> lines;
    std::string cur;
    int w = 0;
    size_t i = 0;
    while (i < s.size()) {
        const size_t start = i;
        const uint32_t cp = utf8::next(s, i);
        const int adv = draw::advance(font, cp);
        if (w + adv > maxW && !cur.empty()) {
            const size_t sp = cur.rfind(' ');
            std::string rest;
            if (sp != std::string::npos && sp > 0) {   // carry the unfinished word to the next line
                rest = cur.substr(sp + 1);
                cur.resize(sp);
            }
            lines.push_back(cur);
            cur = rest;
            w = draw::textWidth(font, cur);
        }
        if (cur.empty() && cp == ' ') continue;        // no space at the start of a line
        cur.append(s, start, i - start);
        w += adv;
    }
    if (!cur.empty()) lines.push_back(cur);
    if (lines.size() > maxLines) {                     // too long: the last allowed line gets the rest
        std::string last;
        for (size_t k = maxLines - 1; k < lines.size(); ++k) last += (k > maxLines - 1 ? " " : "") + lines[k];
        lines.resize(maxLines);
        lines.back() = draw::fit(font, last, maxW);
    }
    return lines;
}

}  // namespace

// ---------------------------------------------------------------------------
std::string QuizScreen::title() const {
    if (_state == State::Done) return tr(Str::HomeQuiz);
    return fmt(Str::QuizTitle, {{"n", std::to_string(_number)},
                                {"total", std::to_string(quiz::kQuestionsPerRound)}});
}

std::string QuizScreen::hints() const {
    switch (_state) {
        case State::Asking:   return tr(Str::QuizHintsAsk);
        case State::Answered: return tr(Str::QuizHintsNext);
        default:              return tr(Str::QuizHintsDone);
    }
}

void QuizScreen::onEnter(App& app) {
    if (_started) return;      // coming back from the entry screen: keep the question
    _started = true;
    startRound(app);
}

void QuizScreen::startRound(App& app) {
    _gen.newRound();
    _number = 0;
    _score = 0;
    nextQuestion(app);
}

void QuizScreen::nextQuestion(App& app) {
    if (_number >= quiz::kQuestionsPerRound) {
        _state = State::Done;
        app.redraw();
        return;
    }
    if (!_gen.next(_q)) {
        app.toast(tr(Str::QuizTooSmall));
        app.pop();
        return;
    }
    ++_number;
    _state = State::Asking;
    _selected = 0;
    _chosen = -1;
    _haveEntry = false;
    app.redraw();
}

void QuizScreen::answer(App& app, int choice) {
    _chosen = choice;
    const bool right = choice == _q.correct;
    if (right) ++_score;
    _gen.recordAnswer(_q, right);
    _haveEntry = app.dict().loadEntry(_q.offset, _entry);
    _state = State::Answered;
    app.redraw();
}

int QuizScreen::rowAt(int y) const {
    const int row = (y - theme::kContentY - kRowsY) / kRowH;
    return (y >= theme::kContentY + kRowsY && row >= 0 && row < quiz::kChoices) ? row : -1;
}

void QuizScreen::onKey(App& app, int k) {
    if (k == key::Backspace) { app.pop(); return; }
    switch (_state) {
        case State::Asking: {
            int choice = -1;
            if (k >= 'a' && k <= 'd') choice = k - 'a';
            else if (k == key::Up) { _selected = (_selected + quiz::kChoices - 1) % quiz::kChoices; app.redraw(); }
            else if (k == key::Down) { _selected = (_selected + 1) % quiz::kChoices; app.redraw(); }
            else if (k == key::Enter || k == key::Select) choice = _selected;
            if (choice >= 0) answer(app, choice);
            break;
        }
        case State::Answered:
            if (k == 'i') actions::openEntry(app, _q.offset);
            else if (k == key::Enter || k == key::Select || k == key::Right || k == key::Down) nextQuestion(app);
            break;
        case State::Done:
            if (k == key::Enter || k == key::Select) { app.pop(); return; } //We'll return to main menu after the quiz
            break;
    }
}

void QuizScreen::onTap(App& app, int, int y) {
    switch (_state) {
        case State::Asking: {
            const int row = rowAt(y);
            if (row >= 0) answer(app, row);
            break;
        }
        case State::Answered: nextQuestion(app); break;
        case State::Done: startRound(app); break;
    }
}

// ---------------------------------------------------------------------------
// Headword + reading on the first line, then the senses ("1. eat; live on  2. ...") in two
// lines of small text. The full entry (all senses, tags) is one key away: `i`.
void QuizScreen::drawDefinition(lgfx::LovyanGFX& g, const Rect& r) {
    const int x = r.x + 8, w = r.w - 16;
    const lgfx::IFont* uf = theme::fontUi();
    int y = r.y + kPromptY + 2;
    const int hw = draw::text(g, uf, draw::fit(uf, _entry.headword(), w), x, y, theme::kText, true);
    if (!_entry.kanji.empty()) {
        std::string reading;
        for (size_t i = 0; i < _entry.readings.size() && i < 2; ++i) reading += (i ? "・" : "") + _entry.readings[i];
        draw::text(g, uf, draw::fit(uf, reading, w - hw - 12), x + hw + 12, y, theme::kDim);
    }
    std::string meanings;
    for (size_t i = 0; i < _entry.senses.size(); ++i) {
        if (i) meanings += "  ";
        if (_entry.senses.size() > 1) meanings += std::to_string(i + 1) + ". ";
        const auto& gl = _entry.senses[i].glosses;
        for (size_t j = 0; j < gl.size(); ++j) meanings += (j ? "; " : "") + gl[j];
    }
    const lgfx::IFont* sf = theme::fontSmall();
    y += draw::height(uf) + 2;
    for (const auto& l : wrap(sf, meanings, w, 2)) {
        draw::text(g, sf, l, x, y, theme::kText);
        y += draw::height(sf);
    }
}

void QuizScreen::draw(App&, lgfx::LovyanGFX& g, const Rect& r) {
    if (_state == State::Done) {
        const std::string score = fmt(Str::QuizScore, {{"score", std::to_string(_score)},
                                                      {"total", std::to_string(quiz::kQuestionsPerRound)}});
        const lgfx::IFont* big = &fonts::lgfxJapanGothicP_24;
        const int w = draw::textWidth(big, score);
        draw::text(g, big, score, r.x + (r.w - w) / 2, r.y + r.h / 2 - 30, theme::kAccent, true);
        const std::string line = tr(Str::QuizScoreLine);
        draw::text(g, theme::fontUi(), line, r.x + (r.w - draw::textWidth(theme::fontUi(), line)) / 2,
                   r.y + r.h / 2 + 4, theme::kDim);
        return;
    }

    // Header: what is asked, and after the answer, right or wrong.
    const Str ask = _q.type == quiz::Type::Reading ? Str::QuizAskReading
                  : _q.type == quiz::Type::JaToEn  ? Str::QuizAskMeaning
                  : _q.type == quiz::Type::Kanji   ? Str::QuizAskKanji
                                                   : Str::QuizAskJapanese;
    draw::text(g, theme::fontSmall(), tr(ask), r.x + 6, r.y + 2, theme::kDim);
    if (_state == State::Answered) {
        const bool ok = _chosen == _q.correct;
        draw::textRight(g, theme::fontUi(), tr(ok ? Str::QuizCorrect : Str::QuizWrong), r.x + r.w - 6, r.y + 1,
                        ok ? theme::kOk : kWrongColour);
    }

    // The prompt: big for a Japanese word, normal for an English meaning. Once answered, the
    // same space shows the word's definition instead (the question is answered, and the
    // headword/meaning are part of the definition).
    if (_state == State::Answered && _haveEntry) {
        drawDefinition(g, r);
    } else {
    const bool japanese = _q.type != quiz::Type::EnToJa;
    const lgfx::IFont* pf = japanese ? &fonts::lgfxJapanGothicP_24 : theme::fontUi();
    const auto plines = wrap(pf, _q.prompt, r.w - 16, japanese ? 1 : 2);
    const int ph = draw::height(pf);
    int y = r.y + kPromptY + (kPromptH - ph * static_cast<int>(plines.size())) / 2;
    for (const auto& l : plines) {
        draw::text(g, pf, l, r.x + (r.w - draw::textWidth(pf, l)) / 2, y, theme::kText, true);
        y += ph;
    }
    }

    // The four choices.
    for (int i = 0; i < quiz::kChoices; ++i) {
        const int ry = r.y + kRowsY + i * kRowH;
        uint16_t bg = theme::kBg, fg = theme::kText;
        if (_state == State::Answered) {
            if (i == _q.correct) { bg = theme::kOk; fg = theme::kWhiteColour; }
            else if (i == _chosen) { bg = kWrongColour; fg = theme::kWhiteColour; }
        } else if (i == _selected) {
            bg = theme::kSelBg; fg = theme::kWhiteColour;
        }
        const int rw = r.w - 2 * kMargin;
        g.fillRect(r.x + kMargin, ry + 1, rw, kRowH - 2, bg);
        g.drawRect(r.x + kMargin, ry + 1, rw, kRowH - 2, theme::kRule);

        const int tx = r.x + kMargin + 26;
        const int tw = rw - 32;
        draw::text(g, theme::fontUi(), std::string(1, static_cast<char>('A' + i)), r.x + kMargin + 8, ry + (kRowH - 16) / 2, fg, true);
        // One line at 16 px if it fits, else two lines at 12 px.
        const lgfx::IFont* of = theme::fontUi();
        // A reading is a hint, so it appears only once the question is answered.
        std::string text = _q.options[i];
        if (_state == State::Answered && !_q.readings[i].empty()) text += " (" + _q.readings[i] + ")";
        auto lines = wrap(of, text, tw, 2);
        if (lines.size() > 1) {
            of = theme::fontSmall();
            lines = wrap(of, text, tw, 2);
        }
        const int oh = draw::height(of);
        int oy = ry + (kRowH - oh * static_cast<int>(lines.size())) / 2;
        for (const auto& l : lines) {
            draw::text(g, of, l, tx, oy, fg);
            oy += oh;
        }
    }
}
