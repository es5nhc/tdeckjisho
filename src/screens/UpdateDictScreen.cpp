#include "screens/UpdateDictScreen.hpp"

#include <algorithm>  // std::all_of
#include <cstdio>

#include "core/I18n.hpp"
#include "core/Paths.hpp"
#include "core/Str.hpp"
#include "core/Url.hpp"
#include "hal/Keys.hpp"
#include "screens/InputScreen.hpp"
#include "ui/Draw.hpp"
#include "ui/Theme.hpp"

using i18n::fmt;
using i18n::Str;
using i18n::tr;
using i18n::trn;

namespace {
constexpr int kHeaderH = 34;                 // explanation above the menu
constexpr uint32_t kRedrawMs = 250;          // progress: 4 frames per second is plenty

std::string mmss(uint32_t ms) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%u:%02u", static_cast<unsigned>(ms / 60000),
                  static_cast<unsigned>(ms / 1000 % 60));
    return buf;
}

// "http://host/path/JMdict_e.gz?x=1" -> "JMdict_e.gz"
// SECURITY: this name comes from an address someone typed (or a server chose) and
// becomes part of an SD card path. Allow only plain names – letters, digits, '.',
// '_', '-', not starting with '.', at most 64 characters – so ".." or "a/b" or
// "..\x" can never point anywhere else. Anything else gets the safe default.
std::string fileNameOf(const std::string& u) {
    std::string p = u.substr(0, u.find_first_of("?#"));
    p = p.substr(p.rfind('/') + 1);
    const bool plain = !p.empty() && p.size() <= 64 && p[0] != '.' &&
        std::all_of(p.begin(), p.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '.' || c == '_' || c == '-';
        });
    return plain ? p : "JMdict_e.gz";
}
}  // namespace

std::string UpdateDictScreen::url(App& app) const {
    return app.settings().get("jmdict_url", build::kDefaultJmdictUrl);
}

std::string UpdateDictScreen::hints() const {
    switch (_state) {
        case State::Menu:     return tr(Str::UpdHintsMenu);
        case State::Finished: return tr(Str::HintBackOnly);
        default:              return tr(_confirmCancel ? Str::UpdHintsConfirm : Str::UpdHintsWorking);
    }
}

void UpdateDictScreen::onEnter(App& app) {
    if (_state == State::Menu) rebuildMenu(app);
}

void UpdateDictScreen::onExit(App&) {
    // Leaving mid-way (should not happen: BS is caught) still cleans up.
    _download.cancel();
    _builder.cancel();
}

void UpdateDictScreen::rebuildMenu(App& app) {
    hal::makeDirs(paths::kJmdict);
    _sources = build::DictBuilder::findSources();
    _list.items.clear();
    _list.items.push_back({fmt(Str::UpdMenuDownload, {{"file", fileNameOf(url(app))}}), url::host(url(app)), true});
    for (const auto& s : _sources)
        _list.items.push_back({fmt(Str::UpdMenuBuild, {{"name", s.name}}), str::humanSize(s.size)});
    _list.items.push_back({tr(Str::UpdMenuAddress), url(app)});
    _list.twoLine = true;
    _list.clampSelection();
}

// ---------------------------------------------------------------------------
//  Starting the two kinds of work
// ---------------------------------------------------------------------------
void UpdateDictScreen::startDownload(App& app) {
    if (hal::wifiState() != hal::WifiState::Connected) { app.toast(tr(Str::NotConnectedOpenWifi)); return; }
    _source = paths::kJmdict + "/" + fileNameOf(url(app));
    // Shows "Connecting…" first: start() blocks for DNS + TCP (< 2 s).
    _state = State::Downloading;
    _confirmCancel = false;
    if (!_download.start(url(app), _source)) finish(app, false, fmt(Str::UpdDownloadFailed, {{"err", _download.error()}}));
    app.redraw();
}

void UpdateDictScreen::startBuild(App& app, const std::string& source) {
    _source = source;
    // Release the old dictionary's files and RAM. It stays on the card, and
    // comes back if the build fails.
    app.dict().close();
    build::DictBuilder::Options opt;
    opt.source = source;
    _state = State::Building;
    _confirmCancel = false;
    if (!_builder.start(opt)) finish(app, false, fmt(Str::UpdCannotStart, {{"err", _builder.error()}}));
    app.redraw();
}

void UpdateDictScreen::finish(App& app, bool ok, const std::string& message) {
    _state = State::Finished;
    _ok = ok;
    _result = message;
    if (!app.dict().isOpen()) app.dict().open(paths::kDict);   // new one, or the old one again
    app.redraw();
}

// ---------------------------------------------------------------------------
//  Keys and background work
// ---------------------------------------------------------------------------
void UpdateDictScreen::onKey(App& app, int k) {
    if (_state == State::Menu) {
        if (_list.onKey(k, _list.rowsIn(theme::kContentH - kHeaderH))) { app.redraw(); return; }
        if (k == key::Backspace) { app.pop(); return; }
        if (k != key::Enter && k != key::Select) return;
        const int i = _list.selected;
        if (i == 0) {
            startDownload(app);
        } else if (i <= static_cast<int>(_sources.size())) {
            startBuild(app, paths::kJmdict + "/" + _sources[i - 1].name);
        } else {
            app.push(std::make_unique<InputScreen>(
                tr(Str::UpdInputTitle), tr(Str::UpdInputPrompt), url(app), false,
                [](App& a, const std::string& typed) {
                    a.settings().set("jmdict_url", str::trim(typed).empty() ? build::kDefaultJmdictUrl
                                                                           : url::normalizeTyped(typed));
                    a.settings().save();
                }));
        }
        return;
    }
    if (_state == State::Finished) {
        if (k == key::Backspace) app.pop();
        return;
    }
    // Downloading / Building: BS twice cancels, anything else un-arms.
    if (k == key::Backspace && _confirmCancel) {
        _download.cancel();
        _builder.cancel();
        finish(app, false, tr(Str::UpdCancelled));
        return;
    }
    _confirmCancel = (k == key::Backspace);
    app.redraw();
}

void UpdateDictScreen::update(App& app) {
    if (_state == State::Downloading) {
        const auto st = _download.poll();
        if (st == hal::Download::State::Done) { startBuild(app, _source); return; }
        if (st == hal::Download::State::Failed) {
            if (_state == State::Downloading)
                finish(app, false, fmt(Str::UpdDownloadFailed, {{"err", _download.error()}}));
            return;
        }
    } else if (_state == State::Building) {
        _builder.step(40);                 // ~40 ms of work per frame
        if (_builder.phase() == build::DictBuilder::Phase::Done) {
            // The one-line summary; the detailed timings (report()) go to the log only.
            finish(app, true, fmt(Str::UpdDone, {{"summary", fmt(Str::UpdSummary,
                       {{"entries", trn(Str::EntriesOne, Str::EntriesMany, _builder.entries())},
                        {"time", mmss(_builder.elapsedMs())}})}}));
            return;
        }
        if (_builder.phase() == build::DictBuilder::Phase::Failed) {
            if (_state == State::Building) finish(app, false, fmt(Str::UpdBuildFailed, {{"err", _builder.error()}}));
            return;
        }
    } else {
        return;
    }
    if (app.now() - _lastDraw > kRedrawMs) app.redraw();
}

// ---------------------------------------------------------------------------
//  Drawing
// ---------------------------------------------------------------------------
void UpdateDictScreen::draw(App& app, lgfx::LovyanGFX& g, const Rect& r) {
    _lastDraw = app.now();
    const lgfx::IFont* ui = theme::fontUi();
    const lgfx::IFont* small = theme::fontSmall();
    const int x = r.x + 10, w = r.w - 20;

    if (_state == State::Menu) {
        // draw::fit() cuts a text that is too wide and adds "…": a translation that turns out
        // longer than the screen is clipped, never drawn off the edge.
        draw::text(g, small, draw::fit(small, tr(Str::UpdIntro1), w), x, r.y + 3, theme::kDim);
        draw::text(g, small, draw::fit(small, fmt(Str::UpdIntro2, {{"dir", paths::kJmdict}}), w), x, r.y + 18,
                   theme::kDim);
        _list.draw(g, Rect{r.x, r.y + kHeaderH, r.w, r.h - kHeaderH});
        return;
    }
    if (_state == State::Finished) {
        draw::message(g, r.x, r.y, r.w, r.h, _result, _ok ? theme::kOk : theme::kAccent);
        return;
    }

    // ---- progress view (Downloading / Building) ------------------------------
    const std::string name = _source.substr(_source.rfind('/') + 1);
    float frac = 0;
    std::string line1, line2;
    if (_state == State::Downloading) {
        frac = _download.total() ? static_cast<float>(_download.done()) / _download.total() : 0.f;
        line1 = fmt(Str::UpdDownloading, {{"name", name}});
        line2 = str::humanSize(_download.done()) +
                (_download.total() ? " / " + str::humanSize(_download.total()) : std::string());
    } else {
        frac = _builder.progress();
        line1 = fmt(Str::UpdBuilding, {{"name", name}});
        const uint32_t el = _builder.elapsedMs();
        line2 = fmt(Str::UpdElapsed, {{"t", mmss(el)}});
        // Not "~1:03": the Japanese font follows JIS X 0201, where byte 0x7E is
        // an OVERLINE (‾) and 0x5C is ¥. Words are safer than ASCII art here.
        if (frac > 0.03f)
            line2 += "   " + fmt(Str::UpdTimeLeft, {{"t", mmss(static_cast<uint32_t>(el / frac - el))}});
    }
    int y = r.y + 14;
    draw::text(g, ui, draw::fit(ui, line1, w), x, y, theme::kText);
    y += 24;
    draw::text(g, small, _state == State::Downloading ? tr(Str::UpdWifiToSd) : _builder.phaseName(), x, y,
               theme::kDim);
    y += 20;
    g.drawRect(x, y, w, 14, theme::kRule);
    g.fillRect(x + 2, y + 2, static_cast<int>((w - 4) * frac), 10, theme::kAccent);
    y += 22;
    draw::text(g, small, std::to_string(static_cast<int>(frac * 100)) + " %   " + line2, x, y, theme::kText);
    y += 18;
    if (_state == State::Building)
        draw::text(g, small, trn(Str::EntriesOne, Str::EntriesMany, _builder.entries()), x, y, theme::kDim);
    y += 26;
    draw::text(g, small, tr(Str::UpdKeepOld), x, y, theme::kDim);
    draw::text(g, small, tr(Str::UpdSleepOk), x, y + 15, theme::kDim);
    if (_confirmCancel)
        draw::message(g, r.x, r.y + r.h - 34, r.w, 30, tr(Str::UpdPressAgain), theme::kAccent);
}
