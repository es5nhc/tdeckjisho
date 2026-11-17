#pragma once
// ============================================================================
//  InputScreen – ask the user for one line of text (URL, password...)
// ============================================================================

#include <functional>
#include <string>

#include "core/I18n.hpp"
#include "hal/Keys.hpp"
#include "ui/App.hpp"
#include "ui/Draw.hpp"
#include "ui/TextField.hpp"
#include "ui/Theme.hpp"

class InputScreen : public Screen {
public:
    using DoneFn = std::function<void(App&, const std::string&)>;

    InputScreen(std::string title, std::string prompt, std::string initial, bool mask, DoneFn done)
        : _title(std::move(title)), _prompt(std::move(prompt)), _done(std::move(done)) {
        _field.text = std::move(initial);
        _field.mask = mask;
    }

    std::string title() const override { return _title; }
    std::string hints() const override {
        return i18n::tr(_field.mask ? i18n::Str::InputHintsMasked : i18n::Str::InputHintsPlain);
    }

    void onKey(App& app, int k) override {
        if (k == key::Enter) {
            // Pop FIRST, then call the callback: if the callback pushes a new
            // screen, it lands on top of the screen that was below us.
            // (Both are queued, so the order of these two lines is the order
            // they will be applied in.)
            app.pop();
            _done(app, _field.text);
            return;
        }
        if (k == key::Select) { _field.mask = !_field.mask; app.redraw(); return; }
        if (k == key::Backspace && _field.text.empty()) { app.pop(); return; }
        if (_field.onKey(k)) app.redraw();
    }

    void draw(App&, lgfx::LovyanGFX& g, const Rect& r) override {
        draw::text(g, theme::fontSmall(), _prompt, r.x + 8, r.y + 20, theme::kDim);
        _field.draw(g, r.x + 8, r.y + 40, r.w - 16);
    }

private:
    std::string _title, _prompt;
    DoneFn _done;
    TextField _field;
};
