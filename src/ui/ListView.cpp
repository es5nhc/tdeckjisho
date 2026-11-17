#include "ui/ListView.hpp"

#include <algorithm>

#include "hal/Keys.hpp"
#include "ui/Draw.hpp"
#include "ui/Theme.hpp"

void ListView::clampSelection() {
    const int n = static_cast<int>(items.size());
    selected = std::max(0, std::min(selected, n - 1));
}

bool ListView::onKey(int key, int visibleRows) {
    if (items.empty()) return false;
    const int n = static_cast<int>(items.size());
    switch (key) {
        case key::Up:    selected = (selected + n - 1) % n; return true;   // wraps around
        case key::Down:  selected = (selected + 1) % n; return true;
        case key::Left:  selected = std::max(0, selected - visibleRows); return true;
        case key::Right: selected = std::min(n - 1, selected + visibleRows); return true;
        default:         return false;
    }
}

void ListView::draw(lgfx::LovyanGFX& g, const Rect& r) {
    const int rh = rowHeight();
    const int rows = std::max(1, r.h / rh);
    const int n = static_cast<int>(items.size());
    clampSelection();

    // Scroll so the selected row is visible – but only as much as needed.
    if (selected < _top) _top = selected;
    if (selected >= _top + rows) _top = selected - rows + 1;
    _top = std::max(0, std::min(_top, std::max(0, n - rows)));

    const bool scrollbar = n > rows;
    const int w = r.w - (scrollbar ? 4 : 0);

    for (int i = 0; i < rows && _top + i < n; ++i) {
        const ListItem& it = items[_top + i];
        const int y = r.y + i * rh;
        const bool sel = (_top + i == selected);
        if (sel) {
            g.fillRect(r.x, y, w, rh, theme::kSelBg);
        }
        const uint16_t col = sel ? theme::kWhiteColour : (it.accent ? theme::kAccent : theme::kText);
        const uint16_t col2 = sel ? theme::kWhiteColour : theme::kDim;
        if (twoLine) {
            draw::text(g, theme::fontUi(), draw::fit(theme::fontUi(), it.primary, w - 14), r.x + 8, y + 2, col);
            draw::text(g, theme::fontSmall(), draw::fit(theme::fontSmall(), it.secondary, w - 14),
                       r.x + 8, y + 20, col2);
        } else {
            const int secW = it.secondary.empty() ? 0 : draw::textWidth(theme::fontSmall(), it.secondary) + 8;
            draw::text(g, theme::fontUi(), draw::fit(theme::fontUi(), it.primary, w - 14 - secW), r.x + 8, y + 3, col);
            if (secW) draw::textRight(g, theme::fontSmall(), it.secondary, r.x + w - 6, y + 5, theme::kDim);
        }
        //g.drawFastHLine(r.x + 8, y + rh - 1, w - 16, theme::kBgBar);   // row separator
    }

    if (scrollbar) {
        // Thumb size and position proportional to the visible part.
        const int th = std::max(8, r.h * rows / n);
        const int ty = r.y + (r.h - th) * _top / std::max(1, n - rows);
        g.fillRect(r.x + r.w - 3, r.y, 3, r.h, theme::kWhiteColour);
        g.fillRect(r.x + r.w - 3, ty, 3, th, theme::kBlackColour);
    }
}
