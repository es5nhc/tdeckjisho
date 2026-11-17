#pragma once
// ============================================================================
//  WebLoadScreen – fetch a page or feed with a byte counter and Backspace = cancel
// ============================================================================
//
//  The network sibling of LoadingScreen, built on hal::Fetch (start-then-poll):
//     1. draw()   paints "Connecting to <host>…"
//     2. update() calls Fetch::start(). On the device that BLOCKS for DNS, TCP,
//                 TLS and the response headers (see Hal.hpp), so step 1 must
//                 have reached the panel first: that is what `_drawn` is for,
//                 as in LoadingScreen.
//     3. update() calls Fetch::poll() every frame while the body arrives; the
//                 screen redraws a few times a second with "34 KB / 120 KB"
//                 and a bar (the bar only if the server sent Content-Length).
//                 Backspace cancels and goes back.
//     4. when done, `build` turns the bytes into a screen (parsing the HTML is
//        blocking again, but short) and we REPLACE ourselves with it.
// ============================================================================

#include <algorithm>   // std::min
#include <functional>
#include <memory>
#include <string>

#include "core/I18n.hpp"
#include "core/Str.hpp"
#include "core/Url.hpp"
#include "hal/Hal.hpp"
#include "hal/Keys.hpp"
#include "ui/App.hpp"
#include "ui/Draw.hpp"
#include "ui/Theme.hpp"

class WebLoadScreen : public Screen {
public:
    // Turns a 200 response into the screen to show, or nullptr and an error message.
    using Build = std::function<std::unique_ptr<Screen>(App&, hal::HttpResult&, std::string& error)>;

    WebLoadScreen(std::string title, std::string url, size_t maxBytes, Build build)
        : _title(std::move(title)), _url(std::move(url)), _limit(maxBytes), _build(std::move(build)) {}

    std::string title() const override { return _title; }
    std::string hints() const override {
        return i18n::tr(_error.empty() ? i18n::Str::HintCancel : i18n::Str::HintBackOnly);
    }

    void onKey(App& app, int k) override {
        if (k != key::Backspace) return;
        _fetch.cancel();                 // no-op if not running
        app.pop();
    }

    void update(App& app) override {
        if (!_drawn || !_error.empty()) return;
        if (_fetch.state() == hal::Fetch::State::Idle) {
            _fetch.start(_url, _limit);    // blocks for the connection (device)
            app.redraw();
        }
        const size_t before = _fetch.done();
        if (_fetch.poll() == hal::Fetch::State::Done) { finish(app); return; }
        // 4 frames per second is plenty for a counter; every frame would cost
        // more time drawing than receiving.
        if (_fetch.done() != before && app.now() - _lastDraw > 250) app.redraw();
    }

    void draw(App& app, lgfx::LovyanGFX& g, const Rect& r) override {
        _drawn = true;
        _lastDraw = app.now();
        if (!_error.empty()) {
            draw::message(g, r.x, r.y, r.w, r.h, std::string(i18n::tr(i18n::Str::ErrorLabel)) + "\n" + _error,
                          theme::kAccent);
            return;
        }
        const std::string host = url::host(_url);
        if (_fetch.state() != hal::Fetch::State::Running) {     // not started yet: connecting
            draw::message(g, r.x, r.y, r.w, r.h, i18n::fmt(i18n::Str::WebConnecting, {{"host", host}}),
                          theme::kText);
            return;
        }
        const lgfx::IFont* ui = theme::fontUi();
        const lgfx::IFont* small = theme::fontSmall();
        const int x = r.x + 10, w = r.w - 20;
        int y = r.y + 40;
        draw::text(g, ui, draw::fit(ui, i18n::fmt(i18n::Str::WebReceiving, {{"host", host}}), w), x, y,
                   theme::kText);
        y += 24;
        draw::text(g, small, draw::fit(small, _url, w), x, y, theme::kDim);
        y += 24;
        const size_t done = _fetch.done(), total = _fetch.total();
        std::string count = str::humanSize(static_cast<uint32_t>(done));
        if (total) {
            const float frac = std::min(1.f, static_cast<float>(done) / total);
            g.drawRect(x, y, w, 14, theme::kRule);
            g.fillRect(x + 2, y + 2, static_cast<int>((w - 4) * frac), 10, theme::kAccent);
            y += 22;
            count += " / " + str::humanSize(static_cast<uint32_t>(total));
        }
        draw::text(g, small, count, x, y, theme::kText);
    }

private:
    void finish(App& app) {
        hal::HttpResult& r = _fetch.result();
        std::unique_ptr<Screen> next;
        if (r.status != 200) _error = r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error;
        else next = _build(app, r, _error);
        if (!next && _error.empty()) _error = "?";   // never Done without a screen or a reason
        if (next) app.replace(std::move(next));
        else app.redraw();               // show the error
    }

    std::string _title, _url;
    size_t _limit;
    Build _build;
    hal::Fetch _fetch;
    std::string _error;
    bool _drawn = false;
    uint32_t _lastDraw = 0;
};
