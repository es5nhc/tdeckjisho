#include "dict/Quiz.hpp"

#include <algorithm>

#include "core/Str.hpp"
#include "core/Utf8.hpp"

namespace quiz {

namespace {

constexpr size_t kMaxMeaningChars = 48;   // longer lists of glosses get cut after a gloss

bool hasGloss(const DictEntry& e, const std::string& lowerGloss) {
    for (const auto& s : e.senses)
        for (const auto& g : s.glosses)
            if (str::toLower(g) == lowerGloss) return true;
    return false;
}

bool sharesGloss(const DictEntry& a, const DictEntry& b) {
    for (const auto& s : a.senses)
        for (const auto& g : s.glosses)
            if (hasGloss(b, str::toLower(g))) return true;
    return false;
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

// Could `cand` ALSO be a right answer to a question about `target`?
bool conflicts(const DictEntry& target, const DictEntry& cand, Type type) {
    if (cand.headword() == target.headword()) return true;
    switch (type) {
        case Type::Reading:   // another word read the same way (homophone)
        case Type::Kanji:     // same: a homophone is another valid spelling of the reading
            for (const auto& r : cand.readings)
                if (contains(target.readings, r)) return true;
            return false;
        case Type::JaToEn:
        case Type::EnToJa:
            return sharesGloss(target, cand);
    }
    return false;
}

bool usable(const DictEntry& e) {
    return !e.readings.empty() && !e.senses.empty() && !e.senses[0].glosses.empty();
}

}  // namespace

std::string meaningText(const DictEntry& e) {
    std::string out;
    if (e.senses.empty()) return out;
    const auto& glosses = e.senses[0].glosses;
    for (size_t i = 0; i < glosses.size() && i < 2; ++i) {
        if (i && utf8::length(out) + utf8::length(glosses[i]) > kMaxMeaningChars) break;
        if (i) out += "; ";
        out += glosses[i];
    }
    return out;
}

std::string wordText(const DictEntry& e) {
    return e.headword();
}

std::string wordReading(const DictEntry& e) {
    return e.kanji.empty() ? std::string() : e.readings.at(0);
}

int Generator::streak(const std::string& progressKey) const {
    return _progress ? _progress->getInt(progressKey, 0) : 0;
}

void Generator::recordAnswer(const Question& q, bool correct) {
    if (!_progress || q.progressKey.empty()) return;
    const int n = correct ? std::min(streak(q.progressKey) + 1, kMasteredStreak) : 0;
    if (n > 0) _progress->setInt(q.progressKey, n);
    else _progress->erase(q.progressKey);      // no entry = streak 0: the file only holds what matters
    _progress->save();
}

bool Generator::pickEntry(DictEntry& e, uint32_t& offset, bool common, bool needKanji, bool avoidUsed) {
    if (!_dict.randomEntry(_rng(), e, offset)) return false;
    if (!usable(e)) return false;
    if (common && !e.common) return false;
    if (needKanji && e.kanji.empty()) return false;
    if (avoidUsed && std::find(_used.begin(), _used.end(), offset) != _used.end()) return false;
    return true;
}

bool Generator::tryBuild(Type type, Question& q) {
    // 1. The word the question is about. Common words first: a quiz of obscure words is no fun.
    const bool kanjiType = type == Type::Reading || type == Type::Kanji;   // needs a word with kanji
    DictEntry target;
    uint32_t offset = 0;
    bool found = false;
    std::string progressKey;
    for (int attempt = 0; attempt < 60 && !found; ++attempt) {
        found = pickEntry(target, offset, /*common=*/attempt < 30, /*needKanji=*/kanjiType,
                          /*avoidUsed=*/attempt < 45);
        if (!found) continue;
        // "k|食べる|たべる": the kind of question + the word. Made from the TEXT, not the file
        // offset, so the streaks survive a dictionary update.
        progressKey = std::string(1, static_cast<char>('0' + static_cast<int>(type))) + "|" +
                      target.headword() + "|" + target.readings[0];
        if (streak(progressKey) >= kMasteredStreak) found = false;   // learned: ask something else
    }
    if (!found) return false;

    auto optionOf = [type](const DictEntry& e) {
        switch (type) {
            case Type::Reading: return e.readings.at(0);
            case Type::JaToEn:  return meaningText(e);
            case Type::Kanji:   return e.kanji.empty() ? std::string() : e.kanji[0];
            default:            return wordText(e);
        }
    };

    // 2. Three wrong choices that differ from each other and from the right one.
    std::vector<std::string> options{optionOf(target)};
    std::vector<std::string> readings{wordReading(target)};
    for (int attempt = 0; attempt < 120 && options.size() < static_cast<size_t>(kChoices); ++attempt) {
        DictEntry cand;
        uint32_t o = 0;
        // Early attempts want look-alikes: common words, with kanji when the question has some.
        const bool strict = attempt < 40;
        if (!pickEntry(cand, o, strict, type == Type::Kanji || (strict && type == Type::Reading), false)) continue;
        if (o == offset || conflicts(target, cand, type)) continue;
        const std::string text = optionOf(cand);
        if (text.empty() || contains(options, text)) continue;
        options.push_back(text);
        readings.push_back(wordReading(cand));
    }
    if (options.size() < static_cast<size_t>(kChoices)) return false;

    // 3. Shuffle: the right answer must not always be first.
    q = Question{};
    q.type = type;
    q.offset = offset;
    q.progressKey = progressKey;
    q.prompt = type == Type::Reading ? target.kanji[0]
             : type == Type::Kanji   ? target.readings[0]
             : type == Type::JaToEn  ? target.headword()
                                     : meaningText(target);
    std::vector<int> order{0, 1, 2, 3};   // order[i] = which candidate goes to slot i
    std::shuffle(order.begin(), order.end(), _rng);
    for (int i = 0; i < kChoices; ++i) {
        q.options[i] = options[order[i]];
        if (type == Type::EnToJa) q.readings[i] = readings[order[i]];
        if (order[i] == 0) q.correct = i;
    }
    _used.push_back(offset);
    return !q.prompt.empty();
}

bool Generator::next(Question& q) {
    for (int tries = 0; tries < 8; ++tries) {
        const Type type = static_cast<Type>(_rng() % 4);
        if (tryBuild(type, q)) return true;
    }
    return false;
}

}  // namespace quiz
