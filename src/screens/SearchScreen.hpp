#pragma once
// ============================================================================
//  SearchScreen – the dictionary
// ============================================================================
//
//  Type romaji or English. The search runs automatically 150 ms after the
//  last key press ("debouncing"), so fast typing does not trigger a search
//  for every letter.
//
//  Modes (← → to switch):
//    自動 Auto  – search both: as reading (romaji -> kana) AND as English
//    和英 JA→EN – reading only     英和 EN→JA – English only
// ============================================================================

#include "core/I18n.hpp"
#include <vector>

#include "dict/Dictionary.hpp"
#include "ui/App.hpp"
#include "ui/ListView.hpp"
#include "ui/TextField.hpp"

class SearchScreen : public Screen {
public:
    std::string title() const override { return i18n::tr(i18n::Str::HomeDictionary); }
    std::string hints() const override;
    void onEnter(App& app) override;
    void onKey(App& app, int key) override;
    void update(App& app) override;
    void draw(App& app, lgfx::LovyanGFX& g, const Rect& r) override;

private:
    // `enum class` = scoped, strongly typed enum (Mode::Auto, not just Auto),
    // and it does not silently convert to int. Like a Rust enum without data.
    enum class Mode { Auto, JaEn, EnJa };

    void runSearch(App& app);
    // Make sure the rows the user can see (plus a few ahead) have their
    // text loaded; loads only what is missing. Rebuilds the list items.
    void ensureDescribed(App& app);

    TextField _field;
    Mode _mode = Mode::Auto;
    std::string _kanaPreview;
    std::vector<DictHit> _hits;
    ListView _list;
    bool _notRomaji = false;        // input contains letters that are not romaji
    bool _pending = false;          // a search is due (debounce timer running)
    uint32_t _lastEdit = 0;
};
