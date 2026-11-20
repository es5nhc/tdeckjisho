#include "screens/Actions.hpp"

#include <algorithm>  // std::min
#include <cstdio>     // snprintf
#include <memory>

#include "core/I18n.hpp"
#include "core/Paths.hpp"
#include "core/Str.hpp"
#include "core/TimeZone.hpp"
#include "core/Url.hpp"
#include "core/Utf8.hpp"
#include "hal/Hal.hpp"
#include "screens/DocumentScreen.hpp"
#include "screens/FileBrowserScreen.hpp"
#include "screens/InputScreen.hpp"
#include "screens/ListScreen.hpp"
#include "screens/LoadingScreen.hpp"
#include "screens/QuizScreen.hpp"
#include "screens/SearchScreen.hpp"
#include "screens/TouchTestScreen.hpp"
#include "screens/UpdateDictScreen.hpp"
#include "screens/WebLoadScreen.hpp"
#include "screens/WifiScreen.hpp"
#include "text/Feed.hpp"
#include "text/Images.hpp"
#include "text/Parsers.hpp"

// The version comes from `custom_version` in platformio.ini, passed to the compiler
// as -DJISHO_VERSION="1.0" by tools/name_firmware.py. A build without it (a quick
// g++ command, an editor's code analysis) still compiles.
#ifndef JISHO_VERSION
#define JISHO_VERSION "dev"
#endif

using i18n::fmt;
using i18n::Str;
using i18n::tr;
using i18n::trn;

namespace actions {

namespace {

constexpr size_t kMaxBookBytes = 4 * 1024 * 1024;   // fits easily in 8 MB PSRAM
constexpr size_t kMaxWebBytes = 1536 * 1024;
const std::string& kFeedsPath = paths::kFeeds;

// ISO-8859-1 (Latin-1) -> UTF-8. Latin-1 bytes ARE the first 256 Unicode
// code points, so each byte just gets re-encoded. Used as a fallback for old
// web pages that are not UTF-8. (Shift_JIS/EUC-JP would need big tables.)
// SECURITY: every byte >= 0x80 becomes TWO bytes, so the text can double. The
// output obeys the same limit as the download (`maxBytes`), or a 1.5 MB page
// would become 3 MB plus everything the parser builds from it.
std::string latin1ToUtf8(std::string_view s, size_t maxBytes, bool& cut) {
    std::string out;
    out.reserve(std::min(maxBytes, s.size() + s.size() / 8));
    cut = false;
    for (char c : s) {
        if (out.size() + 2 > maxBytes) { cut = true; break; }
        utf8::append(out, static_cast<uint8_t>(c));
    }
    return out;
}

// Everything after this may rely on the body being valid UTF-8 within the limit.
void ensureUtf8(hal::HttpResult& r) {
    if (utf8::isValid(r.body)) return;
    bool cut = false;
    r.body = latin1ToUtf8(r.body, kMaxWebBytes, cut);
    r.truncated = r.truncated || cut;
}

std::string extension(const std::string& path) {
    const size_t dot = path.rfind('.');
    return dot == std::string::npos ? "" : str::toLower(path.substr(dot + 1));
}

std::string basename(const std::string& path) { return path.substr(path.rfind('/') + 1); }

// Picture files open in the reader as a document with just that picture.
bool isImageFile(const std::string& path) {
    const std::string ext = extension(path);
    return ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "bmp" || ext == "qoi";
}

// A picture's src is a URL, so resolveImagePath() percent-decodes it: a literal
// '%' in a file name must be written as "%25" or "100%.png" would not be found.
std::string fileNameAsSrc(const std::string& name) {
    std::string out;
    for (char c : name) out += (c == '%') ? std::string("%25") : std::string(1, c);
    return out;
}

bool looksLikeFeed(std::string_view body) {
    const std::string head = str::toLower(body.substr(0, 2048));
    return head.find("<rss") != std::string::npos || head.find("<feed") != std::string::npos;
}

bool requireNetwork(App& app) {
    if (hal::wifiState() == hal::WifiState::Connected) return true;
    app.toast(tr(Str::NotConnectedOpenWifi));
    return false;
}

// Build the list of articles for a downloaded feed.
std::unique_ptr<Screen> feedScreen(const std::string& name, std::string_view xml, std::string& err) {
    // shared_ptr: the feed must live as long as the list's callback might
    // be called. The lambda below holds one reference; when the ListScreen
    // (and so the lambda) is destroyed, the Feed is freed automatically.
    // Python does this for every object; in C++ you opt in.
    auto feed = std::make_shared<rt::Feed>();
    if (!rt::parseFeed(xml, *feed)) { err = tr(Str::FeedNoArticles); return nullptr; }

    std::vector<ListItem> items;
    for (const auto& it : feed->items) items.push_back({it.title, it.date});
    const std::string title = feed->title.empty() ? name : feed->title;
    auto screen = std::make_unique<ListScreen>(
        title, std::move(items),
        [feed, title](App& app, int i) {
            rt::Doc doc;
            rt::feedItemToDoc(feed->items[i], title, doc);
            // A feed without a <title>, listed in feeds.txt without a name ("|url"):
            // the article's own fallback, not DocumentScreen's "File".
            if (doc.title.empty()) doc.title = tr(Str::ArticleDefault);
            app.push(std::make_unique<DocumentScreen>(std::move(doc)));
        },
        /*twoLine=*/true);
    return screen;
}

struct FeedDef {
    std::string name, url;
};

std::vector<FeedDef> loadFeeds() {
    std::vector<FeedDef> feeds;
    std::string text;
    if (!hal::readFile(kFeedsPath, text, 32 * 1024)) return feeds;
    for (std::string_view line : str::split(text, '\n')) {
        line = str::trim(line);
        if (line.empty() || line[0] == '#') continue;
        // "Name | https://url"   or just   "https://url"
        const size_t bar = line.find('|');
        FeedDef f;
        f.url = std::string(str::trim(bar == std::string_view::npos ? line : line.substr(bar + 1)));
        f.name = bar == std::string_view::npos ? url::host(f.url) : std::string(str::trim(line.substr(0, bar)));
        feeds.push_back(std::move(f));
    }
    return feeds;
}

}  // namespace

// ============================================================================
//  Home
// ============================================================================
std::unique_ptr<Screen> home(App& app) {
    // Each menu entry: its text, its shortcut key, its action.
    // A plain array of structs keeps text/key/action next to each other.
    struct Entry {
        Str label;
        char shortcut;
        void (*action)(App&);   // a plain function pointer: no captures needed
    };
    static const Entry kEntries[] = {
        {Str::HomeDictionary, 'd', openDictionary},
        {Str::HomeQuiz,       'q', openQuiz},
        {Str::HomeLibrary,    'l', openLibrary},
        {Str::HomeNews,       'n', openFeeds},
        {Str::HomeWeb,        'w', openWebPrompt},
        {Str::HomeUpdate,     'u', openUpdateDict},
        {Str::HomeSettings,   'c', openSettings},
        {Str::HomeAbout,      'a', openAbout},
    };

    auto screen = std::make_unique<ListScreen>("TDeckJisho", std::vector<ListItem>{},
                                               [](App& app, int i) { kEntries[i].action(app); });
    screen->setRoot(true);
    // The Home screen stays on the stack while the language screen is open, so it
    // must refill its texts when it comes back (see ListScreen::setRebuild). The same goes
    // for the dictionary: without one, "Update dictionary" reads "Download dictionary", and
    // after a successful download it turns back. (`app` is the one global App: it outlives
    // every screen, so capturing it by reference is safe.)
    screen->setRebuild([&app](ListScreen& s) {
        std::vector<ListItem> items;
        for (const Entry& e : kEntries) {
            const Str label = (e.label == Str::HomeUpdate && !app.dict().isOpen()) ? Str::HomeDownload : e.label;
            items.push_back({tr(label), std::string(1, e.shortcut)});
        }
        s.list().items = std::move(items);
        s.setHints(tr(Str::HomeHints));
    });
    screen->setKeyHandler([](App& app, ListScreen&, int k) {
        for (const Entry& e : kEntries)
            if (k == e.shortcut) { e.action(app); return true; }
        return false;
    });
    return screen;
}

// ============================================================================
//  Dictionary
// ============================================================================
void openDictionary(App& app) { app.push(std::make_unique<SearchScreen>()); }

void openQuiz(App& app) {
    if (!app.dict().isOpen()) { app.toast(tr(Str::QuizNoDict)); return; }
    // The seed only has to differ between runs: the time since boot at the moment of the tap.
    app.push(std::make_unique<QuizScreen>(app.dict(), hal::millis() * 2654435761u, &app.quizProgress()));
}

rt::Doc entryToDoc(const DictEntry& e) {
    rt::Doc doc;
    doc.title = "辞書: " + e.headword();
    rt::Builder b(doc);

    b.paragraph(rt::H1);
    b.text(e.headword());
    if (e.common) { b.code(rt::DIM); b.text(tr(Str::EntryCommon)); b.code(rt::DIM); }

    // Readings (only interesting if there is a kanji headword).
    if (!e.kanji.empty()) {
        b.paragraph();
        std::string r;
        for (size_t i = 0; i < e.readings.size(); ++i) r += (i ? "・" : "") + e.readings[i];
        b.text(r);
    }
    b.rule();

    for (size_t i = 0; i < e.senses.size(); ++i) {
        const auto& s = e.senses[i];
        b.paragraph();
        b.code(rt::ACCENT);
        b.text(std::to_string(i + 1) + ".");
        b.code(rt::ACCENT);
        b.text(" ");
        if (!s.tags.empty()) {
            b.code(rt::DIM);
            b.text("[" + s.tags + "] ");
            b.code(rt::DIM);
        }
        std::string g;
        for (size_t j = 0; j < s.glosses.size(); ++j) g += (j ? "; " : "") + s.glosses[j];
        b.text(g);
    }

    if (e.kanji.size() > 1) {
        b.paragraph(rt::SMALL);
        std::string other = tr(Str::EntryOtherForms);
        for (size_t i = 1; i < e.kanji.size(); ++i) other += (i > 1 ? "、" : "") + e.kanji[i];
        b.text(other);
    }
    b.paragraph();
    return doc;
}

void openEntry(App& app, uint32_t offset) {
    DictEntry e;
    if (!app.dict().loadEntry(offset, e)) { app.toast(tr(Str::EntryReadFail)); return; }
    app.push(std::make_unique<DocumentScreen>(entryToDoc(e)));
}

// ============================================================================
//  Library (files on the SD card)
// ============================================================================
void openLibrary(App& app) {
    if (!hal::storageReady()) { app.toast(tr(Str::NoSdCard)); return; }
    app.push(std::make_unique<FileBrowserScreen>(paths::kBooks));
}

void openFile(App& app, const std::string& path) {
    // `[path]` copies the string into the lambda. The lambda runs LATER (in
    // LoadingScreen::update), when the caller's `path` may no longer exist.
    app.push(std::make_unique<LoadingScreen>(
        basename(path), fmt(Str::OpeningFile, {{"name", basename(path)}}),
        [path](App&, std::string& err) -> std::unique_ptr<Screen> {
            if (isImageFile(path)) {
                // A one-picture document. The src is relative to the picture's own
                // folder, so it resolves to the file itself; a picture that can't be
                // shown (too big, damaged) gets the usual icon + caption in its place.
                rt::Doc doc;
                rt::Builder b(doc);
                b.image(fileNameAsSrc(basename(path)), basename(path));
                doc.title = basename(path);
                rt::resolveImages(doc, path);
                // Inside a .md/.html file the caption (alt text) tells what is missing;
                // here the caption is just the file name, so say what is wrong and how to fix it.
                if (doc.images[0].progressive) {
                    err = fmt(Str::ProgressiveJpeg, {{"name", basename(path)}});
                    return nullptr;
                }
                return std::make_unique<DocumentScreen>(std::move(doc), path);
            }
            std::string data;
            if (!hal::readFile(path, data, kMaxBookBytes)) {
                err = tr(Str::ReadFileFail);
                return nullptr;
            }
            if (!utf8::isValid(data)) {
                err = tr(Str::NotUtf8);
                return nullptr;
            }
            rt::Doc doc;
            const std::string ext = extension(path);
            if (ext == "md" || ext == "markdown") rt::parseMarkdown(data, doc);
            else if (ext == "html" || ext == "htm") rt::parseHtml(data, "", rt::Source::Local, doc);
            else rt::parsePlain(data, doc);
            doc.title = basename(path);
            // Pictures: find the files and read their sizes (headers only).
            rt::resolveImages(doc, path);
            // `data` (the raw file) is freed here when the lambda returns;
            // only the parsed doc stays in memory.
            return std::make_unique<DocumentScreen>(std::move(doc), path);
        }));
}

// ============================================================================
//  News (RSS / Atom)
// ============================================================================
void openFeeds(App& app) {
    const auto feeds = loadFeeds();
    std::vector<ListItem> items;
    for (const auto& f : feeds) items.push_back({f.name, url::host(f.url)});
    auto screen = std::make_unique<ListScreen>(
        tr(Str::FeedsTitle), std::move(items),
        [feeds](App& a, int i) { openFeed(a, feeds[i].name, feeds[i].url); }, true);
    screen->setEmptyText(fmt(Str::FeedsEmpty, {{"path", kFeedsPath}}));
    screen->setHints(tr(Str::FeedsHints));
    screen->setKeyHandler([](App& a, ListScreen&, int k) {
        if (k != 'a') return false;
        a.push(std::make_unique<InputScreen>(
            tr(Str::AddFeedTitle), tr(Str::AddFeedPrompt), "https://", false,
            [](App& a2, const std::string& typed) {
                std::string text;
                hal::readFile(kFeedsPath, text, 32 * 1024);
                const std::string u = url::normalizeTyped(typed);
                if (!url::isWebUrl(u)) { a2.toast(fmt(Str::CantOpen, {{"url", u}})); return; }
                text += url::host(u) + " | " + u + "\n";
                hal::writeFile(kFeedsPath, text);
                a2.pop();                 // close the old feed list...
                openFeeds(a2);            // ...and open a fresh one
            }));
        return true;
    });
    app.push(std::move(screen));
}

void openFeed(App& app, const std::string& name, const std::string& feedUrl) {
    if (!requireNetwork(app)) return;
    app.push(std::make_unique<WebLoadScreen>(
        name, feedUrl, kMaxWebBytes,
        [name](App&, hal::HttpResult& r, std::string& err) -> std::unique_ptr<Screen> {
            ensureUtf8(r);                // feeds are input from outside, too
            return feedScreen(name, r.body, err);
        }));
}

// ============================================================================
//  Web
// ============================================================================
void openWebPrompt(App& app) {
    app.push(std::make_unique<InputScreen>(
        tr(Str::HomeWeb), tr(Str::WebPrompt),
        app.settings().get("last_url", "https://"), false,
        [](App& a, const std::string& typed) {
            if (str::trim(typed).empty()) return;
            a.settings().set("last_url", typed);
            a.settings().save();
            openUrl(a, url::normalizeTyped(typed));
        }));
}

void openUrl(App& app, const std::string& target) {
    if (!str::startsWith(target, "http://") && !str::startsWith(target, "https://")) {
        app.toast(fmt(Str::CantOpen, {{"url", target}}));
        return;
    }
    if (!requireNetwork(app)) return;
    app.push(std::make_unique<WebLoadScreen>(
        url::host(target), target, kMaxWebBytes,
        [](App&, hal::HttpResult& r, std::string& err) -> std::unique_ptr<Screen> {
            ensureUtf8(r);
            if (looksLikeFeed(r.body)) return feedScreen(url::host(r.finalUrl), r.body, err);

            rt::Doc doc;
            if (str::startsWith(str::toLower(r.contentType), "text/plain")) rt::parsePlain(r.body, doc);
            else rt::parseHtml(r.body, r.finalUrl, rt::Source::Web, doc);
            if (doc.title.empty()) doc.title = url::host(r.finalUrl);
            if (r.truncated) {
                rt::Builder b(doc);
                b.paragraph(rt::SMALL);
                b.text(tr(Str::PageTruncated));
            }
            return std::make_unique<DocumentScreen>(std::move(doc));
        }));
}

// ============================================================================
//  Touch test
// ============================================================================
void openTouchTest(App& app) { app.push(std::make_unique<TouchTestScreen>()); }

// ============================================================================
//  Wi-Fi, About
// ============================================================================
void openWifi(App& app) { app.push(std::make_unique<WifiScreen>()); }
void openUpdateDict(App& app) { app.push(std::make_unique<UpdateDictScreen>()); }

// The language screen: each language is named in ITS OWN language, so a user who
// ended up in a language they can't read still finds theirs.
void openLanguage(App& app) {
    std::vector<ListItem> items;
    for (size_t i = 0; i < static_cast<size_t>(i18n::Lang::Count); ++i) {
        const auto l = static_cast<i18n::Lang>(i);
        items.push_back({i18n::nativeName(l), i18n::code(l), l == i18n::language()});
    }
    app.push(std::make_unique<ListScreen>(tr(Str::LangTitle), std::move(items), [](App& a, int i) {
        const auto l = static_cast<i18n::Lang>(i);
        i18n::setLanguage(l);
        a.settings().set("lang", i18n::code(l));      // remembered across reboots
        a.settings().save();
        a.pop();   // back to Settings, which refills its texts in onEnter() (Home too, later)
    }));
}

// ============================================================================
//  Settings
// ============================================================================
namespace {

std::string showDuration(int s) {
    if (s <= 0) return tr(Str::SetNever);
    if (s < 60 || s % 60) return fmt(Str::UnitSeconds, {{"n", std::to_string(s)}});
    return fmt(Str::UnitMinutes, {{"n", std::to_string(s / 60)}});
}
std::string showPercent(int p) { return std::to_string(p) + "%"; }

// One row of the settings screen. A TABLE, like the Home menu: a new setting is one more
// line here (plus its strings), no new screen code. The value is always an int in
// settings.txt; `choices` are what ← → step through, in order.
struct SettingDef {
    Str label;
    const char* key;
    int fallback;
    std::vector<int> choices;
    std::string (*show)(int);   // value → the text on the right
};

const std::vector<SettingDef>& settingDefs() {
    // A function-local static: built on first use, then kept. (A global vector would be
    // built before main() – harmless here, but this way the order of construction is clear.)
    static const std::vector<SettingDef> kDefs = {
        {Str::SetBrightness, "brightness", 100, {10, 20, 30, 40, 50, 60, 70, 80, 90, 100}, showPercent},
        {Str::SetSleep,      "sleep_s",    120, {30, 60, 120, 300, 600},                    showDuration},
        {Str::SetWifiOff,    "wifi_off_s", 60,  {0, 60, 120, 300, 600},                     showDuration},
        {Str::SetTimeZone,   "tz_min",     0,   tz::choices(),                           tz::label},
    };
    return kDefs;
}

// Where the stored value sits among the choices. A hand-edited value that isn't one of
// them (sleep_s=90) maps to the first choice at or above it, so ← → still move sensibly.
size_t choiceIndex(const SettingDef& d, int value) {
    for (size_t i = 0; i < d.choices.size(); ++i)
        if (d.choices[i] >= value) return i;
    return d.choices.size() - 1;
}

// Below the values: rows that open a screen of their own. Things you set up once and
// then rarely touch, so they live here and not on Home. The key works on the Settings screen.
struct SettingLink {
    Str label;
    char shortcut;
    void (*open)(App&);
};
const SettingLink kLinks[] = {
    {Str::SetWifi,     's', openWifi},
    {Str::SetLanguage, 'g', openLanguage},
    {Str::SetTouch,    'p', openTouchTest},
};

}  // namespace

void openSettings(App& app) {
    auto screen = std::make_unique<ListScreen>(tr(Str::HomeSettings), std::vector<ListItem>{},
                                               [](App&, int) {});   // Enter: see the key handler
    screen->setRebuild([&app](ListScreen& s) {
        // Title and hints too, not only the items: we are on the stack while the
        // Language screen is open, and come back to a new language.
        s.setTitle(tr(Str::HomeSettings));
        s.setHints(tr(Str::SetHints));
        std::vector<ListItem> items;
        for (const SettingDef& d : settingDefs())
            items.push_back({tr(d.label), d.show(app.settings().getInt(d.key, d.fallback))});
        for (const SettingLink& l : kLinks) items.push_back({tr(l.label), std::string(1, l.shortcut)});
        s.list().items = std::move(items);
    });
    // ← → step through the choices (stopping at the ends); Enter steps forward and wraps,
    // so the trackball click alone can reach every value. Each change is saved at once:
    // a key press is rare enough for an SD write, and nothing is lost if the battery dies.
    screen->setKeyHandler([](App& a, ListScreen& s, int k) {
        for (const SettingLink& l : kLinks)
            if (k == l.shortcut) { l.open(a); return true; }
        const size_t row = static_cast<size_t>(s.list().selected);
        if (row >= settingDefs().size()) {                       // a link row
            // ← → would PAGE the list (ListView's default) and jump to the top row, where the
            // next ← → changes the brightness by surprise. The list fits on one screen: ignore them.
            if (k == key::Left || k == key::Right) return true;
            if (k != key::Enter && k != key::Select) return false;
            kLinks[row - settingDefs().size()].open(a);
            return true;
        }
        if (k != key::Left && k != key::Right && k != key::Enter && k != key::Select) return false;
        const SettingDef& d = settingDefs()[row];
        const size_t n = d.choices.size();
        size_t i = choiceIndex(d, a.settings().getInt(d.key, d.fallback));
        if (k == key::Left) i = i > 0 ? i - 1 : 0;
        else if (k == key::Right) i = std::min(i + 1, n - 1);
        else i = (i + 1) % n;
        a.settings().setInt(d.key, d.choices[i]);
        a.settings().save();
        a.applyBacklight();       // so brightness changes are seen while choosing
        a.applyTimeZone();        // and the status bar clock moves at once
        s.rebuild();
        return true;
    });
    app.push(std::move(screen));
}

void openAbout(App& app) {
    rt::Doc doc;
    doc.title = tr(Str::HomeAbout);
    rt::Builder b(doc);
    b.paragraph(rt::H1);
    b.text("TDeckJisho");
    b.paragraph();
    b.text(tr(Str::AboutDesc));
    b.paragraph(rt::SMALL);
    // JISHO_VERSION and __DATE__ are filled in by the compiler
    b.text(fmt(Str::AboutVersion, {{"v", JISHO_VERSION}, {"d", __DATE__}}));

    b.paragraph(rt::H2);
    b.text(tr(Str::AboutData));
    std::string info;
    if (hal::readFile(paths::kDict + "/info.txt", info, 4096)) {
        for (auto line : str::split(info, '\n'))
            if (!line.empty()) { b.paragraph(rt::BULLET); b.text(line); }
    } else {
        b.paragraph();
        b.text(fmt(Str::AboutNotInstalled, {{"err", app.dict().error()}}));
    }
    // The acknowledgement the EDRDG licence asks for, on a screen reachable
    // from the menu (a splash screen would not be enough). Wording follows
    // https://www.edrdg.org/edrdg/sample.html. It is NOT in Strings.def: the licence
    // text comes from the EDRDG and stays English in every language.
    b.paragraph();
    b.text("This package uses the JMdict/EDICT dictionary files. These files are the property of "
           "the Electronic Dictionary Research and Development Group, and are used in conformance "
           "with the Group's licence. The converted files are distributed under CC BY-SA 4.0.");
    b.paragraph(rt::BULLET);
    b.beginLink("https://www.edrdg.org/wiki/index.php/JMdict-EDICT_Dictionary_Project");
    b.text("JMdict project");
    b.endLink();
    b.paragraph(rt::BULLET);
    b.beginLink("https://www.edrdg.org/edrdg/licence.html");
    b.text("EDRDG licence");
    b.endLink();
    b.paragraph(rt::SMALL);
    b.text(fmt(Str::AboutKeepCurrent, {{"path", paths::kRoot + "/LICENCES/JMDICT-CREDITS.txt"}}));

    b.paragraph(rt::H2);
    b.text(tr(Str::AboutSystem));
    const hal::MemInfo m = hal::memInfo();
    b.paragraph(rt::BULLET);
    b.text(fmt(Str::AboutRam, {{"i", std::to_string(m.freeInternal / 1024)},
                               {"p", std::to_string(m.freePsram / 1024)},
                               {"t", std::to_string(m.totalPsram / 1024)}}));
    b.paragraph(rt::BULLET);
    b.text(fmt(Str::AboutSd, {{"state", tr(hal::storageReady() ? Str::AboutSdOk : Str::AboutSdMissing)}}));
    b.paragraph(rt::BULLET);
    if (const int mv = hal::batteryMillivolts(); mv >= 0) {
        char volts[16];
        snprintf(volts, sizeof volts, "%d.%02d", mv / 1000, mv % 1000 / 10);
        b.text(fmt(Str::AboutBattery, {{"v", volts}, {"p", std::to_string(hal::batteryPercent())}}));
    } else {
        b.text(tr(Str::AboutBatteryNone));
    }
    b.paragraph(rt::BULLET);
    b.text(fmt(Str::AboutWifi, {{"state", hal::wifiState() == hal::WifiState::Connected
                                              ? hal::wifiSsid() + " " + hal::wifiIp()
                                              : std::string(tr(Str::AboutWifiOff))}}));

    b.paragraph(rt::H2);
    b.text(tr(Str::AboutCredits));
    b.paragraph(rt::BULLET);
    b.text(tr(Str::AboutLovyan));
    b.paragraph(rt::BULLET);
    b.text(tr(Str::AboutIpa));
    b.paragraph(rt::BULLET);
    b.text(tr(Str::AboutEfont));
    b.paragraph(rt::SMALL);
    b.text(tr(Str::AboutLicence));
    b.paragraph();
    app.push(std::make_unique<DocumentScreen>(std::move(doc)));
}

}  // namespace actions
