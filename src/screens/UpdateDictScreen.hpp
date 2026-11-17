#pragma once
// ============================================================================
//  UpdateDictScreen – download JMdict and/or build the dictionary on the device
// ============================================================================
//
//  Menu:   Download JMdict_e.gz (Wi-Fi)   -> download, then build
//          Build from <file>              -> one entry per file in /tdeckjisho/jmdict/
//          Download address…              -> change the URL (e.g. a PC on your LAN)
//
//  While working, the screen shows a progress bar and runs ONE SLICE of work
//  per frame in update(): hal::Download::poll() or DictBuilder::step(). The
//  app stays responsive, and the build continues with the screen asleep.
//
//  Keys: Enter select · BS back (while working: BS twice = cancel)
// ============================================================================

#include "core/I18n.hpp"
#include <string>
#include <vector>

#include "dict/DictBuilder.hpp"
#include "hal/Hal.hpp"
#include "ui/App.hpp"
#include "ui/ListView.hpp"

class UpdateDictScreen : public Screen {
public:
    std::string title() const override { return i18n::tr(i18n::Str::UpdTitle); }
    std::string hints() const override;
    void onEnter(App& app) override;
    void onExit(App& app) override;
    void onKey(App& app, int key) override;
    void update(App& app) override;
    void draw(App& app, lgfx::LovyanGFX& g, const Rect& r) override;

private:
    enum class State { Menu, Downloading, Building, Finished };

    void rebuildMenu(App& app);
    void startDownload(App& app);
    void startBuild(App& app, const std::string& source);
    void finish(App& app, bool ok, const std::string& message);
    std::string url(App& app) const;

    State _state = State::Menu;
    ListView _list;
    std::vector<hal::DirEntry> _sources;
    hal::Download _download;
    build::DictBuilder _builder;
    std::string _source;            // file being built from
    std::string _result;            // text shown when Finished
    bool _ok = false;
    bool _confirmCancel = false;
    uint32_t _lastDraw = 0;
};
