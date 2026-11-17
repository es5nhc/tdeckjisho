// ============================================================================
//  Html.cpp – HTML to readable text ("reader mode" for a 320x240 screen)
// ============================================================================
//
//  This is NOT a browser engine. There is no DOM tree, no CSS, no JavaScript.
//  It is a single left-to-right scan over the bytes that:
//
//    * drops everything inside <script>, <style>, <head>, <nav>, <svg>, ...
//    * turns block tags (<p>, <div>, <li>, <h1>...) into paragraphs
//    * turns <a href> into links, <b>/<strong> into bold, <ruby><rt> into furigana
//    * decodes entities (&amp; &#26085; &otilde; ...)
//    * collapses whitespace like a browser does
//
//  "Readability lite": if the page has an <article> or <main> element we show
//  only that, which removes most menus/footers on news sites.
//
//  Python comparison: you'd reach for BeautifulSoup. Here we have ~300 KB of
//  RAM to play with and want the job done in one pass with no allocations
//  per tag, so we hand-write a tiny tokenizer.
// ============================================================================

#include <algorithm>         // std::min
#include <cctype>            // std::isalnum, std::tolower
#include <initializer_list>  // isOneOf({...})

#include "core/Str.hpp"
#include "core/Url.hpp"
#include "core/Utf8.hpp"
#include "text/Parsers.hpp"

namespace rt {

// ---------------------------------------------------------------------------
//  Entities
// ---------------------------------------------------------------------------
namespace {

struct Entity {
    const char* name;
    uint32_t cp;
};
// The common ones, plus the Estonian letters for local news sites.
constexpr Entity kEntities[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''},
    {"nbsp", ' '}, {"ensp", ' '}, {"emsp", ' '}, {"thinsp", ' '}, {"shy", 0},
    {"ndash", 0x2013}, {"mdash", 0x2014}, {"hellip", 0x2026}, {"bull", 0x2022},
    {"middot", 0xB7}, {"laquo", 0xAB}, {"raquo", 0xBB}, {"lsquo", 0x2018},
    {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"bdquo", 0x201E},
    {"copy", 0xA9}, {"reg", 0xAE}, {"trade", 0x2122}, {"deg", 0xB0}, {"times", 0xD7},
    {"euro", 0x20AC}, {"yen", 0xA5}, {"pound", 0xA3},
    {"auml", 0xE4}, {"Auml", 0xC4}, {"ouml", 0xF6}, {"Ouml", 0xD6},
    {"uuml", 0xFC}, {"Uuml", 0xDC}, {"otilde", 0xF5}, {"Otilde", 0xD5},
    {"scaron", 0x161}, {"Scaron", 0x160}, {"zcaron", 0x17E}, {"Zcaron", 0x17D},
    {"eacute", 0xE9}, {"aacute", 0xE1}, {"oacute", 0xF3}, {"szlig", 0xDF},
};

}  // namespace

std::string decodeEntities(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { out += s[i]; continue; }
        // SECURITY: look for the ';' only in a short window. s.find(';', i) would scan to
        // the END of the text for every '&' – a page of 100 000 "&" with no ';' then
        // costs 100 000 x 100 000 steps (minutes on the ESP32). An entity name is at
        // most 10 characters, so 12 bytes ('&' + name + ';') is all we ever need.
        const std::string_view window = s.substr(i, 12);
        const size_t semi = window.find(';');
        if (semi == std::string_view::npos) { out += '&'; continue; }
        const std::string_view name = window.substr(1, semi - 1);
        uint32_t cp = 0xFFFFFFFF;                               // "not found"
        if (!name.empty() && name[0] == '#') {
            // &#26085; (decimal) or &#x65E5; (hex)
            const bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
            cp = static_cast<uint32_t>(str::toLong(name.substr(hex ? 2 : 1), 0xFFFFFFFF, hex ? 16 : 10));
        } else {
            for (const Entity& e : kEntities)
                if (name == e.name) { cp = e.cp; break; }
        }
        if (cp == 0xFFFFFFFF) { out += '&'; continue; }        // unknown: keep as-is
        // &#x110000; or &#xD800; are not characters: append() writes U+FFFD for them.
        if (cp != 0) utf8::append(out, cp);                     // &shy; -> nothing
        i += semi;
    }
    return out;
}

// Where does the tag that starts at s[lt] ('<') end? The '>' that closes it – but NOT
// a '>' inside a quoted attribute value. postimees.ee writes
//     <span onkeydown="event => event.key === 'Enter' ? ... : return false" ...>
// and the first '>' is the one in "=>": the rest of the tag would come out as text.
//
// A quote only opens a value right after '=' (spaces allowed), like in a browser:
// <p title=it's> is an unquoted value, not the start of a string.
//
// Broken markup (<a href="x>, the quote never closed) must not swallow the page: if
// the closing quote is missing, fall back to the plain first '>'.
// SECURITY: still linear time. A found closing quote is inside the tag, which we then
// step over. A MISSING one costs a scan to the end of the page – but it also proves
// there is no such quote anywhere after it, so no later tag can miss again: at most
// one such scan per quote type ('"' and '\'') per page.
size_t tagEnd(std::string_view s, size_t lt) {
    bool afterEquals = false;
    for (size_t k = lt + 1; k < s.size(); ++k) {
        const char c = s[k];
        if (c == '>') return k;
        if (c == '=') { afterEquals = true; continue; }
        if (afterEquals && (c == '"' || c == '\'')) {
            const size_t close = s.find(c, k + 1);
            if (close == std::string_view::npos) return s.find('>', lt);  // broken tag
            k = close;                                                    // skip the value
        }
        if (!str::isSpace(c)) afterEquals = false;
    }
    return std::string_view::npos;
}

// ---------------------------------------------------------------------------
//  The tag scanner
// ---------------------------------------------------------------------------
namespace {

// Tag name, lower-cased, without '<' '/' and attributes: "<A HREF=..>" -> "a"
std::string tagName(std::string_view tag) {
    size_t i = 1;
    if (i < tag.size() && tag[i] == '/') ++i;
    std::string name;
    while (i < tag.size() && (std::isalnum(static_cast<unsigned char>(tag[i])))) {
        name += static_cast<char>(std::tolower(static_cast<unsigned char>(tag[i])));
        ++i;
    }
    return name;
}

// Value of attribute `attr` inside a tag: <a class="x" href='/y'> -> "/y"
std::string attribute(std::string_view tag, const char* attr) {
    const std::string lower = str::toLower(tag);
    const std::string key = std::string(attr) + "=";
    size_t p = 0;
    while ((p = lower.find(key, p)) != std::string::npos) {
        // Must be a whole attribute name: preceded by whitespace.
        if (p > 0 && !str::isSpace(lower[p - 1])) { p += key.size(); continue; }
        size_t v = p + key.size();
        if (v >= tag.size()) return "";
        const char q = tag[v];
        if (q == '"' || q == '\'') {
            const size_t e = tag.find(q, v + 1);
            return decodeEntities(tag.substr(v + 1, e == std::string_view::npos ? e : e - v - 1));
        }
        const size_t e = tag.find_first_of(" \t\r\n>", v);
        return decodeEntities(tag.substr(v, e == std::string_view::npos ? e : e - v));
    }
    return "";
}

bool isOneOf(const std::string& name, std::initializer_list<const char*> list) {
    for (const char* s : list) if (name == s) return true;
    return false;
}

// Does s[at...] spell `lowerNeedle`, ignoring ASCII case?
bool equalsNoCaseAt(std::string_view s, size_t at, std::string_view lowerNeedle) {
    if (at > s.size() || s.size() - at < lowerNeedle.size()) return false;
    for (size_t k = 0; k < lowerNeedle.size(); ++k) {
        char c = s[at + k];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != lowerNeedle[k]) return false;
    }
    return true;
}

// First / last tag spelled `lowerNeedle` (which starts with '<'), any case. No copy of
// the page: a lower-cased copy of a 3 MB page would cost another 3 MB of PSRAM.
size_t findTagNoCase(std::string_view s, std::string_view lowerNeedle) {
    for (size_t p = s.find('<'); p != std::string_view::npos; p = s.find('<', p + 1))
        if (equalsNoCaseAt(s, p, lowerNeedle)) return p;
    return std::string_view::npos;
}
size_t rfindTagNoCase(std::string_view s, std::string_view lowerNeedle) {
    for (size_t p = s.rfind('<'); p != std::string_view::npos; p = p ? s.rfind('<', p - 1) : std::string_view::npos)
        if (equalsNoCaseAt(s, p, lowerNeedle)) return p;
    return std::string_view::npos;
}

// Return the part of the page we actually want to read.
std::string_view mainContent(std::string_view html) {
    for (const char* tag : {"article", "main"}) {
        const std::string open = std::string("<") + tag;
        const std::string close = std::string("</") + tag + ">";
        // "<main" must not match "<mainmenu": the next char must end the name.
        size_t a = std::string_view::npos;
        for (size_t from = 0; from < html.size();) {
            const size_t p = findTagNoCase(html.substr(from), open);
            if (p == std::string_view::npos) break;
            const size_t at = from + p, after = at + open.size();
            if (after < html.size() && (html[after] == '>' || str::isSpace(html[after]))) { a = at; break; }
            from = at + 1;
        }
        const size_t b = rfindTagNoCase(html, close);
        if (a != std::string_view::npos && b != std::string_view::npos && b > a)
            return html.substr(a, b - a);
    }
    return html;
}

}  // namespace

void parseHtml(std::string_view html, const std::string& baseUrl, Source source, Doc& out) {
    out.baseUrl = baseUrl;

    // <title> first – it is in <head>, which the main pass skips.
    {
        const std::string lower = str::toLower(html.substr(0, std::min<size_t>(html.size(), 20000)));
        const size_t a = lower.find("<title");
        const size_t gt = a == std::string::npos ? a : lower.find('>', a);
        const size_t b = gt == std::string::npos ? gt : lower.find("</title>", gt);
        if (b != std::string::npos)
            out.title = std::string(str::trim(decodeEntities(html.substr(gt + 1, b - gt - 1))));
    }

    const std::string_view body = mainContent(html);
    Builder bld(out);
    bool linkOpen = false;          // inside <a href=...> (see "inside a link" below)
    bool pre = false;
    int skipDepth = 0;            // > 0 while inside <script>, <style>...
    std::string skipTag;          // which tag we are skipping
    bool inRt = false;            // inside <rt> (ruby text)
    bool inRuby = false;          // inside <ruby>
    // <picture> shows ONE of the images inside it; the browser picks with CSS/media
    // queries we don't have. postimees.ee puts a light- and a dark-theme <img> in
    // every <picture>: without this, each caption would appear twice.
    bool inPicture = false;
    bool pictureHasImg = false;   // the first <img> of this <picture> is done

    size_t i = 0;
    while (i < body.size()) {
        if (body[i] != '<') {
            // ---- text up to the next tag ---------------------------------
            const size_t lt = body.find('<', i);
            const std::string_view raw = body.substr(i, lt == std::string_view::npos ? lt : lt - i);
            if (skipDepth == 0) bld.text(decodeEntities(raw), pre);
            if (lt == std::string_view::npos) break;
            i = lt;
            continue;
        }
        // ---- comments and <!DOCTYPE> -------------------------------------
        if (body.compare(i, 4, "<!--") == 0) {
            const size_t e = body.find("-->", i + 4);
            i = (e == std::string_view::npos) ? body.size() : e + 3;
            continue;
        }
        const size_t gt = tagEnd(body, i);
        if (gt == std::string_view::npos) break;
        const std::string_view tag = body.substr(i, gt - i + 1);
        i = gt + 1;
        if (tag.size() < 3 || tag[1] == '!' || tag[1] == '?') continue;

        const bool closing = tag[1] == '/';
        const std::string name = tagName(tag);

        // ---- skipped elements: count nesting until the matching close ----
        if (skipDepth > 0) {
            if (name == skipTag) skipDepth += closing ? -1 : 1;
            continue;
        }
        if (!closing && isOneOf(name, {"script", "style", "head", "nav", "svg", "noscript",
                                       "template", "iframe", "form", "button", "select", "footer"})) {
            skipTag = name;
            skipDepth = 1;
            if (tag[tag.size() - 2] == '/') skipDepth = 0;  // self-closing <svg/>
            continue;
        }

        // ---- inside a link, blocks don't break it -------------------------
        // News sites wrap a whole teaser in one <a>: a picture, then the headline in an
        // <h2>. A new paragraph would end the link (links never span paragraphs), and the
        // headline would be left outside it. Inside a link, a block tag is just a space.
        if (linkOpen && isOneOf(name, {"h1", "h2", "h3", "h4", "h5", "h6", "p", "div", "li", "br",
                                        "section", "article", "header", "figure", "figcaption"})) {
            bld.text(" ");
            continue;
        }

        // ---- formatting -------------------------------------------------
        if (name == "h1" || name == "h2") {
            closing ? bld.paragraph() : bld.paragraph(name == "h1" ? H1 : H2);
        } else if (isOneOf(name, {"h3", "h4", "h5", "h6"})) {
            closing ? bld.paragraph() : bld.paragraph(H2);
        } else if (name == "li" || name == "dt") {
            if (!closing) bld.paragraph(BULLET);
        } else if (name == "blockquote") {
            bld.paragraph(closing ? 0 : QUOTE);
        } else if (name == "pre") {
            pre = !closing;
            bld.paragraph(closing ? 0 : PRE);
        } else if (name == "br") {
            bld.paragraph(pre ? PRE : 0);
        } else if (name == "hr") {
            bld.rule();
        } else if (isOneOf(name, {"p", "div", "section", "article", "main", "header", "ul", "ol",
                                  "dl", "dd", "table", "tr", "figure", "figcaption", "aside", "time"})) {
            bld.paragraph();
        } else if (name == "td" || name == "th") {
            if (!closing) bld.text(" | ");
        } else if (name == "b" || name == "strong") {
            bld.code(BOLD);
        } else if (name == "a") {
            if (closing) {
                bld.endLink();
                linkOpen = false;
            } else {
                const std::string href = attribute(tag, "href");
                if (!href.empty() && href[0] != '#' && !str::startsWith(href, "javascript:")) {
                    bld.beginLink(url::resolve(baseUrl, href));
                    // aria-label: the page says in words what the link is (screen readers
                    // read exactly this). Use it, and skip the markup inside: pictures,
                    // icons, badges like "LIVE" that would end up in the link's text.
                    const std::string label = attribute(tag, "aria-label");
                    if (!label.empty()) {
                        bld.text(label);
                        bld.endLink();
                        skipTag = "a";            // until the matching </a>
                        skipDepth = 1;
                        continue;
                    }
                    linkOpen = true;
                }
            }
        } else if (name == "ruby") {
            // <ruby>漢字<rt>かんじ</rt></ruby>: the base starts here.
            inRuby = !closing;
            if (!closing) bld.code(BASE);
        } else if (name == "rt") {
            // rt content becomes furigana above the base.
            if (closing == inRt) { bld.code(RUBY); inRt = !closing; }
            // <ruby>漢<rt>かん</rt>字<rt>じ</rt></ruby> has several bases in
            // one element: after each </rt> a new base may begin. (If none
            // follows, the lone BASE is ignored by the layout.)
            if (closing && inRuby) bld.code(BASE);
        } else if (name == "rp") {
            // <rp>(</rp> is the fallback parenthesis for old browsers – skip it.
            if (!closing) { skipTag = "rp"; skipDepth = 1; }
        } else if (name == "picture") {
            inPicture = !closing;
            pictureHasImg = false;
        } else if (name == "img") {
            if (inPicture && pictureHasImg) continue;   // another variant of the same picture
            pictureHasImg = inPicture;
            const std::string alt = attribute(tag, "alt");
            if (source == Source::Local) {
                // A local file from the SD card: show the picture.
                bld.image(attribute(tag, "src"), alt);
            } else if (!alt.empty()) {
                // A web page: pictures would need extra downloads (and RAM),
                // so only the alt text is shown. (Exercise: fetch small ones
                // with hal::Fetch and decode them from memory.)
                bld.code(DIM); bld.text("[" + alt + "]"); bld.code(DIM);
            }
        }
    }
    bld.paragraph();
}

}  // namespace rt
