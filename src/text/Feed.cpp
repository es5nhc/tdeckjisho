#include "text/Feed.hpp"

#include <algorithm>
#include <cctype>

#include "core/Str.hpp"
#include "text/Parsers.hpp"

namespace rt {

namespace {

// Text content of the first <name ...>...</name> inside `block`.
// Handles <![CDATA[ ... ]]> (raw text, no entity decoding needed) and the
// self-closing Atom form <link href="..."/> (returns the href).
std::string child(std::string_view block, const std::string& name) {
    const std::string open = "<" + name;
    size_t a = 0;
    while (true) {
        a = block.find(open, a);
        if (a == std::string_view::npos) return "";
        const char nx = a + open.size() < block.size() ? block[a + open.size()] : '>';
        if (nx == '>' || nx == ' ' || nx == '/' || nx == '\t' || nx == '\n') break;
        a += open.size();   // "<linkfoo" is not "<link"
    }
    const size_t gt = tagEnd(block, a);   // not a '>' inside a quoted attribute
    if (gt == std::string_view::npos) return "";
    const std::string_view tag = block.substr(a, gt - a + 1);

    // Atom: <link rel="alternate" href="https://..."/>
    if (block[gt - 1] == '/') {
        const size_t h = tag.find("href=");
        if (h == std::string_view::npos) return "";
        const char q = tag[h + 5];
        const size_t e = tag.find(q, h + 6);
        return decodeEntities(tag.substr(h + 6, e - h - 6));
    }

    const size_t close = block.find("</" + name, gt);
    std::string_view inner = block.substr(gt + 1, close == std::string_view::npos ? 0 : close - gt - 1);
    inner = str::trim(inner);
    if (str::startsWith(inner, "<![CDATA[")) {
        inner.remove_prefix(9);
        const size_t e = inner.rfind("]]>");
        return std::string(inner.substr(0, e));
    }
    return decodeEntities(inner);
}

// Pull every <tag ...>...</tag> block out of the document.
std::vector<std::string_view> blocks(std::string_view xml, const std::string& tag) {
    std::vector<std::string_view> out;
    const std::string open = "<" + tag;
    const std::string close = "</" + tag + ">";
    size_t p = 0;
    while ((p = xml.find(open, p)) != std::string_view::npos) {
        // A download cut off at maxBytes may end exactly with "<item": there is no next
        // character to look at. (std::string would hand us its hidden '\0' there, but a
        // string_view promises nothing: reading xml[size()] is undefined behaviour.)
        if (p + open.size() >= xml.size()) break;
        const char nx = xml[p + open.size()];
        if (nx != '>' && nx != ' ' && nx != '\n' && nx != '\t') { p += open.size(); continue; }
        const size_t e = xml.find(close, p);
        if (e == std::string_view::npos) break;
        out.push_back(xml.substr(p, e - p));
        p = e + close.size();
    }
    return out;
}

// Days from 1970-01-01 to a date: Howard Hinnant's "days_from_civil". Plain integer
// arithmetic, so it works without the C library's time zone (timegm is not portable).
int64_t daysFromCivil(int64_t y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;                                      // [0, 399]
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;    // [0, 365]
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;              // [0, 146096]
    return era * 146097 + doe - 719468;
}

// Read a number of up to `maxDigits` digits at s[i]; -1 if there is none.
int number(std::string_view s, size_t& i, int maxDigits = 4) {
    int v = 0, n = 0;
    while (i < s.size() && n < maxDigits && std::isdigit(static_cast<unsigned char>(s[i]))) {
        v = v * 10 + (s[i++] - '0');
        ++n;
    }
    return n ? v : -1;
}

// "+0900", "+09:00", "-05:00", "Z", "GMT", "UT" -> seconds east of UTC.
int64_t zoneOffset(std::string_view s) {
    s = str::trim(s);
    if (s.empty() || s[0] == 'Z' || str::startsWith(s, "GMT") || str::startsWith(s, "UT")) return 0;
    if (s[0] != '+' && s[0] != '-') return 0;      // named zones (EST...): rare, treated as UTC
    size_t i = 1;
    const int h = number(s, i, 2);
    if (i < s.size() && s[i] == ':') ++i;
    const int m = number(s, i, 2);
    const int64_t off = (h < 0 ? 0 : h) * 3600 + (m < 0 ? 0 : m) * 60;
    return s[0] == '-' ? -off : off;
}

}  // namespace

int64_t feedTime(std::string_view date) {
    date = str::trim(date);
    int y, mo, d, h = 0, mi = 0, sec = 0;
    size_t i = 0;
    if (date.size() >= 10 && date[4] == '-') {                 // ISO 8601: 2026-11-15T06:21:00+09:00
        y = number(date, i); ++i;
        mo = number(date, i, 2); ++i;
        d = number(date, i, 2);
        if (i < date.size() && (date[i] == 'T' || date[i] == ' ')) {
            ++i; h = number(date, i, 2); ++i; mi = number(date, i, 2);
            if (i < date.size() && date[i] == ':') { ++i; sec = number(date, i, 2); }
            while (i < date.size() && (date[i] == '.' || std::isdigit(static_cast<unsigned char>(date[i])))) ++i;
        }
    } else {                                                   // RFC 822: Sun, 15 Nov 2026 06:21:00 +0900
        if (const size_t comma = date.find(','); comma != std::string_view::npos) i = comma + 1;
        while (i < date.size() && date[i] == ' ') ++i;
        d = number(date, i, 2);
        while (i < date.size() && date[i] == ' ') ++i;
        static const char* kMonths[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                        "jul", "aug", "sep", "oct", "nov", "dec"};
        mo = -1;
        const std::string mon = str::toLower(date.substr(i, 3));
        for (int k = 0; k < 12; ++k) if (mon == kMonths[k]) mo = k + 1;
        i += 3;
        while (i < date.size() && date[i] == ' ') ++i;
        y = number(date, i);
        if (y >= 0 && y < 100) y += 2000;                      // two-digit years, old feeds
        while (i < date.size() && date[i] == ' ') ++i;
        h = number(date, i, 2);
        if (i < date.size() && date[i] == ':') { ++i; mi = number(date, i, 2); }
        if (i < date.size() && date[i] == ':') { ++i; sec = number(date, i, 2); }
    }
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || mi < 0 || sec < 0) return 0;
    const int64_t local = daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + sec;
    return local - zoneOffset(i < date.size() ? date.substr(i) : std::string_view());
}

bool parseFeed(std::string_view xml, Feed& out) {
    out = Feed{};
    const bool atom = xml.find("<feed") != std::string_view::npos &&
                      xml.find("<rss") == std::string_view::npos;
    const auto items = blocks(xml, atom ? "entry" : "item");

    // The feed title is the first <title> before the first item.
    const size_t firstItem = items.empty() ? xml.size() : static_cast<size_t>(items[0].data() - xml.data());
    out.title = child(xml.substr(0, firstItem), "title");

    for (std::string_view it : items) {
        FeedItem fi;
        fi.title = child(it, "title");
        fi.link = child(it, "link");
        fi.date = child(it, atom ? "updated" : "pubDate");
        if (fi.date.empty()) fi.date = child(it, "dc:date");
        fi.time = feedTime(fi.date);
        fi.summaryHtml = child(it, atom ? "summary" : "description");
        if (fi.summaryHtml.empty()) fi.summaryHtml = child(it, atom ? "content" : "content:encoded");
        out.items.push_back(std::move(fi));
    }
    // Newest first. stable_sort keeps the feed's own order for equal times, so undated
    // items (time 0) stay together at the end, in the order the feed gave them.
    std::stable_sort(out.items.begin(), out.items.end(),
                     [](const FeedItem& a, const FeedItem& b) { return a.time > b.time; });
    return !out.items.empty();
}

void feedItemToDoc(const FeedItem& item, const std::string& feedTitle, Doc& out) {
    out = Doc{};
    out.title = feedTitle;
    out.baseUrl = item.link;
    Builder b(out);
    b.paragraph(H1);
    b.text(item.title);
    b.paragraph(SMALL);
    b.text(feedTitle + (item.date.empty() ? "" : "  ·  " + item.date));
    b.rule();

    // The summary is HTML (or plain text, which is also valid "HTML").
    Doc summary;
    parseHtml(item.summaryHtml, item.link, Source::Web, summary);   // even without a link
    // Append the summary's text and links to our document. Its LINK codes
    // index into its own `links`, which we append after ours – so the order
    // (and therefore the n-th LINK -> links[n] rule) is preserved.
    b.paragraph();
    out.text += summary.text;
    for (auto& l : summary.links) out.links.push_back(l);

    if (!item.link.empty()) {
        b.blankLine();
        b.paragraph();
        b.beginLink(item.link);
        b.text("→ Open full article");
        b.endLink();
    }
    b.paragraph();
}

}  // namespace rt
