#pragma once
// ============================================================================
//  DocumentScreen – the reader. Shows ANY rt::Doc: books, web pages,
//  RSS articles, dictionary entries, the About page.
// ============================================================================
//
//  Keys
//    ↑ ↓          one line            ← →  / b  space   one page
//    g / G        start / end         f                 font size 12/16/20
//    r            furigana on/off     l or click        list of links
//                 (r only in texts that carry furigana: we never derive readings)
//    c            cursor mode: ←→↑↓ move over the text, Enter looks the word up in the
//                 dictionary (or tap a word). In the panel: ↑↓ choose, Enter opens the entry.
//    d            dictionary
//    BS           back
// ============================================================================

#include <string>
#include <vector>

#include "text/RichText.hpp"
#include "ui/App.hpp"
#include "ui/RichView.hpp"

class DocumentScreen : public Screen {
public:
    // positionKey: if non-empty (e.g. the file path), the reading position is
    // saved under this key when leaving and restored when opening again.
    explicit DocumentScreen(rt::Doc doc, std::string positionKey = "");

    std::string title() const override;
    std::string hints() const override;
    void onEnter(App& app) override;
    void onExit(App& app) override;
    void onKey(App& app, int key) override;
    void onTap(App& app, int x, int y) override;
    void draw(App& app, lgfx::LovyanGFX& g, const Rect& r) override;

private:
    void relayout(App& app);

    // ---- look up the word under a cursor ----
    void onCursorKey(App& app, int key);
    void lookup(App& app);                  // search the text at the cursor, open the panel
    void closePanel();
    Rect panelRect() const;                 // where the panel is drawn (top or bottom, away from the cursor)
    void drawPanel(lgfx::LovyanGFX& g);
    std::vector<LookupHit> _hits;
    int _sel = 0;                           // the highlighted row of the panel
    bool _panel = false;
    Rect _content{0, 16, 320, 211};         // the reader's area at the last draw()

    RichView _view;
    std::string _posKey;
    int _fontSize = 0;      // 0 = not laid out yet
    bool _ruby = true;

};
