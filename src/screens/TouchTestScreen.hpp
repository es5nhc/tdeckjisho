#pragma once
// ============================================================================
//  TouchTestScreen – check that a tap lands where the finger was
// ============================================================================
//
//  Five crosses (four corners and the middle). Tap each one: a dot marks
//  where the tap ARRIVED, and the line at the bottom shows the driver's own
//  report (hal::touchStatus): on the T-Deck the raw GT911 numbers and the
//  mapped screen point. If the dot is mirrored or swapped compared to your
//  finger, change kTouchSwapXY / kTouchMirrorX / kTouchMirrorY in
//  hal/tdeck/Input.cpp (docs/04-hardware.md, "Touch").
//
//  In the PC simulator a mouse click is a tap, so the screen works there too.
//  The keyboard keeps working all the time: Backspace goes back.
// ============================================================================

#include <string>
#include <vector>

#include "core/I18n.hpp"
#include "hal/Hal.hpp"
#include "hal/Keys.hpp"
#include "ui/App.hpp"
#include "ui/Draw.hpp"
#include "ui/Theme.hpp"

class TouchTestScreen : public Screen {
public:
    std::string title() const override { return i18n::tr(i18n::Str::TouchTitle); }
    std::string hints() const override { return i18n::tr(i18n::Str::TouchHints); }

    void onKey(App& app, int k) override {
        if (k == key::Backspace) app.pop();
        else if (k == 'c') { _taps.clear(); app.redraw(); }
    }

    void onTap(App& app, int x, int y) override {
        _taps.push_back({x, y});
        if (_taps.size() > 10) _taps.erase(_taps.begin());   // keep the last ten
        app.redraw();
    }

    void draw(App&, lgfx::LovyanGFX& g, const Rect& r) override {
        // Targets in SCREEN coordinates (taps are screen coordinates too, not relative to r).
        const int inset = 12;
        const int tx[] = {inset, 319 - inset, inset, 319 - inset, 160};
        const int ty[] = {r.y + inset, r.y + inset, r.y + r.h - inset, r.y + r.h - inset, r.y + r.h / 2};
        for (int i = 0; i < 5; ++i) {
            g.drawFastHLine(tx[i] - 8, ty[i], 17, theme::kDim);
            g.drawFastVLine(tx[i], ty[i] - 8, 17, theme::kDim);
        }
        for (size_t i = 0; i < _taps.size(); ++i) {
            const bool last = i + 1 == _taps.size();
            g.fillCircle(_taps[i].x, _taps[i].y, last ? 4 : 2, last ? theme::kAccent : theme::kLink);
        }
        std::string line = _taps.empty() ? i18n::tr(i18n::Str::TouchNoTap)
                                         : std::to_string(_taps.back().x) + ", " + std::to_string(_taps.back().y);
        draw::text(g, theme::fontUi(), line, r.x + 40, r.y + r.h / 2 - 34, theme::kText);
        draw::text(g, theme::fontSmall(), hal::touchStatus(), r.x + 40, r.y + r.h / 2 + 16, theme::kDim);
    }

private:
    struct Point { int x, y; };
    std::vector<Point> _taps;
};
