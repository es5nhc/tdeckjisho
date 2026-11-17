#pragma once
// ============================================================================
//  FileBrowserScreen – browse a folder on the SD card
// ============================================================================
//
//  Opening a sub-folder pushes ANOTHER FileBrowserScreen, so the screen stack
//  doubles as the "path history": Backspace goes up one level for free.
// ============================================================================

#include "core/I18n.hpp"
#include <string>
#include <vector>

#include "hal/Hal.hpp"
#include "ui/App.hpp"
#include "ui/ListView.hpp"

class FileBrowserScreen : public Screen {
public:
    explicit FileBrowserScreen(std::string dir) : _dir(std::move(dir)) {}

    std::string title() const override { return "本棚 " + _dir; }
    std::string hints() const override { return i18n::tr(i18n::Str::FolderHints); }
    void onEnter(App& app) override;
    void onKey(App& app, int key) override;
    void draw(App& app, lgfx::LovyanGFX& g, const Rect& r) override;

private:
    std::string _dir;
    std::vector<hal::DirEntry> _entries;
    ListView _list;
    bool _ok = true;
};
