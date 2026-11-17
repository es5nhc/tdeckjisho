// Audit probes: each one exercises a suspected weakness with hostile input.
#include <chrono>
#include <cstdio>
#include <string>
#include "core/Utf8.hpp"
#include "text/Feed.hpp"
#include "text/Parsers.hpp"
using clk = std::chrono::steady_clock;
static double ms(clk::time_point a) { return std::chrono::duration<double, std::milli>(clk::now() - a).count(); }

int main() {
    // 1. Overlong / out-of-range UTF-8 accepted as valid?
    printf("isValid(C0 81 overlong of U+0001) = %d\n", utf8::isValid("\xC0\x81"));
    printf("isValid(ED A0 80 surrogate)       = %d\n", utf8::isValid("\xED\xA0\x80"));
    printf("isValid(F4 90 80 80 > U+10FFFF)   = %d\n", utf8::isValid("\xF4\x90\x80\x80"));

    // 2. Entity decoding of out-of-range code points
    std::string e = rt::decodeEntities("&#x110000;&#xD800;&#-1;");
    printf("decodeEntities(huge/surrogate/neg) valid UTF-8 = %d (%zu bytes)\n", utf8::isValid(e), e.size());

    // 3. Quadratic: many '&' with no ';' (the HTML text path)
    for (size_t n : {20000u, 40000u, 80000u}) {
        std::string html = "<p>" + std::string(n, '&') + "</p>";
        rt::Doc d; auto t = clk::now(); rt::parseHtml(html, "https://x/", rt::Source::Web, d);
        printf("parseHtml %6zu x '&' : %8.1f ms\n", n, ms(t));
    }
    // 4. Quadratic: many 《 without 》 (plain-text path, also used for web text/plain)
    for (size_t n : {5000u, 10000u, 20000u}) {
        std::string s; for (size_t i = 0; i < n; ++i) s += "\xE3\x80\x8A";   // 《
        rt::Doc d; auto t = clk::now(); rt::parsePlain(s, d);
        printf("parsePlain %6zu x '《': %8.1f ms\n", n, ms(t));
    }
    // 5. Feed truncated right after "<item" (what maxBytes truncation produces)
    {
        std::string full = "<rss><channel><title>t</title><item";
        std::string_view v(full.data(), full.size());     // view ends exactly at "<item"
        rt::Feed f; rt::parseFeed(v, f);
        printf("parseFeed truncated at '<item': %zu items (no crash under ASan = ok)\n", f.items.size());
    }
    // 6. Feed item without <link>: summary images vs out.images
    {
        rt::Feed f;
        rt::parseFeed("<rss><channel><item><title>a</title><description>&lt;img src=\"/tdeckjisho/config/wifi.txt\" alt=\"x\"&gt;</description></item></channel></rss>", f);
        rt::Doc d; rt::feedItemToDoc(f.items[0], "F", d);
        size_t imgParas = 0; bool start = true;
        for (char c : d.text) { if (start && c == rt::IMAGE) ++imgParas; start = (c == '\n'); }
        printf("feed w/o link: IMAGE paragraphs=%zu, doc.images=%zu  (web img treated as local SD file)\n", imgParas, d.images.size());
    }
}
