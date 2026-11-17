#pragma once
// ============================================================================
//  RichText.hpp – one compact document format for everything we display
// ============================================================================
//
//  Dictionary entries, Markdown books, Aozora Bunko texts, web pages and RSS
//  articles are all converted into ONE format, and ONE renderer shows them.
//  Write the hard part (wrapping Japanese + Latin text with mixed fonts) once.
//
//  The format: a single UTF-8 std::string where a few CONTROL BYTES carry the
//  formatting – much like ANSI colour codes in a terminal. Example:
//
//      "\x11Title\n" "Some \x01bold\x01 text and \x03a link\x04.\n"
//
//  Why not a tree of objects (vector<Paragraph> of vector<Span>...)?
//    * One allocation instead of thousands. On the ESP32 thousands of small
//      allocations fragment the ~300 KB of fast internal RAM; one big string
//      goes to the 8 MB PSRAM in a single block.
//    * Trivially fast to scan, trivially easy to debug (print it!).
//  The cost: you must never let raw control bytes from the SOURCE text into
//  the document – Builder::text() strips them. (Same idea as escaping user
//  input before putting it into SQL or HTML.)
//
//  Rules
//    * '\n' ends a paragraph. Inline styles are reset at every '\n', so a
//      missing "off" code can never bleed into the rest of the document.
//    * A BLOCK code (0x11..0x17) may appear only as the first byte of a
//      paragraph and sets how the whole paragraph is drawn.
//    * INLINE codes (0x01..0x07) toggle a style on/off.
//
//  Furigana (ruby) is written as a GROUP of three codes:
//
//      BASE 漢字 RUBY かんじ RUBY          ->   かんじ
//                                               漢字
//  BASE marks where the base text starts; the text between the two RUBY codes
//  is drawn small, centred ABOVE the base. Layout treats the whole group as
//  one unbreakable unit, as wide as the wider of base and ruby.
//  A RUBY pair WITHOUT a preceding BASE is drawn inline (small, after the
//  text) – a fallback for sources where the base cannot be found.
// ============================================================================

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rt {

// ---- inline toggles --------------------------------------------------------
constexpr char BOLD   = '\x01';
constexpr char RUBY   = '\x02';   // furigana: drawn small & dim, can be hidden
constexpr char LINK   = '\x03';   // link start (the n-th LINK uses links[n])
constexpr char ENDLINK= '\x04';
constexpr char ACCENT = '\x05';   // highlight colour (headwords, numbers)
constexpr char DIM    = '\x06';   // grey (part-of-speech tags, dates)
constexpr char BASE   = '\x07';   // start of a ruby base (see above)

// ---- paragraph (block) types ----------------------------------------------
constexpr char H1     = '\x11';
constexpr char H2     = '\x12';
constexpr char BULLET = '\x13';
constexpr char QUOTE  = '\x14';
constexpr char RULE   = '\x15';   // horizontal line, paragraph has no text
constexpr char PRE    = '\x16';   // code / preformatted
constexpr char SMALL  = '\x17';   // small print (metadata)
// A picture. The n-th IMAGE paragraph shows doc.images[n]; its TEXT is the
// caption (the alt text), which is also what is shown if the picture can't be.
constexpr char IMAGE  = '\x18';

inline bool isInlineCode(char c) { return c >= '\x01' && c <= '\x07'; }
inline bool isBlockCode(char c) { return c >= '\x11' && c <= '\x18'; }

// Style bits used while laying out / drawing (one bit per inline toggle).
enum StyleBit : uint8_t {
    S_BOLD = 1, S_RUBY = 2, S_LINK = 4, S_ACCENT = 8, S_DIM = 16,
};
// Flip the bit that belongs to an inline code. Returns the new style.
inline uint8_t applyCode(uint8_t style, char code) {
    switch (code) {
        case BOLD:    return style ^ S_BOLD;
        case RUBY:    return style ^ S_RUBY;
        case LINK:    return style | S_LINK;
        case ENDLINK: return style & ~S_LINK;
        case ACCENT:  return style ^ S_ACCENT;
        case DIM:     return style ^ S_DIM;
        case BASE:    return style;          // a position marker, not a style
        default:      return style;
    }
}

struct Link {
    std::string url;
    std::string label;
};

// An image reference. Parsers fill src/alt; text/Images.cpp later resolves
// the path and reads the size from the file's header (without decoding it).
enum class ImageFormat : uint8_t { Unknown, Jpeg, Png, Bmp, Qoi };
struct Image {
    std::string src;             // as written in the document: "img/fuji.jpg"
    std::string alt;
    std::string path;            // resolved SD path, "" if not loadable
    ImageFormat format = ImageFormat::Unknown;
    uint16_t w = 0, h = 0;       // pixels; 0 = cannot be shown
    std::string problem;         // why not, e.g. "progressive JPEG"
};

struct Doc {
    std::string title;
    std::string text;            // the formatted stream described above
    std::vector<Link> links;     // in order of appearance
    std::vector<Image> images;   // in order of appearance (IMAGE paragraphs)
    std::string baseUrl;         // for web pages: where relative links point
};

// Builder – the ONLY way parsers write into a Doc. It keeps the invariants:
// control bytes are escaped, paragraphs are well-formed, links are counted.
class Builder {
public:
    explicit Builder(Doc& doc) : _d(doc) {}

    // Start a new paragraph, optionally with a block code (H1, BULLET...).
    void paragraph(char block = 0);
    // Append visible text. Control bytes and '\n' are turned into spaces;
    // runs of whitespace are collapsed unless `keepSpaces` (for PRE).
    void text(std::string_view s, bool keepSpaces = false);
    // Inline toggles.
    void code(char c) { _d.text += c; }
    void beginLink(const std::string& url);
    void endLink();
    void rule() { paragraph(RULE); paragraph(); }
    // A picture as its own paragraph; afterwards a normal paragraph begins,
    // so text that followed the image inline continues below it.
    void image(const std::string& src, const std::string& alt);
    // An intentionally empty paragraph (a blank line in a plain-text file).
    void blankLine();

    bool paragraphEmpty() const;   // nothing visible since paragraph() ?

private:
    Doc& _d;
    bool _inLink = false;
    bool _lastWasSpace = true;     // for whitespace collapsing
};

}  // namespace rt
