// ============================================================================
//  Markdown.cpp – a small, forgiving Markdown subset
// ============================================================================
//
//  Real Markdown (CommonMark) has a 30-page spec. For an e-reader we only
//  need the 90 % case, parsed in two levels:
//
//   1. BLOCK level, line by line: what kind of paragraph is this line?
//   2. INLINE level, inside a paragraph: **bold**, [links](...), {ruby|...}
//
//  Consecutive normal lines are joined into one paragraph (as in Markdown).
//  Joining Japanese lines must NOT insert a space ("日本\n語" is "日本語"),
//  joining English (or mixed) lines must ("hello\nworld" is "hello world").
// ============================================================================

#include "core/Str.hpp"
#include "core/Utf8.hpp"
#include "text/Parsers.hpp"

namespace rt {

namespace {

// Last code point of a string (0 if empty).
uint32_t lastCodePoint(std::string_view s) {
    if (s.empty()) return 0;
    size_t i = s.size() - 1;
    while (i > 0 && (static_cast<uint8_t>(s[i]) & 0xC0) == 0x80) --i;   // back to lead byte
    return utf8::next(s, i);
}
uint32_t firstCodePoint(std::string_view s) {
    size_t i = 0;
    return s.empty() ? 0 : utf8::next(s, i);
}

// SECURITY: "find the closing bracket" inside a loop over the same text is the
// classic accidental O(n²). A paragraph of 100 000 '[' with no "](" would scan to the
// end 100 000 times. A NextFinder remembers its last answer: while the position only
// grows, every byte is searched at most once. find(pos) means exactly s.find(needle, pos).
struct NextFinder {
    std::string_view s, needle;
    size_t from = std::string_view::npos;     // where the remembered search started
    size_t hit = std::string_view::npos;      // what it found
    size_t find(size_t pos) {
        if (from != std::string_view::npos && pos >= from && (hit == std::string_view::npos || hit >= pos))
            return hit;                       // the remembered answer is still right
        from = pos;
        hit = s.find(needle, pos);
        return hit;
    }
};

// Link labels may contain markup, so they are parsed recursively. Today a label ends at
// the FIRST "](", so it can never hold another link and the depth stays 1 – but that is
// a property of the current code, not a promise. Recursion on input-controlled data
// always gets an explicit limit: the ESP32's task stack is only 8 KB, and running out
// of it is a crash, not an exception.
constexpr int kMaxLinkDepth = 2;

// ---- inline parsing --------------------------------------------------------
// Walks one paragraph's raw text and emits styled text into the Builder.
void inlineMarkdown(std::string_view s, Builder& b, int depth = 0) {
    bool bold = false, em = false, code = false;
    std::string pending;                  // plain text not yet sent to b.text()
    auto flush = [&] { b.text(pending); pending.clear(); };
    NextFinder linkMid{s, "]("}, paren{s, ")"}, pipe{s, "|"}, brace{s, "}"};

    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        // `code` – inside backticks nothing else is interpreted.
        if (c == '`') { flush(); code = !code; b.code(ACCENT); continue; }
        if (code) { pending += c; continue; }

        // Backslash escape: \* prints a literal *
        if (c == '\\' && i + 1 < s.size()) { pending += s[++i]; continue; }

        // **bold** or __bold__
        if ((c == '*' || c == '_') && i + 1 < s.size() && s[i + 1] == c) {
            flush(); bold = !bold; b.code(BOLD); ++i; continue;
        }
        // *emphasis* – we have no italic font that fits, so use the accent colour.
        // A '_' inside a word (snake_case) is NOT emphasis.
        if (c == '*' || (c == '_' && (i == 0 || s[i - 1] == ' '))) {
            flush(); em = !em; b.code(ACCENT); continue;
        }
        // ![alt](src) – a picture. It becomes its own IMAGE paragraph; text
        // after it continues in a new paragraph below.
        // [![alt](src)](url) – a linked picture (common in READMEs): we show
        // the picture and drop the link.
        const bool linkedImage = c == '[' && s.substr(i, 3) == "[![";
        if ((c == '!' && i + 1 < s.size() && s[i + 1] == '[') || linkedImage) {
            const size_t open = i + (linkedImage ? 2 : 1);          // the '[' of the image
            const size_t mid = linkMid.find(open);
            const size_t end = mid == std::string_view::npos ? mid : paren.find(mid);
            if (end != std::string_view::npos) {
                flush();
                std::string src(str::trim(s.substr(mid + 2, end - mid - 2)));
                // ![a](pic.jpg "Title"): drop the optional title.
                if (const size_t sp = src.find(' '); sp != std::string::npos) src.resize(sp);
                b.image(src, std::string(s.substr(open + 1, mid - open - 1)));
                i = end;
                if (linkedImage && s.substr(end + 1, 2) == "](") {
                    const size_t close = paren.find(end + 3);
                    if (close != std::string_view::npos) i = close;
                }
                continue;
            }
        }
        // [label](url)
        if (c == '[') {
            const size_t close = linkMid.find(i);
            const size_t end = close == std::string_view::npos ? close : paren.find(close);
            if (end != std::string_view::npos) {
                flush();
                b.beginLink(std::string(s.substr(close + 2, end - close - 2)));
                const std::string_view label = s.substr(i + 1, close - i - 1);
                if (depth < kMaxLinkDepth) inlineMarkdown(label, b, depth + 1);   // recursion!
                else b.text(label);                                              // deep enough
                b.endLink();
                i = end;
                continue;
            }
        }
        // {漢字|かんじ} ruby
        if (c == '{') {
            const size_t bar = pipe.find(i);
            const size_t end = brace.find(i);
            if (bar != std::string_view::npos && end != std::string_view::npos && bar < end) {
                flush();
                b.code(BASE);                               // furigana group:
                b.text(s.substr(i + 1, bar - i - 1));       //   BASE base
                b.code(RUBY);                               //   RUBY ruby RUBY
                b.text(s.substr(bar + 1, end - bar - 1));
                b.code(RUBY);
                i = end;
                continue;
            }
        }
        pending += c;
    }
    flush();
    (void)bold; (void)em;   // styles reset at the paragraph end anyway
}

}  // namespace

void parseMarkdown(std::string_view src, Doc& out) {
    Builder b(out);
    std::string para;           // raw text of the paragraph being collected
    char paraBlock = 0;         // its block type
    bool inFence = false;       // inside ``` ... ```

    // Emit the collected paragraph (if any).
    auto flushPara = [&] {
        if (para.empty()) return;
        b.paragraph(paraBlock);
        inlineMarkdown(para, b);
        para.clear();
        paraBlock = 0;
    };

    for (std::string_view raw : str::split(src, '\n')) {
        if (!raw.empty() && raw.back() == '\r') raw.remove_suffix(1);   // Windows files

        // ---- fenced code block -------------------------------------------
        if (str::startsWith(str::trim(raw), "```")) {
            flushPara();
            inFence = !inFence;
            continue;
        }
        if (inFence) {               // every line verbatim, one PRE paragraph each
            b.paragraph(PRE);
            b.text(raw, /*keepSpaces=*/true);
            continue;
        }

        const std::string_view line = str::trim(raw);
        if (line.empty()) { flushPara(); continue; }   // blank line ends a paragraph

        // ---- block markers -----------------------------------------------
        if (line == "---" || line == "***" || line == "___") { flushPara(); b.rule(); continue; }
        if (str::startsWith(line, "# "))  { flushPara(); paraBlock = H1; para = line.substr(2); flushPara(); continue; }
        if (str::startsWith(line, "## ") || str::startsWith(line, "### ") || str::startsWith(line, "#### ")) {
            flushPara();
            paraBlock = H2;
            para = str::trim(line.substr(line.find(' ')));
            flushPara();
            continue;
        }
        if (str::startsWith(line, "- ") || str::startsWith(line, "* ") || str::startsWith(line, "+ ")) {
            flushPara(); paraBlock = BULLET; para = line.substr(2); continue;
        }
        // "1. item" – keep the number visible, draw it like a bullet.
        if (line.size() > 2 && line[0] >= '0' && line[0] <= '9') {
            const size_t dot = line.find(". ");
            if (dot != std::string_view::npos && dot < 4) {
                flushPara(); paraBlock = BULLET; para = line; continue;
            }
        }
        if (str::startsWith(line, ">")) {
            const std::string_view q = str::trim(line.substr(1));
            if (paraBlock != QUOTE) flushPara();
            paraBlock = QUOTE;
            if (!para.empty()) para += ' ';
            para += q;
            continue;
        }

        // ---- continuation of a normal paragraph --------------------------
        if (!para.empty()) {
            // Join WITHOUT a space only when both sides are CJK.
            const bool cjkJoin = utf8::isCJK(lastCodePoint(para)) && utf8::isCJK(firstCodePoint(line));
            if (!cjkJoin) para += ' ';
        }
        para += line;
    }
    flushPara();
}

}  // namespace rt
