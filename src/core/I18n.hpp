#pragma once
// ============================================================================
//  I18n.hpp – localization ("i18n": i + 18 letters + n): the UI in several languages
// ============================================================================
//
//  Every user-visible text lives in core/Strings.def, one row per text with all
//  languages side by side. Code never contains such a text; it names it:
//
//      draw::text(g, f, tr(Str::HomeDictionary), x, y, color);
//      app.toast(fmt(Str::WifiForgot, {{"ssid", net.ssid}}));
//      std::string n = trn(Str::EntriesOne, Str::EntriesMany, count);   // 1 entry / 5 entries
//
//  The texts are `const char*` literals in flash (zero RAM, like the romaji table),
//  and the language can be switched while the program runs: screens ask for their
//  text every time they draw. (A screen that copied a text at creation keeps the
//  old language, see ListScreen::setRebuild.)
//
//  Python comparison: gettext's `_("text")` looks a text up by its English
//  wording; here the key is an enum, so a typo is a COMPILE error, not a missing
//  translation noticed by a user.
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

namespace i18n {

enum class Lang : uint8_t { Et, En, Ja, Count };   // the column order of Strings.def

enum class Str : uint16_t {
#define X(id, et, en, ja) id,
#include "core/Strings.def"
#undef X
    Count
};

void setLanguage(Lang lang);
Lang language();

const char* code(Lang lang);          // "et", "en", "ja": what settings.txt stores
const char* nativeName(Lang lang);    // "Eesti", "English", "日本語": each in its own language
Lang fromCode(std::string_view code, Lang fallback);

// The text in the current language. An untranslated ("") row gives the English one.
const char* tr(Str id);
// The cell of the table as written, WITHOUT the English fallback ("" if untranslated).
// For tests and tools; the program itself uses tr().
const char* raw(Str id, Lang lang);

// One {name} -> value pair for fmt().
struct Arg {
    std::string_view name;
    std::string value;
};
// The text with every {name} replaced. An unknown {name} stays as it is, which is
// ugly on purpose: you will notice it. fmt(Str::X, {{"n", "5"}, {"who", name}})
std::string fmt(Str id, std::initializer_list<Arg> args);

// Which of the two plural forms a language uses for n: 0 = "one", 1 = "other".
// Estonian and English: exactly 1 -> 0, everything else -> 1. Japanese: always 0.
int pluralForm(Lang lang, long n);
// Picks `one` or `other` by pluralForm() and fills {n}.
std::string trn(Str one, Str other, long n);

}  // namespace i18n
