#include "screens/FileBrowserScreen.hpp"
#include "core/I18n.hpp"
#include "core/Paths.hpp"

#include <algorithm>

#include "core/Str.hpp"
#include "hal/Keys.hpp"
#include "screens/Actions.hpp"
#include "ui/Draw.hpp"
#include "ui/Theme.hpp"

void FileBrowserScreen::onEnter(App& app) {
    // Re-read the folder every time we come back: files may have changed.
    const int keep = _list.selected;
    _ok = hal::listDir(_dir, _entries);

    // Hide dot-files (".Trashes", macOS "._x" files) and our temp files.
    _entries.erase(std::remove_if(_entries.begin(), _entries.end(),
                                  [](const hal::DirEntry& e) {
                                      return e.name.empty() || e.name[0] == '.' ||
                                             str::endsWith(e.name, ".tmp");
                                  }),
                   _entries.end());
    // Folders first, then by name. Compare as lower-case for a natural order.
    std::sort(_entries.begin(), _entries.end(), [](const hal::DirEntry& a, const hal::DirEntry& b) {
        if (a.isDir != b.isDir) return a.isDir;
        return str::toLower(a.name) < str::toLower(b.name);
    });

    _list.items.clear();
    for (const auto& e : _entries) {
        ListItem it;
        it.primary = e.isDir ? e.name + "/" : e.name;
        it.secondary = e.isDir ? "" : str::humanSize(e.size);
        it.accent = e.isDir;
        _list.items.push_back(std::move(it));
    }
    _list.selected = keep;
    _list.clampSelection();
    app.redraw();
}

void FileBrowserScreen::onKey(App& app, int k) {
    if (_list.onKey(k, _list.rowsIn(theme::kContentH))) { app.redraw(); return; }
    if ((k == key::Enter || k == key::Select) && !_entries.empty()) {
        const hal::DirEntry& e = _entries[_list.selected];
        const std::string path = (_dir == "/" ? "" : _dir) + "/" + e.name;
        if (e.isDir) app.push(std::make_unique<FileBrowserScreen>(path));
        else actions::openFile(app, path);
    } else if (k == key::Backspace) {
        app.pop();
    }
}

void FileBrowserScreen::draw(App&, lgfx::LovyanGFX& g, const Rect& r) {
    if (!_ok) {
        draw::message(g, r.x, r.y, r.w, r.h,
                      i18n::fmt(i18n::Str::FolderCannotOpen, {{"dir", _dir}, {"books", paths::kBooks}}),
                      theme::kAccent);
    } else if (_entries.empty()) {
        draw::message(g, r.x, r.y, r.w, r.h, i18n::tr(i18n::Str::FolderEmpty), theme::kDim);
    } else {
        _list.draw(g, r);
    }
}
