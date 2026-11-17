// One libFuzzer target for every parser that sees outside bytes.
// The first byte picks the parser; the rest is the input.
#include <cstdint>
#include <string>
#include "core/Settings.hpp"
#include "core/Url.hpp"
#include "core/Utf8.hpp"
#include "dict/BuildIo.hpp"
#include "dict/JmdictXml.hpp"
#include "text/Feed.hpp"
#include "text/Images.hpp"
#include "text/Parsers.hpp"

class Mem : public build::ByteSource {
public:
    Mem(const uint8_t* d, size_t n) : _d(d), _n(n) {}
    size_t read(uint8_t* b, size_t n) override {
        if (n > _n - _p) n = _n - _p;
        for (size_t k = 0; k < n; ++k) b[k] = _d[_p + k];
        _p += n; return n;
    }
    uint32_t filePosition() const override { return (uint32_t)_p; }
private:
    const uint8_t* _d; size_t _n, _p = 0;
};

static void checkDoc(const rt::Doc& d) {
    // Invariants every parser must keep: valid UTF-8 if the input was valid,
    // and one Link per LINK code.
    size_t links = 0;
    for (char c : d.text) if (c == rt::LINK) ++links;
    if (links != d.links.size()) __builtin_trap();
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) return 0;
    const int which = data[0] % 9;
    const std::string s(reinterpret_cast<const char*>(data + 1), size - 1);
    rt::Doc d;
    switch (which) {
        case 0: rt::parseHtml(s, "https://x.org/a/b.html", rt::Source::Web, d); checkDoc(d); break;
        case 1: rt::parseHtml(s, "", rt::Source::Local, d); checkDoc(d); break;
        case 2: rt::parseMarkdown(s, d); checkDoc(d); break;
        case 3: rt::parsePlain(s, d); checkDoc(d); break;
        case 4: { rt::Feed f; rt::parseFeed(s, f); for (auto& it : f.items) { rt::Doc x; rt::feedItemToDoc(it, "F", x); checkDoc(x); if (!x.images.empty()) __builtin_trap(); } break; }
        case 5: { std::string e = rt::decodeEntities(s); (void)e; size_t i = 0; while (i < s.size()) { size_t b = i; utf8::next(s, i); if (i <= b) __builtin_trap(); } break; }
        case 6: { Mem m(data + 1, size - 1); build::JmdictReader r(m); build::JmEntry e; for (int k = 0; k < 1000 && r.next(e) == build::JmdictReader::Result::Entry; ++k) {} break; }
        case 7: { const std::string p = rt::resolveImagePath("/tdeckjisho/books/a.md", s); if (!p.empty() && p.rfind("/tdeckjisho/books/", 0) != 0) __builtin_trap(); break; }
        case 8: { const std::string r = url::resolve("https://x.org/a/b?c#d", s); (void)url::isWebUrl(r); (void)url::redirectProblem("https://x.org/", r); (void)url::host(r); break; }
    }
    return 0;
}
