#pragma once
// ============================================================================
//  ListView.hpp – a scrollable, selectable list (menus, results, files...)
// ============================================================================
//
//  Not a Screen itself – a WIDGET that screens embed and forward keys to.
//  "Composition over inheritance": SearchScreen HAS a ListView, it is not one.
// ============================================================================

#include <string>
#include <vector>

#include "ui/Screen.hpp"

struct ListItem {
    std::string primary;      // main text (theme::fontUi, 16 px)
    std::string secondary;    // second line, or right-aligned note (12 px)
    bool accent = false;      // draw primary in the accent colour
};

class ListView {
public:
    std::vector<ListItem> items;
    int selected = 0;
    bool twoLine = false;     // secondary text below (true) or on the right (false)

    // Handles Up/Down/Left/Right paging. Returns true if the key was used.
    bool onKey(int key, int visibleRows);
    void draw(lgfx::LovyanGFX& g, const Rect& r);
    int rowHeight() const { return twoLine ? 36 : 22; }
    int rowsIn(int h) const { return h / rowHeight(); }
    void clampSelection();

private:
    int _top = 0;             // index of the first visible row
};
