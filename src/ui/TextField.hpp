#pragma once
// ============================================================================
//  TextField.hpp – a one-line text input (search box, URL, password)
// ============================================================================
//
//  Header-only: small enough that a .cpp file would just add noise.
//  The cursor is always at the end – editing in the middle needs Left/Right,
//  which we already use for other things (trackball). Good enough for a
//  search box; an exercise if you want more.
// ============================================================================

#include <string>

#include "hal/Keys.hpp"
#include "ui/Draw.hpp"
#include "ui/Theme.hpp"

struct TextField {
    std::string text;
    bool mask = false;          // show •••• instead of the text (passwords)
    size_t maxLen = 200;

    // Returns true if the key changed the text.
    bool onKey(int k) {
        if (key::isPrintable(k) && text.size() < maxLen) { text += static_cast<char>(k); return true; }
        if (k == key::Backspace && !text.empty()) { text.pop_back(); return true; }
        return false;
    }

    void draw(lgfx::LovyanGFX& g, int x, int y, int w, bool focused = true) const {
        const lgfx::IFont* f = theme::fontUi();
        g.fillRoundRect(x, y, w, 22, 4, theme::kBg);
        g.drawRoundRect(x, y, w, 22, 4, focused ? theme::kAccent : theme::kBlackColour);
        std::string shown = mask ? std::string(text.size(), '*') : text;
        // If the text is wider than the box, show its END (where we type).
        while (!shown.empty() && draw::textWidth(f, shown) > w - 14) shown.erase(0, 1);
        const int tw = draw::text(g, f, shown, x + 5, y + 3, theme::kText);
        if (focused) g.fillRect(x + 6 + tw, y + 4, 2, 15, theme::kAccent);   // the cursor
    }
};
