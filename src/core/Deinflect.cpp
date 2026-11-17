#include "core/Deinflect.hpp"

#include <algorithm>

#include "core/Str.hpp"
#include "core/Utf8.hpp"

namespace deinflect {

namespace {

struct Rule {
    std::string_view ending;   // what the inflected word ends with
    std::string_view base;     // what to put there instead
    bool again;                // the result is a te/ta form: deinflect that too ("食べている")
};

// Godan verbs: the dictionary ending (う く ぐ す つ ぬ ぶ む る) and the vowel-i / vowel-a
// forms it turns into before ます / ない.
struct Godan { std::string_view u, i, a; };
constexpr Godan kGodan[] = {
    {"う", "い", "わ"}, {"く", "き", "か"}, {"ぐ", "ぎ", "が"}, {"す", "し", "さ"}, {"つ", "ち", "た"},
    {"ぬ", "に", "な"}, {"ぶ", "び", "ば"}, {"む", "み", "ま"}, {"る", "り", "ら"},
};

constexpr std::string_view kPolite[] = {"ませんでした", "ました", "ません", "ます"};
constexpr std::string_view kNegative[] = {"なかった", "ない", "なくて"};

void add(std::vector<std::string>& out, std::string s) {
    if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(std::move(s));
}

void apply(std::string_view word, std::vector<std::string>& out, int depth);

// stem + replacement, if `word` ends with `ending` and something is left of the stem.
bool strip(std::string_view word, std::string_view ending, std::string_view base, std::string& result) {
    if (word.size() <= ending.size() || !str::endsWith(word, ending)) return false;
    result = std::string(word.substr(0, word.size() - ending.size())) + std::string(base);
    return true;
}

void apply(std::string_view word, std::vector<std::string>& out, int depth) {
    std::string r;
    auto tryRule = [&](std::string_view ending, std::string_view base, bool again = false) {
        if (!strip(word, ending, base, r)) return;
        add(out, r);
        if (again && depth < 2) apply(r, out, depth + 1);
    };

    // -- the progressive "...ている / ていた / ています / ていません": strip it, then the te-form rules apply
    for (std::string_view aux : {"ていました", "ています", "ていません", "ている", "ていた", "てる", "ていない"})
        tryRule(aux, "て", true);
    for (std::string_view aux : {"でいました", "でいます", "でいる", "でいた"}) tryRule(aux, "で", true);

    // -- irregular する and 来る
    for (std::string_view e : {"しました", "します", "しません", "しない", "した", "して", "している", "しよう"})
        tryRule(e, "する");
    for (std::string_view e : {"来ました", "来ます", "来ません", "来ない", "来た", "来て"}) tryRule(e, "来る");
    for (std::string_view e : {"きました", "きます", "こない", "きた", "きて"}) tryRule(e, "くる");

    // -- godan: polite and negative forms
    for (const Godan& g : kGodan) {
        for (std::string_view p : kPolite) tryRule(std::string(g.i).append(p), g.u);
        for (std::string_view n : kNegative) tryRule(std::string(g.a).append(n), g.u);
        tryRule(std::string(g.a) + "れる", g.u);          // passive 書かれる
        tryRule(std::string(g.a) + "せる", g.u);          // causative 書かせる
    }
    // -- godan: past and te forms, and the conditional / potential
    for (std::string_view e : {"った", "って"}) { tryRule(e, "う"); tryRule(e, "つ"); tryRule(e, "る"); }
    for (std::string_view e : {"んだ", "んで"}) { tryRule(e, "む"); tryRule(e, "ぶ"); tryRule(e, "ぬ"); }
    for (std::string_view e : {"いた", "いて"}) tryRule(e, "く");
    for (std::string_view e : {"いだ", "いで"}) tryRule(e, "ぐ");
    for (std::string_view e : {"した", "して"}) tryRule(e, "す");
    for (const Godan& g : kGodan) {
        std::string e = std::string(g.u == "う" ? "え" : g.u == "く" ? "け" : g.u == "ぐ" ? "げ" : g.u == "す" ? "せ" :
                                    g.u == "つ" ? "て" : g.u == "ぬ" ? "ね" : g.u == "ぶ" ? "べ" : g.u == "む" ? "め" : "れ");
        tryRule(e + "ば", g.u);                              // 書けば -> 書く
        tryRule(e + "る", g.u);                              // 書ける -> 書く (potential)
    }

    // -- ichidan (食べる): the stem + る
    for (std::string_view p : kPolite) tryRule(p, "る");
    for (std::string_view n : kNegative) tryRule(n, "る");
    for (std::string_view e : {"られる", "させる", "られた", "れば", "よう", "た", "て", "ず"}) tryRule(e, "る");

    // -- adjectives (高い)
    for (std::string_view e : {"くなかった", "くない", "くなくて", "かった", "くて", "ければ", "さ", "く"}) tryRule(e, "い");
    tryRule("かったです", "い");
    tryRule("くないです", "い");
}

}  // namespace

std::vector<std::string> candidates(std::string_view word) {
    std::vector<std::string> out;
    if (utf8::length(word) < 2) return out;
    apply(word, out, 0);
    return out;
}

}  // namespace deinflect
