#include "core/I18n.hpp"

#include <cstring>

namespace i18n {

namespace {

// The table: one row per Str, three columns per row. Made from the same
// Strings.def as the enum, so the two cannot get out of step.
constexpr const char* kTable[][static_cast<size_t>(Lang::Count)] = {
#define X(id, et, en, ja) {et, en, ja},
#include "core/Strings.def"
#undef X
};

// If this fails, a row has the wrong number of languages.
static_assert(sizeof kTable / sizeof kTable[0] == static_cast<size_t>(Str::Count),
              "Strings.def and the table disagree");

Lang g_lang = Lang::En;

constexpr const char* kCodes[] = {"et", "en", "ja"};
constexpr const char* kNames[] = {"Eesti", "English", "日本語"};

}  // namespace

void setLanguage(Lang lang) {
    if (lang < Lang::Count) g_lang = lang;
}

Lang language() { return g_lang; }

const char* code(Lang lang) { return kCodes[static_cast<size_t>(lang)]; }
const char* nativeName(Lang lang) { return kNames[static_cast<size_t>(lang)]; }

Lang fromCode(std::string_view c, Lang fallback) {
    for (size_t i = 0; i < static_cast<size_t>(Lang::Count); ++i)
        if (c == kCodes[i]) return static_cast<Lang>(i);
    return fallback;
}

const char* tr(Str id) {
    const size_t row = static_cast<size_t>(id);
    if (row >= static_cast<size_t>(Str::Count)) return "";
    const char* text = kTable[row][static_cast<size_t>(g_lang)];
    if (text[0] == '\0') text = kTable[row][static_cast<size_t>(Lang::En)];   // not translated yet
    return text;
}

const char* raw(Str id, Lang lang) {
    const size_t row = static_cast<size_t>(id);
    if (row >= static_cast<size_t>(Str::Count) || lang >= Lang::Count) return "";
    return kTable[row][static_cast<size_t>(lang)];
}

std::string fmt(Str id, std::initializer_list<Arg> args) {
    const std::string_view text = tr(id);
    std::string out;
    out.reserve(text.size() + 16);
    for (size_t i = 0; i < text.size();) {
        if (text[i] == '{') {
            const size_t close = text.find('}', i);
            if (close != std::string_view::npos) {
                const std::string_view name = text.substr(i + 1, close - i - 1);
                bool found = false;
                for (const Arg& a : args) {
                    if (a.name == name) { out += a.value; found = true; break; }
                }
                if (found) { i = close + 1; continue; }
            }
        }
        out += text[i++];
    }
    return out;
}

int pluralForm(Lang lang, long n) {
    if (lang == Lang::Ja) return 0;      // Japanese has no plural: counters instead
    return n == 1 ? 0 : 1;               // Estonian: 1 kirje / 5 kirjet. English: 1 entry / 5 entries
}

std::string trn(Str one, Str other, long n) {
    return fmt(pluralForm(g_lang, n) == 0 ? one : other, {{"n", std::to_string(n)}});
}

}  // namespace i18n
