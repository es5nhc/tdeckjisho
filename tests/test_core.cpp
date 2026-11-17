// ============================================================================
//  tests/test_core.cpp – unit tests for everything that is not hardware
// ============================================================================
//
//  Build and run (from the project root): tests/run.sh, which does
//
//    g++ -std=c++17 -g -fsanitize=address,undefined -Isrc tests/test_core.cpp
//        src/core/*.cpp src/dict/*.cpp src/text/*.cpp src/hal/sdl/Storage.cpp
//        -o .pio/test_core && .pio/test_core
//  (one command – or simply run tests/run.sh)
//
//  Why not PlatformIO's `pio test` with Unity? It works too, but needs an
//  extra download, and this shows the real point: code that does not touch
//  hardware is ordinary C++ and can be tested with nothing but a compiler.
//  That is the payoff of keeping hal/ separate.
//
//  Python analogy: a pytest file of `assert`s. CHECK() below prints the file
//  and line of every failure and keeps going, then main() returns non-zero.
// ============================================================================

#include <algorithm>   // std::min, std::copy, std::find
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <string>

#include "core/Deinflect.hpp"
#include "core/I18n.hpp"
#include "core/Romaji.hpp"
#include "core/Settings.hpp"
#include "core/Url.hpp"
#include "core/WifiStore.hpp"
#include "core/Utf8.hpp"
#include "dict/DictBuilder.hpp"
#include "dict/Dictionary.hpp"
#include "dict/Quiz.hpp"
#include "text/Images.hpp"
#include "text/Cursor.hpp"
#include "text/Feed.hpp"
#include "text/Layout.hpp"
#include "text/Parsers.hpp"

static int g_failures = 0;
static int g_checks = 0;

// A macro (not a function) so that __FILE__, __LINE__ and the expression text
// #expr refer to the place where CHECK is *used*.
#define CHECK(expr)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(expr)) {                                                       \
            ++g_failures;                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr);      \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        ++g_checks;                                                          \
        const auto _va = (a);                                                \
        const auto _vb = (b);                                                \
        if (!(_va == _vb)) {                                                 \
            ++g_failures;                                                    \
            std::printf("FAIL %s:%d  %s == %s\n   got: [%s]\n  want: [%s]\n", \
                        __FILE__, __LINE__, #a, #b, std::string(_va).c_str(), \
                        std::string(_vb).c_str());                           \
        }                                                                    \
    } while (0)

// hal::log() and friends: tests/hal_stub.cpp

// Every glyph 8 px wide (CJK 16 px), every line 10 px: predictable layout.
struct FakeMetrics : rt::Metrics {
    int advance(uint32_t cp, char, uint8_t) override { return utf8::isCJK(cp) ? 16 : 8; }
    int lineHeight(char) override { return 10; }
    bool showRuby() const override { return true; }
};

static std::string lineText(const rt::Doc& d, const rt::Line& l) {
    std::string s;
    for (uint32_t i = l.start; i < l.end; ++i)
        if (static_cast<uint8_t>(d.text[i]) >= 0x20 || d.text[i] < 0) s += d.text[i];
    return s;
}

static void testUtf8() {
    std::string s = "aõ日😀";
    size_t i = 0;
    CHECK(utf8::next(s, i) == 'a');
    CHECK(utf8::next(s, i) == 0xF5);
    CHECK(utf8::next(s, i) == 0x65E5);
    CHECK(utf8::next(s, i) == 0x1F600);
    CHECK(i == s.size());
    CHECK(utf8::length(s) == 4);
    CHECK(utf8::isValid(s));
    CHECK(!utf8::isValid("\xE6\x97"));                 // truncated 日
    // Hostile input: each of these must be REFUSED, not decoded.
    CHECK(!utf8::isValid("\xC0\x81"));                 // overlong U+0001
    CHECK(!utf8::isValid("\xC0\xAF"));                 // overlong '/'
    CHECK(!utf8::isValid("\xE0\x80\x80"));             // overlong NUL, 3 bytes
    CHECK(!utf8::isValid("\xED\xA0\x80"));             // surrogate U+D800
    CHECK(!utf8::isValid("\xF4\x90\x80\x80"));         // U+110000
    CHECK(!utf8::isValid("\xF7\xBF\xBF\xBF"));         // lead byte F7
    CHECK(utf8::isValid("\xF4\x8F\xBF\xBF"));          // U+10FFFF: the last real one
    CHECK_EQ(std::string(utf8::truncateBytes("日本語", 7)), std::string("日本"));
    CHECK_EQ(utf8::katakanaToHiragana("コーヒー"), std::string("こーひー"));
    std::string out;
    utf8::append(out, 0x17E);
    CHECK_EQ(out, std::string("ž"));
    out.clear();
    utf8::append(out, 0xD800);                         // never WRITE a non-character
    utf8::append(out, 0x110000);
    CHECK_EQ(out, std::string("\xEF\xBF\xBD\xEF\xBF\xBD"));   // two U+FFFD
}

static void testRomaji() {
    auto k = [](const char* s) { return romaji::toHiragana(s).kana; };
    CHECK_EQ(k("taberu"), std::string("たべる"));
    CHECK_EQ(k("tabemono"), std::string("たべもの"));
    CHECK_EQ(k("shinbun"), std::string("しんぶん"));
    CHECK_EQ(k("sinbun"), std::string("しんぶん"));
    CHECK_EQ(k("konnichiha"), std::string("こんにちは"));
    CHECK_EQ(k("konnnichiha"), std::string("こんにちは"));
    CHECK_EQ(k("kitte"), std::string("きって"));
    CHECK_EQ(k("matcha"), std::string("まっちゃ"));
    CHECK_EQ(k("toukyou"), std::string("とうきょう"));
    CHECK_EQ(k("ko-hi-"), std::string("こーひー"));
    CHECK_EQ(k("kan'i"), std::string("かんい"));
    CHECK_EQ(k("jisho"), std::string("じしょ"));
    CHECK_EQ(k("TABERU"), std::string("たべる"));
    auto r = romaji::toHiragana("taber");
    CHECK_EQ(r.kana, std::string("たべ"));
    CHECK_EQ(r.tail, std::string("r"));
    CHECK(r.clean);
    CHECK(!romaji::toHiragana("qqqa").clean);
}

static void testUrl() {
    const char* base = "https://example.com/news/today.html?x=1";
    CHECK_EQ(url::resolve(base, "item.html"), std::string("https://example.com/news/item.html"));
    CHECK_EQ(url::resolve(base, "../about"), std::string("https://example.com/about"));
    CHECK_EQ(url::resolve(base, "/rss.xml"), std::string("https://example.com/rss.xml"));
    CHECK_EQ(url::resolve(base, "//cdn.org/a"), std::string("https://cdn.org/a"));
    CHECK_EQ(url::resolve(base, "http://x.ee/"), std::string("http://x.ee/"));
    CHECK_EQ(url::resolve("https://a.b", "c"), std::string("https://a.b/c"));
    CHECK_EQ(url::host(base), std::string("example.com"));
    CHECK_EQ(url::normalizeTyped("err.ee"), std::string("https://err.ee"));
}

static void testMarkdown() {
    rt::Doc d;
    rt::parseMarkdown("# Pealkiri\n\nTere **maailm** ja [link](http://x).\n- üks\n- kaks\n\n{漢字|かんじ}", d);
    CHECK_EQ(d.text, std::string("\x11Pealkiri\nTere \x01maailm\x01 ja \x03link\x04.\n"
                                 "\x13üks\n\x13kaks\n\x07漢字\x02かんじ\x02"));
    CHECK(d.links.size() == 1 && d.links[0].url == "http://x" && d.links[0].label == "link");

    rt::Doc j;
    rt::parseMarkdown("日本\n語です\nand\nmore", j);   // CJK+CJK joins without space
    CHECK_EQ(j.text, std::string("日本語です and more"));
}

static void testAozora() {
    rt::Doc d;
    rt::parsePlain("吾輩《わがはい》は｜猫《ねこ》である。［＃改ページ］\n\nつぎ", d);
    CHECK_EQ(d.text, std::string("\x07吾輩\x02わがはい\x02は\x07猫\x02ねこ\x02である。\x06［＃改ページ］\x06\n\nつぎ"));
}

static void testHtml() {
    rt::Doc d;
    rt::parseHtml("<html><head><title>T &amp; T</title><script>x()</script></head>"
                  "<body><nav>menu</nav><article><h1>Uudis</h1><p>S&otilde;na <b>x</b>"
                  " <a href='/a'>A</a></p><ruby>漢<rt>かん</rt></ruby></article></body></html>",
                  "https://err.ee/news/1", rt::Source::Web, d);
    CHECK_EQ(d.title, std::string("T & T"));
    CHECK_EQ(d.text, std::string("\x11Uudis\nSõna \x01x\x01 \x03" "A\x04\n\x07漢\x02かん\x02\x07\n"));
    CHECK(d.links.size() == 1 && d.links[0].url == "https://err.ee/a");
    CHECK_EQ(rt::decodeEntities("&#26085;&#x672C;&zcaron;&bogus;"), std::string("日本ž&bogus;"));

    // A news teaser: the whole block is one link, the headline in an <h2> inside it (err.ee).
    rt::Doc teaser;
    rt::parseHtml("<p><a aria-label='Haiglate t&ouml;&ouml;d' href='//www.err.ee/16/x'><span><img alt='Haigla'"
                  " src='a.jpg'><div class='live'>OTSE</div></span><h2><span>Haiglate t&ouml;&ouml;d</span></h2></a></p>",
                  "https://www.err.ee/", rt::Source::Web, teaser);
    CHECK(teaser.links.size() == 1);
    CHECK_EQ(teaser.links[0].label, std::string("Haiglate tööd"));      // the aria-label, no "OTSE"
    CHECK_EQ(teaser.links[0].url, std::string("https://www.err.ee/16/x"));
    rt::Doc plainTeaser;                                                 // no aria-label: the heading text
    rt::parseHtml("<a href='/y'><h2>Pealkiri</h2></a><p>Muu</p>", "https://err.ee/", rt::Source::Web, plainTeaser);
    CHECK(plainTeaser.links.size() == 1);
    CHECK_EQ(plainTeaser.links[0].label, std::string("Pealkiri"));

    // A '>' inside a quoted attribute does not end the tag (postimees.ee: "event => ...").
    rt::Doc arrow;
    rt::parseHtml("<p>Uudis <span onkeydown=\"event => event.key === 'Enter' ? go() : 0\" "
                  "data-x='a>b' title=it's>(16)</span></p>", "https://x/", rt::Source::Web, arrow);
    CHECK_EQ(arrow.text, std::string("Uudis (16)\n"));
    // A quote that is never closed: the tag ends at the first '>' as before.
    rt::Doc broken;
    rt::parseHtml("<p><a href=\"/x>Lugu</a></p>", "https://x/", rt::Source::Web, broken);
    CHECK(broken.text.find("Lugu") != std::string::npos);
    // <picture> with light and dark variants (postimees.ee): one caption, not two.
    rt::Doc pic;
    rt::parseHtml("<picture><source srcset='a.webp'><img src='l.png' alt='Foto'> "
                  "<img src='d.png' alt='Foto'></picture><img src='x.png' alt='Teine'>",
                  "https://x/", rt::Source::Web, pic);
    CHECK_EQ(pic.text, std::string("\x06[Foto]\x06 \x06[Teine]\x06\n"));
}

// Hostile input: what a malicious web page could send. Each case must finish quickly
// (linear time). The limit is generous on purpose: a quadratic parser needs MINUTES for
// these sizes, a linear one a few milliseconds, so the test is not flaky.
template <typename F>
static double seconds(F f) {
    const auto t0 = std::chrono::steady_clock::now();
    f();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

static void testHostileText() {
    const size_t n = 300000;
    rt::Doc d;
    const std::string amps = "<p>" + std::string(n, '&') + "</p>";
    CHECK(seconds([&] { rt::parseHtml(amps, "https://x/", rt::Source::Web, d); }) < 2.0);
    std::string quotes;                                                  // <b x="> with no closing "
    for (size_t k = 0; k < n / 8; ++k) quotes += k ? "<b x='>" : "<b x=\">";
    CHECK(seconds([&] { d = rt::Doc{}; rt::parseHtml(quotes, "https://x/", rt::Source::Web, d); }) < 2.0);
    std::string guillemets;
    for (size_t k = 0; k < n / 3; ++k) guillemets += "\xE3\x80\x8A";       // 《 without 》
    CHECK(seconds([&] { d = rt::Doc{}; rt::parsePlain(guillemets, d); }) < 2.0);
    for (char c : {'[', '{', '!'}) {
        const std::string md(n, c);                                      // [[[[ {{{{ !!!!
        CHECK(seconds([&] { d = rt::Doc{}; rt::parseMarkdown(md, d); }) < 2.0);
    }
    // Entities that are not characters become U+FFFD, never invalid UTF-8.
    CHECK_EQ(rt::decodeEntities("&#x110000;&#xD800;"), std::string("\xEF\xBF\xBD\xEF\xBF\xBD"));
    CHECK(utf8::isValid(rt::decodeEntities("&#1114112;&#55296;&#-1;&#99999999999;")));
    // Upper-case tags are found without copying the page.
    d = rt::Doc{};
    rt::parseHtml("<HTML><BODY>menu<MAIN><P>story</P></MAIN></BODY></HTML>", "https://x/", rt::Source::Web, d);
    CHECK(d.text.find("menu") == std::string::npos);
    CHECK(d.text.find("story") != std::string::npos);
}

static void testFeed() {
    const char* rss = "<rss><channel><title>NHK</title>"
                      "<item><title>A</title><link>http://a</link><pubDate>Mon</pubDate>"
                      "<description><![CDATA[<p>hi</p>]]></description></item>"
                      "<item><title>B &amp; C</title><link>http://b</link></item></channel></rss>";
    rt::Feed f;
    CHECK(rt::parseFeed(rss, f));
    CHECK_EQ(f.title, std::string("NHK"));
    CHECK(f.items.size() == 2);
    CHECK_EQ(f.items[0].summaryHtml, std::string("<p>hi</p>"));
    CHECK_EQ(f.items[1].title, std::string("B & C"));

    const char* atom = "<feed><title>Blog</title><entry><title>X</title>"
                       "<link rel=\"alternate\" href=\"http://x\"/><updated>2026</updated></entry></feed>";
    CHECK(rt::parseFeed(atom, f));
    CHECK_EQ(f.items[0].link, std::string("http://x"));
    // A '>' inside a quoted attribute does not end the tag.
    const char* gtAttr = "<rss><channel><title type=\"a>b\">Uudised</title>"
                         "<item><title>Y</title></item></channel></rss>";
    CHECK(rt::parseFeed(gtAttr, f));
    CHECK_EQ(f.title, std::string("Uudised"));

    // Dates: the same moment in two formats and two time zones; 0 for nonsense.
    CHECK(rt::feedTime("Sun, 15 Nov 2026 06:21:00 +0900") == rt::feedTime("2026-11-14T21:21:00Z"));
    CHECK(rt::feedTime("2026-11-15T06:21:00+09:00") == rt::feedTime("Sat, 14 Nov 2026 21:21:00 GMT"));
    CHECK(rt::feedTime("1970-01-02T00:00:00Z") == 86400);
    CHECK(rt::feedTime("Mon") == 0 && rt::feedTime("") == 0);
    // Newest first, whatever order the feed uses; undated last.
    const char* mixed = "<rss><channel><title>N</title>"
                        "<item><title>old</title><pubDate>Sat, 14 Nov 2026 08:00:00 +0900</pubDate></item>"
                        "<item><title>none</title></item>"
                        "<item><title>new</title><pubDate>Sun, 15 Nov 2026 06:00:00 +0900</pubDate></item>"
                        "<item><title>mid</title><pubDate>Sat, 14 Nov 2026 20:00:00 GMT</pubDate></item>   /* = 05:00 in Japan */"
                        "</channel></rss>";
    CHECK(rt::parseFeed(mixed, f) && f.items.size() == 4);
    CHECK_EQ(f.items[0].title + f.items[1].title + f.items[2].title + f.items[3].title,
             std::string("newmidoldnone"));
    // A download cut off right after "<item": no crash, no item invented.
    {
        const std::string cut = "<rss><channel><title>T</title><item";
        rt::Feed f;
        CHECK(!rt::parseFeed(std::string_view(cut.data(), cut.size()), f));
        CHECK(f.items.empty());
    }
}

// Round trip with nasty input: what goes in must come out, and nothing else.
static void testSettingsRoundTrip() {
    const std::string path = "/tdeckjisho/build-test/settings-test.txt";
    hal::makeDirs("/tdeckjisho/build-test");
    Settings w;
    w.load(path);                                          // missing is fine: sets the path
    w.set("last_url", "https://x/?a=b\nlang=ja");           // a line break in a value
    w.setInt("/books/a=b c.txt", 7);                        // '=' and a space in a key
    w.set("#not-a-comment", "1");
    CHECK(w.save());
    Settings r;
    CHECK(r.load(path));
    CHECK_EQ(r.get("lang", "none"), std::string("none"));  // no injected setting
    CHECK_EQ(r.get("last_url"), std::string("https://x/?a=b lang=ja"));
    CHECK(r.getInt("/books/a=b c.txt", 0) == 7);
    CHECK_EQ(r.get("#not-a-comment"), std::string("1"));
}

// The network policy, and secrets that must not grow extra lines.
static void testNetPolicy() {
    CHECK(url::isWebUrl("https://www.err.ee/"));
    CHECK(url::isWebUrl("HTTP://example.com/"));
    CHECK(!url::isWebUrl("file:///etc/passwd"));
    CHECK(!url::isWebUrl("ftp://example.com/x"));
    CHECK(!url::isWebUrl("javascript:alert(1)"));
    CHECK(!url::isWebUrl("https://a/x\r\nHost: evil"));   // header injection
    CHECK(url::redirectProblem("http://a/", "https://a/").empty());     // upgrade: fine
    CHECK(!url::redirectProblem("https://a/", "http://a/").empty());    // downgrade: refused
    CHECK(!url::redirectProblem("https://a/", "file:///x").empty());
    rememberWifi({"Home", "pw"});
    rememberWifi({"Evil\nInjected", "x"});            // a line break in the SSID: refused
    rememberWifi({"Cafe", std::string(65, 'p')});     // longer than WPA2 allows: refused
    const auto creds = loadWifiCreds();
    CHECK(creds.size() == 1);
    CHECK_EQ(creds[0].ssid, std::string("Home"));
}

static void testLayout() {
    FakeMetrics m;
    {
        rt::Doc d;
        rt::Builder b(d);
        b.paragraph();
        b.text("aaa bbb ccc");            // 11 chars * 8 px = 88 px
        // Width 64: "aaa bbb " fits exactly (64 px), "ccc" wraps.
        auto lines = rt::layout(d, 64, m);
        CHECK(lines.size() == 2);
        CHECK_EQ(lineText(d, lines[0]), std::string("aaa bbb "));
        CHECK_EQ(lineText(d, lines[1]), std::string("ccc"));
    }
    {
        rt::Doc d;
        rt::Builder b(d);
        b.paragraph();
        b.text("日本語。日本");            // 6 CJK chars * 16 px
        // Width 56 = 3.5 chars. "日本語" fits, but "。" may not start a line,
        // so the break moves one character earlier: 日本 / 語。日 / 本.
        auto lines = rt::layout(d, 56, m);
        CHECK(lines.size() == 3);
        CHECK_EQ(lineText(d, lines[0]), std::string("日本"));
        CHECK_EQ(lineText(d, lines[1]), std::string("語。日"));
        CHECK_EQ(lineText(d, lines[2]), std::string("本"));
    }
}

static void testRubyLayout() {
    // A ruby group is ONE unit: as wide as max(base, ruby), never split.
    FakeMetrics m;                      // CJK = 16 px, everything else 8 px
    rt::Doc d;
    rt::parseMarkdown("あ{一杯|いっぱい}", d);   // base 32 px, ruby 64 px
    auto lines = rt::layout(d, 80, m);  // あ (16) + group (64) = 80: fits exactly
    CHECK(lines.size() == 1);
    lines = rt::layout(d, 79, m);       // one pixel less: the WHOLE group wraps
    CHECK(lines.size() == 2);
    CHECK_EQ(lineText(d, lines[0]), std::string("あ"));
    CHECK_EQ(lineText(d, lines[1]), std::string("一杯いっぱい"));
}

static void testDictionary() {
    // Uses sdcard/jisho built from tools/sample_jmdict.xml.
    Dictionary dict;
    if (!dict.open("/tdeckjisho/jisho")) {
        std::printf("SKIP dictionary tests: %s (run the converter first)\n", dict.error().c_str());
        return;
    }
    auto hits = dict.searchKana("たべ", 10);          // prefix: 食べる and 食べ物
    CHECK(hits.size() == 2);
    hits = dict.searchKana("たべる", 10);
    CHECK_EQ(hits[0].headword, std::string("食べる"));

    hits = dict.searchKana("にほん", 10);
    CHECK(!hits.empty() && hits[0].headword == "日本");   // exact match first

    hits = dict.searchKana("こーひー", 10);
    CHECK(hits.size() == 1 && hits[0].gloss == "coffee");

    hits = dict.searchEnglish("eat", 10);
    CHECK(!hits.empty() && hits[0].headword == "食べる");

    hits = dict.searchEnglish("ice cream", 10);
    CHECK(hits.size() == 1 && hits[0].headword == "氷菓子");

    DictEntry e;
    CHECK(dict.loadEntry(hits[0].offset, e));
    CHECK(e.senses.size() == 1 && e.senses[0].tags == "n");
}

static void testQuiz() {
    Dictionary dict;
    if (!dict.open("/tdeckjisho/jisho")) { std::printf("SKIP quiz tests\n"); return; }
    quiz::Generator gen(dict, 12345);
    int counts[4] = {0, 0, 0, 0};
    for (int round = 0; round < 5; ++round) {
        gen.newRound();
        for (int n = 0; n < quiz::kQuestionsPerRound; ++n) {
            quiz::Question q;
            CHECK(gen.next(q));
            ++counts[static_cast<int>(q.type)];
            CHECK(!q.prompt.empty());
            CHECK(q.correct >= 0 && q.correct < quiz::kChoices);
            for (int i = 0; i < quiz::kChoices; ++i) {
                CHECK(!q.options[i].empty());
                for (int j = i + 1; j < quiz::kChoices; ++j) CHECK(q.options[i] != q.options[j]);
            }
            // The marked option really belongs to the entry the question is about.
            DictEntry e;
            CHECK(dict.loadEntry(q.offset, e));
            const std::string& right = q.options[q.correct];
            if (q.type == quiz::Type::Reading) CHECK(right == e.readings[0] && q.prompt == e.kanji[0]);
            if (q.type == quiz::Type::Kanji) CHECK(right == e.kanji[0] && q.prompt == e.readings[0]);
            if (q.type == quiz::Type::JaToEn) CHECK(right == quiz::meaningText(e) && q.prompt == e.headword());
            if (q.type == quiz::Type::EnToJa) {
                CHECK(right == quiz::wordText(e) && q.prompt == quiz::meaningText(e));
                CHECK(right.find('(') == std::string::npos);   // no reading in the option text
                CHECK(q.readings[q.correct] == quiz::wordReading(e));
            }
        }
    }
    CHECK(counts[0] > 0 && counts[1] > 0 && counts[2] > 0 && counts[3] > 0);

    // Streaks: 10 right answers in a row retire a question; a wrong answer starts over.
    Settings progress;
    hal::makeDirs("/tdeckjisho/build-test");
    progress.load("/tdeckjisho/build-test/quiz-test.txt");
    quiz::Generator pg(dict, 777, &progress);
    quiz::Question q;
    CHECK(pg.next(q));
    for (int i = 0; i < quiz::kMasteredStreak - 1; ++i) pg.recordAnswer(q, true);
    CHECK(pg.streak(q.progressKey) == quiz::kMasteredStreak - 1);
    pg.recordAnswer(q, false);
    CHECK(pg.streak(q.progressKey) == 0);
    for (int i = 0; i < quiz::kMasteredStreak; ++i) pg.recordAnswer(q, true);
    CHECK(pg.streak(q.progressKey) == quiz::kMasteredStreak);
    bool again = false;
    for (int i = 0; i < 300; ++i) {
        quiz::Question q2;
        if (pg.next(q2) && q2.progressKey == q.progressKey) again = true;
    }
    CHECK(!again);                                      // never asked again
    Settings reloaded;
    reloaded.load("/tdeckjisho/build-test/quiz-test.txt");
    CHECK(reloaded.getInt(q.progressKey, 0) == quiz::kMasteredStreak);   // and it was saved   // all three kinds come up
}

// ---- the on-device dictionary builder ---------------------------------------
static std::string fileBytes(const std::string& sdPath) {
    std::string s;
    hal::readFile(sdPath, s, 64 * 1024 * 1024);
    return s;
}

// Hostile JMdict: a download is input like any other. The XML text is in memory here.
class MemSource : public build::ByteSource {
public:
    explicit MemSource(std::string s) : _s(std::move(s)) {}
    size_t read(uint8_t* buf, size_t n) override {
        n = std::min(n, _s.size() - _pos);
        std::copy(_s.begin() + _pos, _s.begin() + _pos + n, buf);
        _pos += n;
        return n;
    }
    uint32_t filePosition() const override { return static_cast<uint32_t>(_pos); }
private:
    std::string _s;
    size_t _pos = 0;
};

static void testHostileJmdict() {
    const std::string head = "<JMdict><entry><r_ele><reb>て</reb></r_ele><sense><gloss>";
    {   // a text node of 1 MB: the reader must stop with an error, not grow forever
        MemSource src(head + std::string(1 << 20, 'a') + "</gloss></sense></entry></JMdict>");
        build::JmdictReader r(src);
        build::JmEntry e;
        CHECK(r.next(e) == build::JmdictReader::Result::Error);
        CHECK(!r.error().empty());
    }
    {   // 100 000 empty senses: no text at all, but still memory
        std::string many;
        for (int k = 0; k < 100000; ++k) many += "<sense/>";
        MemSource src("<JMdict><entry><r_ele><reb>て</reb></r_ele>" + many + "</entry></JMdict>");
        build::JmdictReader r(src);
        build::JmEntry e;
        CHECK(r.next(e) == build::JmdictReader::Result::Error);
    }
    {   // a line break and a TAB inside a gloss become spaces: no extra "sense" line
        MemSource src(head + "one\ntwo\tthree</gloss></sense></entry></JMdict>");
        build::JmdictReader r(src);
        build::JmEntry e;
        CHECK(r.next(e) == build::JmdictReader::Result::Entry);
        CHECK(e.senses.size() == 1 && e.senses[0].glosses.size() == 1);
        if (!e.senses.empty() && !e.senses[0].glosses.empty())
            CHECK_EQ(e.senses[0].glosses[0], std::string("one two three"));
    }
}

static void testEncodeRules() {
    using namespace build;
    // Python's str.lower() turns the KELVIN SIGN into an ASCII k.
    CHECK_EQ(pyLower("\xE2\x84\xAA" "ilo"), std::string("kilo"));
    CHECK_EQ(pyStrip("\xE3\x80\x80 eat\xC2\xA0"), std::string_view("eat"));   // U+3000, NBSP
    CHECK_EQ(glossCore("To eat (food)"), std::string("eat"));
    CHECK_EQ(glossCore("a (b (c) d) e"), std::string("a  d) e"));   // re.sub does not nest
    CHECK(priority({"news1", "ichi1", "nf01"}) == 74);
    CHECK(isCommon({"spec2"}) && !isCommon({"news2"}));
}

static void buildOnce(const std::string& source) {
    build::DictBuilder b;
    build::DictBuilder::Options opt;
    opt.source = source;
    opt.workDir = "/tdeckjisho/build-test";
    opt.swap = false;
    opt.ramBytes = 256 * 1024;                     // the minimum: still correct
    CHECK(b.start(opt));
    while (b.running()) b.step(30);
    CHECK(b.phase() == build::DictBuilder::Phase::Done);
    CHECK(b.entries() == 52);
    // Byte for byte what tools/jmdict2jisho.py produced for the same input.
    for (const char* f : {"/entries.dat", "/kana.idx", "/kanji.idx", "/eng.idx"})
        CHECK(fileBytes("/tdeckjisho/build-test" + std::string(f)) == fileBytes("/tdeckjisho/jisho" + std::string(f)));
    hal::removeTree("/tdeckjisho/build-test");
}

static void testBuilder() {
    // tests/run.sh writes the gzipped copy; the plain XML comes via a symlink-free
    // copy made here, so both input paths are exercised.
    hal::WFile w;
    const std::string xml = [] { std::string s; FILE* f = std::fopen("tools/sample_jmdict.xml", "rb");
        if (f) { char b[4096]; size_t n; while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n); std::fclose(f); }
        return s; }();
    CHECK(!xml.empty());
    w.open("/tdeckjisho/jmdict/sample_jmdict.xml");
    w.write(xml.data(), xml.size());
    w.close();
    buildOnce("/tdeckjisho/jmdict/sample_jmdict.xml");
    buildOnce("/tdeckjisho/jmdict/sample_jmdict.xml.gz");

    // A cut-off download must be caught by the gzip trailer check.
    std::string gz = fileBytes("/tdeckjisho/jmdict/sample_jmdict.xml.gz");
    w.open("/tdeckjisho/jmdict/broken.gz");
    w.write(gz.data(), gz.size() - 6);
    w.close();
    build::DictBuilder b;
    build::DictBuilder::Options opt;
    opt.source = "/tdeckjisho/jmdict/broken.gz";
    opt.workDir = "/tdeckjisho/build-test";
    opt.swap = false;
    if (b.start(opt)) while (b.running()) b.step(30);
    CHECK(b.phase() == build::DictBuilder::Phase::Failed);
    CHECK(!hal::exists("/tdeckjisho/build-test"));    // a failed build leaves nothing behind
    hal::removeFile("/tdeckjisho/jmdict/broken.gz");
    hal::removeFile("/tdeckjisho/jmdict/sample_jmdict.xml");
}

// ---- pictures -----------------------------------------------------------------
static void testImages() {
    CHECK_EQ(rt::resolveImagePath("/tdeckjisho/books/a.md", "img/x.jpg"), std::string("/tdeckjisho/books/img/x.jpg"));
    CHECK_EQ(rt::resolveImagePath("/tdeckjisho/books/sub/a.md", "../img/b%20c.png"),
             std::string("/tdeckjisho/books/img/b c.png"));
    CHECK_EQ(rt::resolveImagePath("/tdeckjisho/books/a.md", "https://x.org/a.png"), std::string(""));
    // A book may only show pictures from the books folder, and no tricks in the name.
    CHECK_EQ(rt::resolveImagePath("/tdeckjisho/books/a.md", "/tdeckjisho/config/wifi.txt"), std::string(""));
    CHECK_EQ(rt::resolveImagePath("/tdeckjisho/books/a.md", "../config/settings.txt"), std::string(""));
    CHECK_EQ(rt::resolveImagePath("/tdeckjisho/books/a.md", "../../../../etc/x.png"), std::string(""));
    CHECK_EQ(rt::resolveImagePath("/tdeckjisho/books/a.md", "img/x.jpg%00.png"), std::string(""));
    // A feed item without a link is still WEB content: no SD pictures.
    {
        rt::FeedItem item;
        item.title = "t";
        item.summaryHtml = "<img src='/tdeckjisho/books/img/fuji.jpg' alt='x'>";
        rt::Doc feedDoc;
        rt::feedItemToDoc(item, "F", feedDoc);
        CHECK(feedDoc.images.empty());
        CHECK(feedDoc.text.find(rt::IMAGE) == std::string::npos);
    }

    rt::Doc doc;
    rt::parseMarkdown("Text ![Fuji](img/fuji.jpg) more\n\n![](img/fuji-progressive.jpg)\n"
                      "[![fan](img/harisen.png)](https://example.org)\n\n![b](img/sakura.bmp)\n![q](img/onigiri.qoi)\n",
                      doc);
    CHECK(doc.images.size() == 5);
    CHECK(doc.links.empty());                        // the linked picture's link is dropped
    rt::resolveImages(doc, "/tdeckjisho/books/pictures.md");
    using F = rt::ImageFormat;
    CHECK(doc.images[0].format == F::Jpeg && doc.images[0].w == 120 && doc.images[0].h == 75);
    CHECK(doc.images[1].w == 0 && doc.images[1].problem.find("progressive") != std::string::npos);
    CHECK(doc.images[2].format == F::Png && doc.images[2].w == 55);
    CHECK(doc.images[3].format == F::Bmp && doc.images[3].h == 30);
    CHECK(doc.images[4].format == F::Qoi && doc.images[4].w == 40);

    // Layout: a picture is ONE line as tall as the picture (+6), then its caption.
    FakeMetrics m;
    const auto lines = rt::layout(doc, 300, m);
    int pictures = 0;
    for (const auto& l : lines) if (l.image >= 0) ++pictures;
    CHECK(pictures == 4);                             // the progressive one shows only a caption
    for (const auto& l : lines) if (l.image == 0) CHECK(l.height == 75 + 6);

    rt::Doc html;
    rt::parseHtml("<p>a<img src='img/fuji.jpg' alt='F'>b</p>", "", rt::Source::Local, html);
    CHECK(html.images.size() == 1 && html.images[0].alt == "F");
    rt::Doc web;
    rt::parseHtml("<p>a<img src='x.jpg' alt='F'>b</p>", "https://example.org/", rt::Source::Web, web);
    CHECK(web.images.empty());                       // web pages: alt text only
}

// ---- localization ---------------------------------------------------------------
// "{name}" placeholders of a text, in order of appearance, joined by ","
static std::string placeholders(const char* text) {
    std::string out;
    const std::string_view t = text;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] != '{') continue;
        const size_t close = t.find('}', i);
        if (close == std::string_view::npos) break;
        if (!out.empty()) out += ',';
        out += std::string(t.substr(i + 1, close - i - 1));
        i = close;
    }
    return out;
}

static void testI18n() {
    using namespace i18n;
    // 1. The table is complete where it must be, and consistent everywhere.
    int rows = 0, untranslatedJa = 0;
    for (size_t i = 0; i < static_cast<size_t>(Str::Count); ++i, ++rows) {
        const Str id = static_cast<Str>(i);
        const std::string et = raw(id, Lang::Et), en = raw(id, Lang::En), ja = raw(id, Lang::Ja);
        if (et.empty() || en.empty()) { std::printf("  Strings.def row %zu: Estonian or English is empty\n", i); }
        CHECK(!et.empty() && !en.empty());
        // The same placeholders in every language, or a translation would show "{n}" to the user.
        CHECK_EQ(placeholders(et.c_str()), placeholders(en.c_str()));
        if (!ja.empty()) CHECK_EQ(placeholders(ja.c_str()), placeholders(en.c_str()));
        else ++untranslatedJa;
        // The Japanese font draws 0x7E as an overline and 0x5C as a yen sign.
        CHECK(ja.find('~') == std::string::npos && ja.find('\\') == std::string::npos);
    }
    CHECK(rows > 80);
    CHECK(untranslatedJa == 1);                       // only the demo row: keep it that way

    // 2. Switching language, and the fallback to English.
    setLanguage(Lang::Et);
    CHECK_EQ(std::string(tr(Str::NoSdCard)), std::string("SD-kaarti pole"));
    CHECK_EQ(std::string(tr(Str::DemoUntranslated)), std::string("Eestikeelne tekst"));
    setLanguage(Lang::Ja);
    CHECK_EQ(std::string(tr(Str::NoSdCard)), std::string("SDカードがありません"));
    CHECK_EQ(std::string(tr(Str::DemoUntranslated)), std::string("English text, no Japanese yet"));
    setLanguage(Lang::En);
    CHECK_EQ(std::string(tr(Str::NoSdCard)), std::string("No SD card"));

    // 3. Language codes.
    CHECK(fromCode("et", Lang::En) == Lang::Et && fromCode("ja", Lang::En) == Lang::Ja);
    CHECK(fromCode("xx", Lang::Et) == Lang::Et);       // unknown: the fallback
    CHECK_EQ(std::string(code(Lang::Ja)), std::string("ja"));
    CHECK_EQ(std::string(nativeName(Lang::Et)), std::string("Eesti"));

    // 4. Placeholders.
    setLanguage(Lang::En);
    CHECK_EQ(fmt(Str::WifiForgot, {{"ssid", "Kodu"}}), std::string("Forgot Kodu"));
    setLanguage(Lang::Et);
    CHECK_EQ(fmt(Str::WifiForgot, {{"ssid", "Kodu"}}), std::string("Unustatud: Kodu"));
    CHECK_EQ(fmt(Str::UpdTimeLeft, {{"wrong", "1:00"}}), std::string("umbes {t} jäänud"));   // visible bug

    // 5. Plurals: Estonian "1 kirje" / "5 kirjet", English "1 entry" / "5 entries", Japanese "件".
    setLanguage(Lang::Et);
    CHECK_EQ(trn(Str::EntriesOne, Str::EntriesMany, 1), std::string("1 kirje"));
    CHECK_EQ(trn(Str::EntriesOne, Str::EntriesMany, 5), std::string("5 kirjet"));
    CHECK_EQ(trn(Str::EntriesOne, Str::EntriesMany, 0), std::string("0 kirjet"));
    setLanguage(Lang::En);
    CHECK_EQ(trn(Str::EntriesOne, Str::EntriesMany, 1), std::string("1 entry"));
    CHECK_EQ(trn(Str::EntriesOne, Str::EntriesMany, 215018), std::string("215018 entries"));
    setLanguage(Lang::Ja);
    CHECK_EQ(trn(Str::EntriesOne, Str::EntriesMany, 1), std::string("1 件"));
    CHECK_EQ(trn(Str::EntriesOne, Str::EntriesMany, 7), std::string("7 件"));
    setLanguage(Lang::En);                             // leave the global as the other tests expect it
}

// ---- the text cursor and de-inflection ---------------------------------
static bool has(const std::vector<std::string>& v, const char* s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

static void testCursor() {
    const std::string s = "a\xC3\xB5\xE6\x97\xA5";           // a, õ (2 bytes), 日 (3 bytes) = 6 bytes
    CHECK(utf8::prev(s, 6) == 3 && utf8::prev(s, 3) == 1 && utf8::prev(s, 1) == 0 && utf8::prev(s, 0) == 0);

    FakeMetrics m;                                          // Latin 8 px, CJK 16 px
    rt::Doc d;
    rt::parseMarkdown("ab日本{語|ご}c\n\nxyz", d);          // the furigana group 語/ご is ONE unit
    const auto lines = rt::layout(d, 400, m);
    const auto units = rt::lineUnits(d, lines[0], m);
    CHECK(units.size() == 6);                               // a b 日 本 [語] c
    CHECK(units[2].x == 16 && units[2].w == 16);            // 日
    CHECK(units[4].x == 48 && units[4].w == 16);            // the group is as wide as its base
    CHECK(units[5].x == 64 && units[5].w == 8);             // c
    CHECK(rt::unitAtX(units, 50) == 4 && rt::unitAtX(units, 1000) == 5 && rt::unitAtX(units, -3) == 0);
    // A tap: only what is really under it (3 px slack). After the end of the line: nothing.
    CHECK(rt::unitUnderX(units, 50) == 4 && rt::unitUnderX(units, 73) == 5);   // c ends at 72
    CHECK(rt::unitUnderX(units, 76) == -1 && rt::unitUnderX(units, 1000) == -1 && rt::unitUnderX(units, -10) == -1);

    // The text a look-up gets: no control codes, no furigana readings, ends at the paragraph end.
    CHECK_EQ(rt::plainTextFrom(d, units[2].start, 10), std::string("日本語c"));
    CHECK_EQ(rt::plainTextFrom(d, units[0].start, 2), std::string("ab"));
    size_t end = 0;
    rt::plainTextFrom(d, units[2].start, 3, &end);          // 日 本 語: ends after the whole group
    CHECK(end == units[5].start);

    // The text BEFORE the cursor, for the look-up's context: a furigana group counts whole,
    // and the paragraph start ends it.
    CHECK_EQ(rt::plainTextBefore(d, units[5].start, 2), std::string("本語"));
    CHECK_EQ(rt::plainTextBefore(d, units[2].start, 5), std::string("ab"));
    CHECK_EQ(rt::plainTextBefore(d, units[0].start, 2), std::string(""));
    CHECK_EQ(rt::plainTextBefore(d, d.text.find('x'), 2), std::string(""));

    // Moving: one unit at a time, across the paragraph end, both ways.
    const size_t x0 = d.text.find('x');
    CHECK(rt::nextOffset(d, units[3].start) == units[4].start);
    CHECK(rt::nextOffset(d, units[4].start) == units[5].start);
    CHECK(rt::nextOffset(d, units[5].start) == x0);
    CHECK(rt::prevOffset(d, x0) == units[5].start);
    CHECK(rt::prevOffset(d, units[4].start) == units[3].start);
    CHECK(rt::prevOffset(d, units[0].start) == rt::kNoOffset);
    CHECK(rt::nextOffset(d, d.text.size() - 1) == rt::kNoOffset);

    // Inside a Latin word the look-up starts at the word's first letter; Japanese text is unaffected.
    rt::Doc e;
    rt::parseMarkdown("see practice now 日本", e);
    const size_t a = e.text.find("actice");
    CHECK(rt::latinWordStart(e, a) == e.text.find("practice"));
    CHECK(rt::latinWordStart(e, e.text.find("日")) == e.text.find("日"));
    CHECK(rt::latinWordStart(e, e.text.find("practice")) == e.text.find("practice"));
}

static void testDeinflect() {
    using deinflect::candidates;
    CHECK(has(candidates("食べました"), "食べる"));         // ichidan polite past
    CHECK(has(candidates("食べない"), "食べる"));
    CHECK(has(candidates("食べている"), "食べる"));         // ている -> て -> る (two steps)
    CHECK(has(candidates("書きます"), "書く"));             // godan
    CHECK(has(candidates("書いた"), "書く"));
    CHECK(has(candidates("読んだ"), "読む"));
    CHECK(has(candidates("行かなかった"), "行く"));
    CHECK(has(candidates("話して"), "話す"));
    CHECK(has(candidates("高かった"), "高い"));             // adjective
    CHECK(has(candidates("勉強しました"), "勉強する"));     // noun + する
    CHECK(has(candidates("来ました"), "来る"));
    CHECK(candidates("あ").empty());                        // one character: nothing to strip
    CHECK(!has(candidates("食べる"), "食べる"));            // never returns the word itself
}

static void testLookup() {
    Dictionary dict;
    if (!dict.open("/tdeckjisho/jisho") || !dict.hasKanjiIndex()) {
        std::printf("SKIP lookup tests: no dictionary with kanji.idx (run the converter)\n");
        return;
    }
    // Longest match: 日本語 (3 characters) comes before 日本 (2), and 日本 is still offered.
    auto h = dict.lookupAt("日本語を勉強する", 4);
    CHECK(!h.empty() && h[0].hit.headword == "日本語" && h[0].chars == 3);
    CHECK(std::any_of(h.begin(), h.end(), [](const LookupHit& l) { return l.hit.headword == "日本" && l.chars == 2; }));

    // De-inflection: the text says 食べました, the dictionary has 食べる.
    h = dict.lookupAt("食べました。", 4);                   // the 。 ends the word
    CHECK(!h.empty() && h[0].hit.headword == "食べる" && h[0].chars == 5);
    CHECK(!h.empty() && h[0].matched == "食べました" && h[0].form == "食べる");
    h = dict.lookupAt("読んだ本", 4);
    CHECK(!h.empty() && h[0].hit.headword == "読む" && h[0].chars == 3);

    // Katakana is found through the (hiragana) kana index: コーヒー -> 珈琲.
    h = dict.lookupAt("コーヒーを飲む", 4);
    CHECK(!h.empty() && h[0].hit.headword == "珈琲" && h[0].chars == 4);

    // Half kanji, half katakana, not listed in JMdict: 子グマ is 子熊 (こぐま) = 子 (こ) + ぐま.
    // The row shows the kanji form that fits (子熊), not the entry's first form (小熊).
    h = dict.lookupAt("子グマが", 4);
    CHECK(!h.empty() && h[0].chars == 3 && h[0].matched == "子グマ" && h[0].form == "子熊");
    CHECK(!h.empty() && h[0].hit.headword == "子熊" && h[0].hit.reading == "こぐま");
    CHECK(std::any_of(h.begin(), h.end(), [](const LookupHit& l) { return l.hit.headword == "子" && l.chars == 1; }));

    // Katakana in news is mostly species: シカ is 鹿 (JMdict lists the spelling シカ) before
    // 歯科 (only しか), although 歯科 scores higher.
    h = dict.lookupAt("シカが", 4);
    CHECK(h.size() >= 2 && h[0].hit.headword == "鹿" && h[1].hit.headword == "歯科");
    // In hiragana nothing changes: the score decides.
    h = dict.lookupAt("しか", 4);
    CHECK(!h.empty() && h[0].hit.headword == "歯科");
    // A compound JMdict doesn't have: 親グマ -> the species, with the rendaku undone (グマ -> クマ).
    h = dict.lookupAt("親グマ", 4);
    CHECK(!h.empty() && h[0].chars == 3 && h[0].form == "熊" && h[0].hit.headword == "熊");
    CHECK(std::any_of(h.begin(), h.end(), [](const LookupHit& l) { return l.hit.headword == "親" && l.chars == 1; }));
    // The cursor on グ of 子グマ: the text starts with グマ, which is no word, but クマ is.
    h = dict.lookupAt("グマが", 4);
    CHECK(!h.empty() && h[0].chars == 2 && h[0].matched == "グマ" && h[0].hit.headword == "熊");
    // ジカ IS a word (時価), but after a kanji (雄ジカ) the un-voiced シカ (鹿) comes first.
    h = dict.lookupAt("ジカ", 4, "雄");
    CHECK(h.size() >= 2 && h[0].hit.headword == "鹿" && h[1].hit.headword == "時価");
    h = dict.lookupAt("ジカ", 4);
    CHECK(!h.empty() && h[0].hit.headword == "時価");

    // After a number, counters first, showing the counter sense: 3人 is にん, not ひと.
    h = dict.lookupAt("人が", 4, "3");
    CHECK(h.size() >= 2 && h[0].hit.reading == "にん" && h[1].hit.reading == "ひと");
    h = dict.lookupAt("人が", 4);
    CHECK(!h.empty() && h[0].hit.reading == "ひと");
    // ３本の: 本 (counter) before the longer 本の, which is the counter + の here.
    h = dict.lookupAt("本の", 4, "３");
    CHECK(h.size() >= 2 && h[0].hit.headword == "本" && h[0].hit.gloss == "counter for long, thin objects");
    CHECK(h.size() >= 2 && h[1].hit.headword == "本の");
    h = dict.lookupAt("本の", 4, "五");
    CHECK(!h.empty() && h[0].hit.headword == "本");
    h = dict.lookupAt("本の", 4);                           // no number: the longest match, as always
    CHECK(h.size() >= 2 && h[0].hit.headword == "本の" && h[1].hit.gloss == "book; volume; script");

    // Latin text: the English word under the cursor.
    h = dict.lookupAt("eat it", 4);
    CHECK(!h.empty() && h[0].hit.headword == "食べる" && h[0].matched == "eat");

    CHECK(dict.lookupAt("。。", 4).empty());                // punctuation: nothing to look up
    CHECK(dict.lookupAt("zzzz", 4).empty());
}

int main() {
    testUtf8();
    testRomaji();
    testUrl();
    testMarkdown();
    testAozora();
    testHtml();
    testHostileText();
    testFeed();
    testLayout();
    testSettingsRoundTrip();
    testNetPolicy();
    testRubyLayout();
    testDictionary();
    testQuiz();
    testEncodeRules();
    testHostileJmdict();
    testBuilder();
    testImages();
    testI18n();
    testCursor();
    testDeinflect();
    testLookup();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
