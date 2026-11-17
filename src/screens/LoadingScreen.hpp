#pragma once
// ============================================================================
//  LoadingScreen – show "Loading…", then run a slow job, then show its result
// ============================================================================
//
//  Problem: downloading a page or reading a 2 MB book BLOCKS for a second or
//  more. If we start the job inside onKey(), the screen never shows that
//  anything is happening.
//
//  Solution – a tiny state machine:
//     state 0: draw()   paints "Loading…"          -> state 1
//     state 1: update() runs the job (blocking)    -> replace this screen with
//                                                     the job's result screen
//  App::tick() calls update() BEFORE draw(), so we must wait until one frame
//  has actually been drawn; that is what `_drawn` tracks.
//
//  A real multitasking solution would run the job in a FreeRTOS task on the
//  ESP32's second core (see docs/01-architecture.md, "Long jobs"). The state machine is the
//  simplest thing that gives correct feedback.
// ============================================================================

#include <functional>
#include <memory>
#include <string>

#include "core/I18n.hpp"
#include "hal/Hal.hpp"
#include "hal/Keys.hpp"
#include "ui/App.hpp"
#include "ui/Draw.hpp"
#include "ui/Theme.hpp"

class LoadingScreen : public Screen {
public:
    // The job returns the screen to show, or nullptr and an error message.
    using Job = std::function<std::unique_ptr<Screen>(App&, std::string& error)>;

    LoadingScreen(std::string title, std::string message, Job job)
        : _title(std::move(title)), _message(std::move(message)), _job(std::move(job)) {}

    std::string title() const override { return _title; }
    std::string hints() const override {
        return i18n::tr(_error.empty() ? i18n::Str::PleaseWait : i18n::Str::HintBackOnly);
    }

    void onKey(App& app, int k) override {
        if (k == key::Backspace && !_error.empty()) app.pop();
    }

    void update(App& app) override {
        if (!_drawn || _done) return;
        _done = true;
        const uint32_t t0 = hal::millis();
        std::unique_ptr<Screen> next = _job(app, _error);
        hal::log("[load] '%s' took %u ms\n", _title.c_str(),
                 static_cast<unsigned>(hal::millis() - t0));   // handy for profiling
        if (next) app.replace(std::move(next));
        else app.redraw();          // show the error
    }

    void draw(App&, lgfx::LovyanGFX& g, const Rect& r) override {
        _drawn = true;
        if (_error.empty()) draw::message(g, r.x, r.y, r.w, r.h, _message, theme::kText);
        else draw::message(g, r.x, r.y, r.w, r.h, std::string(i18n::tr(i18n::Str::ErrorLabel)) + "\n" + _error,
                                   theme::kAccent);
    }

private:
    std::string _title, _message, _error;
    Job _job;
    bool _drawn = false, _done = false;
};
