#pragma once
// ============================================================================
//  ListScreen – a generic menu: title + list + "what happens on Enter"
// ============================================================================
//
//  Many screens are "pick one item from a list": the home menu, feeds,
//  articles in a feed, links on a page. Instead of a class for each, ONE
//  class takes the behaviour as a function object (std::function).
//
//  Python:  ListScreen("Feeds", items, on_select=lambda app, i: ...)
//  Rust:    a struct holding a Box<dyn Fn(&mut App, usize)>
//
//  The lambdas you pass in usually CAPTURE data (e.g. the list of URLs), and
//  std::function stores that captured data inside itself, so it stays alive
//  as long as the screen does.
// ============================================================================

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core/I18n.hpp"
#include "hal/Keys.hpp"
#include "ui/App.hpp"
#include "ui/Draw.hpp"
#include "ui/ListView.hpp"
#include "ui/Theme.hpp"

class ListScreen : public Screen {
public:
    using SelectFn = std::function<void(App&, int index)>;
    // Extra keys (e.g. shortcuts). Return true if handled.
    using KeyFn = std::function<bool(App&, ListScreen&, int key)>;

    ListScreen(std::string title, std::vector<ListItem> items, SelectFn onSelect, bool twoLine = false)
        : _title(std::move(title)), _onSelect(std::move(onSelect)) {
        _list.items = std::move(items);
        _list.twoLine = twoLine;
    }

    void setHints(std::string h) { _hints = std::move(h); }
    void setEmptyText(std::string t) { _empty = std::move(t); }
    // A screen that stays on the stack while the language changes (the Home menu)
    // copied its texts when it was created. This hook runs every time the screen
    // becomes the top screen again, so it can refill its items with the new language.
    using RebuildFn = std::function<void(ListScreen&)>;
    void setRebuild(RebuildFn fn) { _rebuild = std::move(fn); _rebuild(*this); }
    void setKeyHandler(KeyFn fn) { _onKeyExtra = std::move(fn); }
    void setRoot(bool root) { _root = root; }
    ListView& list() { return _list; }

    std::string title() const override { return _title; }
    std::string hints() const override {
        return _hints.empty() ? i18n::tr(i18n::Str::HintListDefault) : _hints;
    }

    void onEnter(App&) override {
        if (_rebuild) { _rebuild(*this); _list.clampSelection(); }
    }

    void onKey(App& app, int k) override {
        if (_onKeyExtra && _onKeyExtra(app, *this, k)) { app.redraw(); return; }
        if (_list.onKey(k, _list.rowsIn(theme::kContentH))) { app.redraw(); return; }
        if ((k == key::Enter || k == key::Select) && !_list.items.empty()) {
            _onSelect(app, _list.selected);
        } else if (k == key::Backspace && !_root) {
            app.pop();
        }
    }

    void draw(App&, lgfx::LovyanGFX& g, const Rect& r) override {
        if (_list.items.empty()) {
            draw::message(g, r.x, r.y, r.w, r.h, _empty.empty() ? i18n::tr(i18n::Str::ListEmpty) : _empty.c_str(),
                          theme::kDim);
            return;
        }
        _list.draw(g, r);
    }

private:
    std::string _title, _hints, _empty;   // empty _empty: the translated "(empty)"
    SelectFn _onSelect;
    KeyFn _onKeyExtra;
    ListView _list;
    RebuildFn _rebuild;
    bool _root = false;
};
