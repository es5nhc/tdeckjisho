#include "text/Cursor.hpp"

#include <algorithm>

#include "core/Utf8.hpp"

namespace rt {

namespace {

// End of the paragraph that contains byte i (the position of its '\n', or the text size).
size_t paragraphEnd(const std::string& t, size_t i) {
    const size_t nl = t.find('\n', i);
    return nl == std::string::npos ? t.size() : nl;
}

// Width of text[from, to) as the layout measures it: inline codes change the style and take no space.
int measure(const std::string& t, size_t from, size_t to, char block, uint8_t style, Metrics& m,
            bool ruby, uint8_t* styleOut = nullptr) {
    int w = 0;
    const std::string_view view(t.data(), to);
    for (size_t k = from; k < to;) {
        if (isInlineCode(t[k])) { style = applyCode(style, t[k]); ++k; continue; }
        w += m.advance(utf8::next(view, k), block, ruby ? (style | S_RUBY) : style);
    }
    if (styleOut) *styleOut = style;
    return w;
}

}  // namespace

std::vector<Unit> lineUnits(const Doc& doc, const Line& line, Metrics& m) {
    std::vector<Unit> out;
    if (line.image >= 0 || line.block == RULE) return out;
    const std::string& t = doc.text;
    const std::string_view view(t.data(), line.end);
    uint8_t style = line.style;
    int x = 0;
    for (size_t i = line.start; i < line.end;) {
        const char c = t[i];
        Unit u;
        u.start = static_cast<uint32_t>(i);
        u.x = x;
        size_t r1, r2;
        if (c == BASE) {
            if (!findRubyGroup(t, i, line.end, r1, r2)) { ++i; continue; }   // stray BASE: ignored
            uint8_t after = style;
            const int baseW = measure(t, i + 1, r1, line.block, style, m, false, &after);
            const int rubyW = measure(t, r1 + 1, r2, line.block, after, m, true);
            u.w = m.showRuby() ? std::max(baseW, rubyW) : baseW;
            i = r2 + 1;
            style = after;
        } else if (isInlineCode(c)) {
            style = applyCode(style, c);
            ++i;
            continue;
        } else {
            const uint32_t cp = utf8::next(view, i);
            const bool hidden = (style & S_RUBY) && !m.showRuby();
            u.w = hidden ? 0 : m.advance(cp, line.block, style);
            u.selectable = !(style & S_RUBY);
        }
        u.end = static_cast<uint32_t>(i);
        x += u.w;
        out.push_back(u);
    }
    return out;
}

int unitAtX(const std::vector<Unit>& units, int x) {
    int best = -1;
    for (size_t k = 0; k < units.size(); ++k) {
        if (!units[k].selectable) continue;
        if (best < 0) best = static_cast<int>(k);              // at least the first selectable one
        if (x >= units[k].x) best = static_cast<int>(k);       // the last one that starts at or before x
        else break;
    }
    return best;
}

int unitUnderX(const std::vector<Unit>& units, int x, int slack) {
    const int k = unitAtX(units, x);
    if (k < 0) return -1;
    const Unit& u = units[k];
    return (x >= u.x - slack && x < u.x + u.w + slack) ? k : -1;
}

size_t firstUnitAtOrAfter(const Doc& doc, size_t i) {
    const std::string& t = doc.text;
    const size_t n = t.size();
    while (i < n) {
        const char c = t[i];
        if (c == '\n' || isBlockCode(c)) { ++i; continue; }
        if (c == BASE) {
            size_t r1, r2;
            if (findRubyGroup(t, i, paragraphEnd(t, i), r1, r2)) return i;
            ++i;                                               // stray BASE
            continue;
        }
        if (c == RUBY) {                                       // inline furigana: skip to its closing RUBY
            const size_t j = t.find(RUBY, i + 1);
            i = j == std::string::npos ? n : j + 1;
            continue;
        }
        if (isInlineCode(c)) { ++i; continue; }
        return i;
    }
    return kNoOffset;
}

size_t nextOffset(const Doc& doc, size_t off) {
    const std::string& t = doc.text;
    if (off >= t.size()) return kNoOffset;
    size_t i = off, r1, r2;
    if (t[i] == BASE && findRubyGroup(t, i, paragraphEnd(t, i), r1, r2)) {
        i = r2 + 1;                                            // leave the whole group
    } else {
        utf8::next(t, i);                                      // leave one character
    }
    return firstUnitAtOrAfter(doc, i);
}

size_t prevOffset(const Doc& doc, size_t off) {
    const std::string& t = doc.text;
    size_t limit = std::min(off, t.size());
    while (limit > 0) {
        // Walk the paragraph before `limit` from its start, remembering the last unit.
        const size_t nl = t.rfind('\n', limit - 1);
        const size_t p = nl == std::string::npos ? 0 : nl + 1;
        size_t last = kNoOffset;
        for (size_t cur = firstUnitAtOrAfter(doc, p); cur != kNoOffset && cur < limit; cur = nextOffset(doc, cur))
            last = cur;
        if (last != kNoOffset) return last;
        if (p == 0) break;
        limit = p - 1;                                         // continue in the paragraph before
    }
    return kNoOffset;
}

size_t latinWordStart(const Doc& doc, size_t off) {
    const std::string& t = doc.text;
    auto letter = [&](size_t o) {
        const char c = t[o];
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    };
    if (off >= t.size() || !letter(off)) return off;
    for (;;) {
        const size_t p = prevOffset(doc, off);
        // the unit before must be a letter, directly before (no paragraph end between)
        if (p == kNoOffset || !letter(p) || t.find('\n', p) < off) break;
        off = p;
    }
    return off;
}

std::string plainTextFrom(const Doc& doc, size_t off, size_t maxChars, size_t* endOffset) {
    const std::string& t = doc.text;
    const size_t end = off < t.size() ? paragraphEnd(t, off) : t.size();
    std::string out;
    size_t chars = 0, i = off, taken = off;
    while (i < end && chars < maxChars) {
        const char c = t[i];
        size_t r1, r2;
        if (c == BASE && findRubyGroup(t, i, end, r1, r2)) {   // the base text of a group, not the reading
            const std::string_view view(t.data(), r1);
            for (size_t k = i + 1; k < r1 && chars < maxChars;) {
                if (isInlineCode(t[k])) { ++k; continue; }
                const size_t at = k;
                utf8::next(view, k);
                out.append(t, at, k - at);
                ++chars;
            }
            i = taken = r2 + 1;
            continue;
        }
        if (c == RUBY) {                                       // inline furigana
            const size_t j = t.find(RUBY, i + 1);
            i = j == std::string::npos ? end : j + 1;
            continue;
        }
        if (isInlineCode(c) || isBlockCode(c)) { ++i; continue; }
        const size_t at = i;
        utf8::next(std::string_view(t.data(), end), i);
        out.append(t, at, i - at);
        ++chars;
        taken = i;
    }
    if (endOffset) *endOffset = taken;
    return out;
}

std::string plainTextBefore(const Doc& doc, size_t off, size_t maxChars) {
    // Step back over up to maxChars units, staying in the paragraph.
    size_t start = off;
    for (size_t n = 0; n < maxChars; ++n) {
        const size_t p = prevOffset(doc, start);
        if (p == kNoOffset || doc.text.find('\n', p) < start) break;
        start = p;
    }
    if (start == off) return "";
    // plainTextFrom() stops after a number of CHARACTERS, not at an offset: take one more each
    // time until the next one would pass `off`. (A furigana group is taken whole, so the end
    // offset can stay at `off` while its characters are added one by one.)
    std::string out;
    for (size_t k = 1; k <= maxChars * 4; ++k) {
        size_t e = 0;
        std::string s = plainTextFrom(doc, start, k, &e);
        if (e > off || s.size() <= out.size()) break;
        out = std::move(s);
    }
    return out;
}

}  // namespace rt
