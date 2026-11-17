// ============================================================================
//  Plain.cpp – plain text, with Aozora Bunko (青空文庫) ruby notation
// ============================================================================
//
//  Aozora Bunko (https://www.aozora.gr.jp) publishes thousands of
//  public-domain Japanese books as .txt files with this markup:
//
//      漢字《かんじ》        ruby for the run of kanji right before 《
//      ｜青空《あおぞら》    "｜" marks where the ruby base starts explicitly
//      ［＃「漢字」に傍点］   editorial notes – we show them dimmed
//
//  NOTE: Aozora files are distributed in Shift_JIS encoding. Convert them
//  once on the PC:   iconv -f SHIFT_JIS -t UTF-8 in.txt > out.txt
//  (Shift_JIS -> Unicode needs a 7000-entry table; not worth it on device.)
//
//  Every source line becomes one paragraph (the usual convention for both
//  Japanese texts and ebooks-from-text). Blank lines become visible gaps.
// ============================================================================

#include "core/Str.hpp"
#include "core/Utf8.hpp"
#include "text/Parsers.hpp"

namespace rt {

namespace {

constexpr uint32_t kRubyBar = 0xFF5C;    // ｜
constexpr uint32_t kRubyOpen = 0x300A;   // 《
constexpr uint32_t kRubyClose = 0x300B;  // 》
constexpr uint32_t kNoteOpen = 0xFF3B;   // ［
constexpr uint32_t kNoteClose = 0xFF3D;  // ］

// Walk back from the end of `out` over a run of kanji; return where it starts.
size_t kanjiRunStart(const std::string& out) {
    size_t k = out.size();
    while (k > 0) {
        size_t lead = k - 1;                       // find the lead byte of the
        while (lead > 0 && (static_cast<uint8_t>(out[lead]) & 0xC0) == 0x80) --lead;  // previous char
        size_t tmp = lead;
        if (!utf8::isKanji(utf8::next(out, tmp))) break;
        k = lead;
    }
    return k;
}

void parseLine(std::string_view line, Builder& b) {
    // We build the paragraph in `out` first, because when we meet 《 we need
    // to go BACK and mark where the ruby base started:
    //     吾輩《わがはい》  ->  BASE 吾輩 RUBY わがはい RUBY
    // `out` contains our own codes (BASE, RUBY, DIM) mixed with text; the
    // file's own control bytes were already replaced by spaces in parsePlain.
    std::string out;
    size_t baseStart = std::string::npos;   // set by ｜
    bool inNote = false;
    // SECURITY: once we know there is no 》 after some point, never search there
    // again. Without this, a line of 100 000 《 and no 》 scans to the end of the
    // line 100 000 times: hours on the ESP32 for one hostile web page.
    size_t noCloseFrom = std::string_view::npos;

    for (size_t i = 0; i < line.size();) {
        const size_t at = i;                     // byte where this character starts
        const uint32_t cp = utf8::next(line, i); // i now points after it

        if (cp == kRubyBar) { baseStart = out.size(); continue; }   // ｜ explicit base start

        if (cp == kRubyOpen) {
            // Find the closing 》 (unless we already know there is none).
            size_t close = std::string_view::npos;
            if (i < noCloseFrom) {
                for (size_t j = i; j < line.size();) {
                    const size_t pos = j;
                    if (utf8::next(line, j) == kRubyClose) { close = pos; break; }
                }
                if (close == std::string_view::npos) noCloseFrom = i;
            }
            if (close == std::string_view::npos) { out.append(line.substr(at, i - at)); continue; }

            // No ｜: the base is the run of kanji just before 《.
            if (baseStart == std::string::npos) baseStart = kanjiRunStart(out);
            if (baseStart < out.size()) out.insert(baseStart, 1, BASE);
            // (if there is no base at all, the RUBY pair is drawn inline)
            out += RUBY;
            out.append(line.substr(i, close - i));
            out += RUBY;
            i = close + 3;                         // 》 is 3 bytes in UTF-8
            baseStart = std::string::npos;
            continue;
        }

        if (cp == kNoteOpen && !inNote) {        // ［＃ starts an editorial note
            size_t j = i;
            if (j < line.size() && utf8::next(line, j) == 0xFF03) {   // ＃
                out += DIM;
                inNote = true;
            }
        }
        out.append(line.substr(at, i - at));
        if (cp == kNoteClose && inNote) {        // ］ ends it
            out += DIM;
            inNote = false;
        }
    }

    // Hand the paragraph to the Builder: text pieces via text(), our codes
    // via code(), so the Builder's escaping rules still apply to the text.
    size_t start = 0;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == BASE || out[i] == RUBY || out[i] == DIM) {
            b.text(std::string_view(out).substr(start, i - start), /*keepSpaces=*/true);
            b.code(out[i]);
            start = i + 1;
        }
    }
    b.text(std::string_view(out).substr(start), true);
}

}  // namespace

void parsePlain(std::string_view src, Doc& out) {
    Builder b(out);
    // Strip a UTF-8 byte-order mark (EF BB BF) that Windows editors add.
    if (str::startsWith(src, "\xEF\xBB\xBF")) src.remove_prefix(3);

    for (std::string_view line : str::split(src, '\n')) {
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        // Control bytes in the FILE (tabs, or garbage) become spaces BEFORE
        // parsing, so they can never be mistaken for our own codes.
        std::string clean(line);
        for (char& c : clean) if (static_cast<uint8_t>(c) < 0x20) c = ' ';
        if (str::trim(clean).empty()) { b.blankLine(); continue; }
        b.paragraph();
        parseLine(clean, b);
    }
}

}  // namespace rt
