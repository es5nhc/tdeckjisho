#include "core/Romaji.hpp"

#include <cstring>

#include "core/Str.hpp"

namespace romaji {

namespace {   // anonymous namespace = "private to this .cpp file" (like Python's _name)

// One row of the conversion table. `const char*` string literals live in
// flash memory on the ESP32, so this table costs no RAM at all.
struct Pair {
    const char* roma;
    const char* kana;
};

// The table. Order does not matter: we always try the LONGEST match first
// (4 letters, then 3, 2, 1), so "kya" wins over "ky"+"a".
// `constexpr` makes the compiler build the array at compile time.
constexpr Pair kTable[] = {
    {"a", "あ"},   {"i", "い"},   {"u", "う"},   {"e", "え"},   {"o", "お"},
    {"ka", "か"},  {"ki", "き"},  {"ku", "く"},  {"ke", "け"},  {"ko", "こ"},
    {"kya", "きゃ"}, {"kyu", "きゅ"}, {"kyo", "きょ"},
    {"ga", "が"},  {"gi", "ぎ"},  {"gu", "ぐ"},  {"ge", "げ"},  {"go", "ご"},
    {"gya", "ぎゃ"}, {"gyu", "ぎゅ"}, {"gyo", "ぎょ"},
    {"sa", "さ"},  {"si", "し"},  {"shi", "し"}, {"su", "す"},  {"se", "せ"},  {"so", "そ"},
    {"sha", "しゃ"}, {"shu", "しゅ"}, {"sho", "しょ"}, {"she", "しぇ"},
    {"sya", "しゃ"}, {"syu", "しゅ"}, {"syo", "しょ"},
    {"za", "ざ"},  {"zi", "じ"},  {"ji", "じ"},  {"zu", "ず"},  {"ze", "ぜ"},  {"zo", "ぞ"},
    {"ja", "じゃ"}, {"ju", "じゅ"}, {"jo", "じょ"}, {"je", "じぇ"},
    {"jya", "じゃ"}, {"jyu", "じゅ"}, {"jyo", "じょ"},
    {"zya", "じゃ"}, {"zyu", "じゅ"}, {"zyo", "じょ"},
    {"ta", "た"},  {"ti", "ち"},  {"chi", "ち"}, {"tu", "つ"},  {"tsu", "つ"},
    {"te", "て"},  {"to", "と"},
    {"cha", "ちゃ"}, {"chu", "ちゅ"}, {"cho", "ちょ"}, {"che", "ちぇ"},
    {"tya", "ちゃ"}, {"tyu", "ちゅ"}, {"tyo", "ちょ"},
    {"cya", "ちゃ"}, {"cyu", "ちゅ"}, {"cyo", "ちょ"},
    {"tsa", "つぁ"}, {"thi", "てぃ"}, {"twu", "とぅ"},
    {"da", "だ"},  {"di", "ぢ"},  {"du", "づ"},  {"dzu", "づ"}, {"de", "で"},  {"do", "ど"},
    {"dya", "ぢゃ"}, {"dyu", "ぢゅ"}, {"dyo", "ぢょ"}, {"dhi", "でぃ"}, {"dwu", "どぅ"},
    {"na", "な"},  {"ni", "に"},  {"nu", "ぬ"},  {"ne", "ね"},  {"no", "の"},
    {"nya", "にゃ"}, {"nyu", "にゅ"}, {"nyo", "にょ"},
    {"ha", "は"},  {"hi", "ひ"},  {"hu", "ふ"},  {"fu", "ふ"},  {"he", "へ"},  {"ho", "ほ"},
    {"hya", "ひゃ"}, {"hyu", "ひゅ"}, {"hyo", "ひょ"},
    {"fa", "ふぁ"}, {"fi", "ふぃ"}, {"fe", "ふぇ"}, {"fo", "ふぉ"}, {"fyu", "ふゅ"},
    {"ba", "ば"},  {"bi", "び"},  {"bu", "ぶ"},  {"be", "べ"},  {"bo", "ぼ"},
    {"bya", "びゃ"}, {"byu", "びゅ"}, {"byo", "びょ"},
    {"pa", "ぱ"},  {"pi", "ぴ"},  {"pu", "ぷ"},  {"pe", "ぺ"},  {"po", "ぽ"},
    {"pya", "ぴゃ"}, {"pyu", "ぴゅ"}, {"pyo", "ぴょ"},
    {"ma", "ま"},  {"mi", "み"},  {"mu", "む"},  {"me", "め"},  {"mo", "も"},
    {"mya", "みゃ"}, {"myu", "みゅ"}, {"myo", "みょ"},
    {"ya", "や"},  {"yu", "ゆ"},  {"yo", "よ"},  {"ye", "いぇ"},
    {"ra", "ら"},  {"ri", "り"},  {"ru", "る"},  {"re", "れ"},  {"ro", "ろ"},
    {"rya", "りゃ"}, {"ryu", "りゅ"}, {"ryo", "りょ"},
    {"wa", "わ"},  {"wi", "うぃ"}, {"we", "うぇ"}, {"wo", "を"},
    {"va", "ゔぁ"}, {"vi", "ゔぃ"}, {"vu", "ゔ"},  {"ve", "ゔぇ"}, {"vo", "ゔぉ"},
    // Small kana, Wapuro style: x... or l...
    {"xa", "ぁ"},  {"xi", "ぃ"},  {"xu", "ぅ"},  {"xe", "ぇ"},  {"xo", "ぉ"},
    {"xya", "ゃ"}, {"xyu", "ゅ"}, {"xyo", "ょ"}, {"xtu", "っ"}, {"xtsu", "っ"}, {"xwa", "ゎ"},
    {"la", "ぁ"},  {"li", "ぃ"},  {"lu", "ぅ"},  {"le", "ぇ"},  {"lo", "ぉ"},
    {"lya", "ゃ"}, {"lyu", "ゅ"}, {"lyo", "ょ"}, {"ltu", "っ"}, {"ltsu", "っ"},
    {"nn", "ん"},  {"n'", "ん"},
    {"-", "ー"},
};

bool isVowel(char c) { return std::strchr("aeiou", c) != nullptr && c != 0; }
bool isLetter(char c) { return c >= 'a' && c <= 'z'; }

// Look up `s` (at most 4 chars) in the table. Linear search over ~200
// entries is a few microseconds – not worth a hash map here.
const char* lookup(std::string_view s) {
    for (const Pair& p : kTable)
        if (s == p.roma) return p.kana;
    return nullptr;
}

// Could `s` still become a syllable if more letters are typed? ("ky" -> yes)
bool isPrefixOfSomething(std::string_view s) {
    for (const Pair& p : kTable)
        if (str::startsWith(p.roma, s)) return true;
    return false;
}

}  // namespace

Result toHiragana(std::string_view input) {
    Result r;
    const std::string s = str::toLower(input);
    const size_t n = s.size();
    size_t i = 0;

    while (i < n) {
        const char c = s[i];
        const char nx = (i + 1 < n) ? s[i + 1] : '\0';   // '\0' = "nothing"

        // --- 1. Double consonant -> small tsu.  "kk" in "kitte", "pp" ... ----
        if (isLetter(c) && c == nx && !isVowel(c) && c != 'n') {
            r.kana += "っ";
            i += 1;                     // consume only the first of the pair
            continue;
        }
        // "tch" as in "matcha" is Hepburn's way of writing っち.
        if (c == 't' && nx == 'c' && i + 2 < n && s[i + 2] == 'h') {
            r.kana += "っ";
            i += 1;
            continue;
        }

        // --- 2. The many faces of ん --------------------------------------
        if (c == 'n') {
            if (nx == '\0') {           // trailing n: treat as ん for searching
                r.kana += "ん";
                i += 1;
                continue;
            }
            if (nx == '\'') { r.kana += "ん"; i += 2; continue; }
            if (nx == 'n') {
                // "nn" followed by a vowel/y: the first n is ん, the second
                // starts the next syllable: konnichiwa = こ ん に ち わ.
                const char after = (i + 2 < n) ? s[i + 2] : '\0';
                if (isVowel(after) || after == 'y') { r.kana += "ん"; i += 1; }
                else { r.kana += "ん"; i += 2; }   // "nn" = ん on its own
                continue;
            }
            if (!isVowel(nx) && nx != 'y') {      // n + consonant: さんぽ
                r.kana += "ん";
                i += 1;
                continue;
            }
            // otherwise: na/ni/nu/ne/no/nya... handled by the table below
        }

        // --- 3. Longest table match ----------------------------------------
        bool matched = false;
        for (size_t len = 4; len >= 1; --len) {
            if (i + len > n) continue;
            if (const char* kana = lookup(std::string_view(s).substr(i, len))) {
                r.kana += kana;
                i += len;
                matched = true;
                break;
            }
        }
        if (matched) continue;

        // --- 4. Nothing matched -------------------------------------------
        const std::string_view rest = std::string_view(s).substr(i);
        if (isPrefixOfSomething(rest)) {
            // e.g. "taber": the "r" is waiting for a vowel. Keep it apart so
            // the search can still use the "たべ" part.
            r.tail = std::string(rest);
            break;
        }
        if (isLetter(c)) r.clean = false;  // a letter that cannot be kana
        r.kana += c;                       // digits, spaces etc. pass through
        i += 1;
    }
    return r;
}

}  // namespace romaji
