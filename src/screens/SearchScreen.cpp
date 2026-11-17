#include "screens/SearchScreen.hpp"
#include "core/I18n.hpp"

#include <algorithm>

#include "core/Romaji.hpp"
#include "hal/Hal.hpp"
#include "hal/Keys.hpp"
#include "screens/Actions.hpp"
#include "ui/Draw.hpp"
#include "ui/Theme.hpp"

using i18n::fmt;
using i18n::Str;
using i18n::tr;

namespace {
constexpr uint32_t kDebounceMs = 150;
constexpr size_t kMaxResults = 40;
constexpr int kListY = 50;          // list starts below the input + preview
}

std::string SearchScreen::hints() const {
    return tr(Str::SearchHints);
}

void SearchScreen::onEnter(App& app) {
    _list.twoLine = true;
    // Remember the last mode across screens/boots.
    _mode = static_cast<Mode>(std::clamp(app.settings().getInt("dict_mode", 0), 0, 2));
}

void SearchScreen::onKey(App& app, int k) {
    const int rows = _list.rowsIn(theme::kContentH - kListY);
    if (k == key::Left || k == key::Right) {
        const int m = (static_cast<int>(_mode) + (k == key::Right ? 1 : 2)) % 3;
        _mode = static_cast<Mode>(m);
        app.settings().setInt("dict_mode", m);
        app.settings().save();
        _pending = true;
        _lastEdit = 0;              // search immediately
    } else if (k == key::Up || k == key::Down) {
        _list.onKey(k, rows);
        ensureDescribed(app);   // scrolling may reveal rows not loaded yet
    } else if (k == key::Enter || k == key::Select) {
        if (_pending) runSearch(app);
        if (!_hits.empty()) actions::openEntry(app, _hits[_list.selected].offset);
    } else if (k == key::Backspace && _field.text.empty()) {
        app.pop();
        return;
    } else if (_field.onKey(k)) {
        _pending = true;
        _lastEdit = app.now();
        // Show how the romaji was understood – but only if it IS romaji.
        // "ice cream" would come out as "いcえ cれあm": noise, not help.
        const romaji::Result r = romaji::toHiragana(_field.text);
        _kanaPreview = r.clean ? r.kana + r.tail : "";
        _notRomaji = !r.clean;
    } else {
        return;
    }
    app.redraw();
}

void SearchScreen::update(App& app) {
    if (_pending && app.now() - _lastEdit >= kDebounceMs) runSearch(app);
}

void SearchScreen::runSearch(App& app) {
    _pending = false;
    _hits.clear();
    _list.items.clear();
    _list.selected = 0;
    Dictionary& dict = app.dict();
    if (_field.text.empty() || !dict.isOpen()) { app.redraw(); return; }

    const uint32_t t0 = hal::millis();
    std::vector<DictHit> ja, en;

    const romaji::Result r = romaji::toHiragana(_field.text);
    // describeFirst = 0: rank now, load entry text later, only for the rows
    // that are actually shown (ensureDescribed below). On the SD card that
    // turns ~40 entry reads per keystroke into ~8.
    if (_mode != Mode::EnJa && r.clean && !r.kana.empty())
        ja = dict.searchKana(r.kana, kMaxResults, 0);
    if (_mode != Mode::JaEn)
        en = dict.searchEnglish(_field.text, kMaxResults, 0);

    // Merge: Japanese hits first, then English, without duplicates. In Auto
    // mode an exact English hit beats an inexact kana hit ("eat" should show
    // 食べる before エアコン...), so we stable-sort exact matches to the top.
    _hits = std::move(ja);
    for (auto& h : en) {
        const bool dup = std::any_of(_hits.begin(), _hits.end(),
                                     [&](const DictHit& x) { return x.offset == h.offset; });
        if (!dup) _hits.push_back(std::move(h));
    }
    std::stable_sort(_hits.begin(), _hits.end(),
                     [](const DictHit& a, const DictHit& b) { return a.exact && !b.exact; });
    if (_hits.size() > kMaxResults) _hits.resize(kMaxResults);

    ensureDescribed(app);
    hal::log("[search] '%s' -> %u hits in %u ms\n", _field.text.c_str(),
             static_cast<unsigned>(_hits.size()), static_cast<unsigned>(hal::millis() - t0));
    app.redraw();
}

void SearchScreen::ensureDescribed(App& app) {
    // Visible rows + a few ahead, so scrolling down one row is instant.
    const size_t want = static_cast<size_t>(_list.selected) + 8;
    app.dict().describe(_hits, 0, want);

    const int keep = _list.selected;
    _list.items.clear();
    for (const DictHit& h : _hits) {
        ListItem it;
        if (h.described) {
            it.primary = h.headword;
            if (!h.reading.empty()) it.primary += "【" + h.reading + "】";
            if (h.common) it.primary += " ★";
            it.secondary = h.gloss;
        } else {
            it.primary = "…";            // not loaded yet (never visible for long)
        }
        it.accent = h.exact;
        _list.items.push_back(std::move(it));
    }
    _list.selected = keep;
}

void SearchScreen::draw(App& app, lgfx::LovyanGFX& g, const Rect& r) {
    // ---- mode chip + input field ---------------------------------------
    static const char* const kModeLabel[] = {"自動", "和英", "英和"};
    const lgfx::IFont* f = theme::fontUi();
    const int chipW = 40;
    g.fillRoundRect(r.x + 4, r.y + 4, chipW, 22, 4, theme::kAccent);
    draw::text(g, f, kModeLabel[static_cast<int>(_mode)], r.x + 8, r.y + 7, theme::kBg);
    _field.draw(g, r.x + chipW + 8, r.y + 4, r.w - chipW - 12);

    // ---- kana preview: shows how your romaji was understood -------------
    if (_mode != Mode::EnJa && !_kanaPreview.empty())
        draw::text(g, theme::fontSmall(), "→ " + _kanaPreview, r.x + chipW + 12, r.y + 30, theme::kDim);

    const Rect listArea{r.x, r.y + kListY, r.w, r.h - kListY};
    if (!app.dict().isOpen()) {
        draw::message(g, listArea.x, listArea.y, listArea.w, listArea.h,
                      fmt(Str::SearchNotFound, {{"err", app.dict().error()}}),
                      theme::kAccent);
    } else if (_field.text.empty()) {
        if (_mode == Mode::Auto){
            draw::message(g, listArea.x, listArea.y, listArea.w, listArea.h,
                        "taberu · shinbun · ko-hi- · eat · ice cream", theme::kDim);
        }
        else if (_mode == Mode::EnJa){
            draw::message(g, listArea.x, listArea.y, listArea.w, listArea.h, 
                         "eat ・ ice cream", theme::kDim);    
        }
        else{
            draw::message(g, listArea.x, listArea.y, listArea.w, listArea.h,
                        "taberu ・ shinbun ・ ko-hi-", theme::kDim);
        }
    } else if (_list.items.empty() && !_pending) {
        const bool hint = _notRomaji && _mode == Mode::JaEn;
        draw::message(g, listArea.x, listArea.y, listArea.w, listArea.h,
                      tr(hint ? Str::SearchNoResultsRomaji : Str::SearchNoResults),
                      theme::kDim);
    } else {
        _list.draw(g, listArea);
    }
}
