#include "screens/DocumentScreen.hpp"
#include <algorithm>


#include "core/I18n.hpp"
#include "hal/Hal.hpp"
#include "hal/Keys.hpp"
#include "screens/Actions.hpp"
#include "screens/ListScreen.hpp"
#include "ui/Theme.hpp"

using i18n::fmt;
using i18n::Str;
using i18n::tr;

namespace {
constexpr int kTextWidth = theme::kScreenW - 12;   // RichView draws with 6 px margins
}

DocumentScreen::DocumentScreen(rt::Doc doc, std::string positionKey)
    : _posKey(std::move(positionKey)) {
    _view.setDoc(std::move(doc));
}

std::string DocumentScreen::title() const {
    std::string t = _view.doc().title.empty() ? tr(Str::DocumentDefault) : _view.doc().title;
    return t + "  " + std::to_string(_view.percent()) + "%";
}

std::string DocumentScreen::hints() const {
    if (_view.cursorActive()) return tr(_panel ? Str::PanelHints : Str::CursorHints);
    // "r furigana" only where the text has furigana to switch (see RichView::hasRuby).
    const bool links = !_view.doc().links.empty();
    if (!_view.hasRuby()) return tr(links ? Str::ReaderHintsLinksNoRuby : Str::ReaderHintsNoRuby);
    return tr(links ? Str::ReaderHintsLinks : Str::ReaderHints);
}

void DocumentScreen::relayout(App& app) {
    _fontSize = app.settings().getInt("font", 16);
    _ruby = app.settings().getInt("ruby", 1) != 0;
    _view.layout(kTextWidth, _fontSize, _ruby);
}

void DocumentScreen::onEnter(App& app) {
    // Layout lazily on first show (and again if settings changed while a
    // screen on top of us was open – e.g. font size changed in another doc).
    const bool first = _fontSize == 0;
    if (first || _fontSize != app.settings().getInt("font", 16) ||
        _ruby != (app.settings().getInt("ruby", 1) != 0)) {
        relayout(app);
    }
    if (first && !_posKey.empty()) {
        _view.setPosition(static_cast<uint32_t>(app.positions().getInt(_posKey, 0)));
    }
}

void DocumentScreen::onExit(App& app) {
    if (_posKey.empty()) return;
    app.positions().setInt(_posKey, static_cast<int>(_view.position()));
    app.positions().save();
}

void DocumentScreen::onKey(App& app, int k) {
    if (_view.cursorActive()) { onCursorKey(app, k); return; }
    const int pageH = theme::kContentH - 4;
    switch (k) {
        case key::Up:    _view.scrollLines(-1); break;
        case key::Down:  _view.scrollLines(+1); break;
        case key::Left:
        case 'b':        _view.pageUp(pageH); break;
        case key::Right:
        case ' ':        _view.pageDown(pageH); break;
        case 'g':        _view.toTop(); break;
        case 'G':        _view.toEnd(pageH); break;
        case 'f': {
            // Cycle 12 -> 16 -> 20 -> 12. Stored in settings so every
            // document (and the next boot) uses the same size.
            const int next = _fontSize == 12 ? 16 : _fontSize == 16 ? 20 : 12;
            app.settings().setInt("font", next);
            app.settings().save();
            relayout(app);
            app.toast(fmt(Str::FontSizeToast, {{"n", std::to_string(next)}}));
            break;
        }
        case 'r':
            if (!_view.hasRuby()) return;   // no furigana in this text: nothing to switch
            app.settings().setInt("ruby", _ruby ? 0 : 1);
            app.settings().save();
            relayout(app);
            app.toast(tr(_ruby ? Str::FuriganaOn : Str::FuriganaOff));
            break;
        case 'c':                         // cursor mode: select a word, look it up
            _view.cursorStart();
            break;
        case 'd':
            actions::openDictionary(app);
            break;
        case 'l':
        case key::Select:
        case key::Enter: {
            const auto& links = _view.doc().links;
            if (links.empty()) { app.toast(tr(Str::NoLinks)); break; }
            std::vector<ListItem> items;
            for (const auto& l : links)
                items.push_back({l.label.empty() ? l.url : l.label, l.url});
            // The lambda captures a COPY of the links ([links]). Capturing by
            // reference ([&links]) would point into this DocumentScreen; that
            // happens to be safe here (we stay below the list on the stack),
            // but a self-contained copy is the safe default for any callback
            // that is stored and called later.
            auto screen = std::make_unique<ListScreen>(
                fmt(Str::LinksTitle, {{"n", std::to_string(links.size())}}), std::move(items),
                [links](App& a, int i) { actions::openUrl(a, links[i].url); }, true);
            app.push(std::move(screen));
            break;
        }
        case key::Backspace:
            app.pop();
            break;
        default:
            return;     // unknown key: nothing changed, no redraw needed
    }
    app.redraw();
}

void DocumentScreen::draw(App& app, lgfx::LovyanGFX& g, const Rect& r) {
    _content = r;
    _view.draw(g, r);
    if (_panel && !_hits.empty()) drawPanel(g);
}

// ============================================================================
//  Select a word, look it up
// ============================================================================
namespace {
constexpr int kRowH = 22;                   // one result row in the panel
constexpr int kHeadH = 16;                  // the line above the rows: what was matched
constexpr int kPanelMargin = 4;
constexpr size_t kMaxRows = 4;
constexpr size_t kLookupChars = 16;         // how much text after the cursor a look-up gets
}  // namespace

void DocumentScreen::closePanel() {
    _panel = false;
    _hits.clear();
    _view.cursorMark(0);
}

void DocumentScreen::lookup(App& app) {
    _view.cursorToWordStart();              // inside "practice": look up the whole word
    size_t end = 0;
    const std::string text = _view.cursorText(kLookupChars, &end);
    // The character before matters too: 3匹 (a counter), 雄ジカ (シカ voiced by the compound).
    _hits = app.dict().lookupAt(text, kMaxRows, _view.cursorTextBefore(2));
    _sel = 0;
    if (_hits.empty()) {
        _panel = false;
        _view.cursorMark(0);
        app.toast(tr(app.dict().hasKanjiIndex() ? Str::LookupNothing : Str::LookupNoKanji));
        return;
    }
    // Underline what the best match covers, so you see where the word ends.
    size_t markEnd = 0;
    _view.cursorText(_hits[0].chars, &markEnd);
    _view.cursorMark(markEnd);
    _panel = true;
}

void DocumentScreen::onCursorKey(App& app, int k) {
    if (_panel) {                           // the panel is open: the arrows choose a row
        switch (k) {
            case key::Up:    if (_sel > 0) --_sel; break;
            case key::Down:  if (_sel + 1 < static_cast<int>(_hits.size())) ++_sel; break;
            case key::Enter:
            case key::Select: actions::openEntry(app, _hits[_sel].hit.offset); return;
            case key::Left:
            case key::Right: closePanel(); _view.cursorMove(k == key::Right ? +1 : -1, 0); break;
            case key::Backspace:
            case 'c':        closePanel(); break;
            default: return;
        }
        app.redraw();
        return;
    }
    switch (k) {
        case key::Left:  _view.cursorMove(-1, 0); break;
        case key::Right: _view.cursorMove(+1, 0); break;
        case key::Up:    _view.cursorMove(0, -1); break;
        case key::Down:  _view.cursorMove(0, +1); break;
        case key::Enter:
        case key::Select: lookup(app); break;
        case key::Backspace:                // first Backspace leaves cursor mode, the next leaves the page
        case 'c':        _view.cursorStop(); break;
        default: return;
    }
    app.redraw();
}

void DocumentScreen::onTap(App& app, int x, int y) {
    if (_panel) {
        const Rect p = panelRect();
        const bool inside = x >= p.x && x < p.x + p.w && y >= p.y && y < p.y + p.h;
        if (inside) {
            const int row = (y - p.y - kHeadH) / kRowH;
            if (y >= p.y + kHeadH && row >= 0 && row < static_cast<int>(_hits.size())) {
                if (row == _sel) actions::openEntry(app, _hits[row].hit.offset);   // a second tap opens it
                else _sel = row;
                app.redraw();
            }
            return;
        }
        closePanel();                       // a tap elsewhere closes the panel and picks a new word
    }
    if (_view.cursorFromPoint(x, y)) lookup(app);
    app.redraw();
}

Rect DocumentScreen::panelRect() const {
    const int rows = static_cast<int>(std::min(_hits.size(), kMaxRows));
    const int h = kHeadH + rows * kRowH + 4;
    // Keep the panel away from the word: cursor in the upper half -> panel at the bottom.
    const int cy = _view.cursorScreenY();
    const bool atBottom = cy < 0 || cy < _content.y + _content.h / 2;
    const int y = atBottom ? _content.y + _content.h - h - 2 : _content.y + 2;
    return Rect{_content.x + kPanelMargin, y, _content.w - 2 * kPanelMargin, h};
}

void DocumentScreen::drawPanel(lgfx::LovyanGFX& g) {
    const lgfx::IFont* ui = theme::fontUi();
    const lgfx::IFont* small = theme::fontSmall();
    const Rect p = panelRect();
    g.fillRoundRect(p.x, p.y, p.w, p.h, 5, theme::kWhiteColour);
    g.drawRoundRect(p.x, p.y, p.w, p.h, 5, theme::kBlueColour);

    // What was matched, and what it was looked up as: 食べました → 食べる
    const LookupHit& cur = _hits[_sel];
    std::string head = cur.matched;
    if (cur.form != cur.matched) head += " → " + cur.form;
    draw::text(g, small, draw::fit(small, head, p.w - 12), p.x + 6, p.y + 2, theme::kDim);

    for (size_t i = 0; i < _hits.size() && i < kMaxRows; ++i) {
        const DictHit& h = _hits[i].hit;
        const int ry = p.y + kHeadH + static_cast<int>(i) * kRowH;
        bool isSelected = (static_cast<int>(i) == _sel);
        if (isSelected) g.fillRect(p.x + 2, ry, p.w - 4, kRowH, theme::kSelBg);
        int x = p.x + 6;
        x += draw::text(g, ui, draw::fit(ui, h.headword, 110), x, ry + 2, isSelected ? theme::kWhiteColour : theme::kText);
        if (!h.reading.empty()) x += 4 + draw::text(g, small, "【" + h.reading + "】", x + 4, ry + 5, isSelected ? theme::kWhiteColour : theme::kText);
        const int room = p.x + p.w - 6 - (x + 6);
        if (room > 24) draw::text(g, small, draw::fit(small, h.gloss, room), x + 6, ry + 5, isSelected ? theme::kWhiteColour : theme::kText);
    }
}
