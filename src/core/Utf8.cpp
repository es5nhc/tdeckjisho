#include "core/Utf8.hpp"

namespace utf8 {

uint32_t next(std::string_view s, size_t& i) {
    // static_cast<uint8_t>: `char` may be signed on some compilers (it is on
    // x86, it is NOT on Xtensa!). Comparing a signed char with 0x80 gives
    // surprising results, so we always look at bytes as unsigned numbers.
    const uint8_t b0 = static_cast<uint8_t>(s[i]);

    if (b0 < 0x80) { i += 1; return b0; }          // plain ASCII – the fast path

    // How many continuation bytes follow, and which bits of b0 are payload?
    int extra;
    uint32_t cp;
    if      ((b0 & 0xE0) == 0xC0) { extra = 1; cp = b0 & 0x1F; }
    else if ((b0 & 0xF0) == 0xE0) { extra = 2; cp = b0 & 0x0F; }
    else if ((b0 & 0xF8) == 0xF0) { extra = 3; cp = b0 & 0x07; }
    else { i += 1; return kReplacement; }          // stray continuation byte

    // Not enough bytes left: a character cut off at the end of the string.
    // (The last byte we will touch is s[i + extra], so it must be < size.)
    if (i + extra >= s.size()) { i += 1; return kReplacement; }
    for (int k = 1; k <= extra; ++k) {
        const uint8_t b = static_cast<uint8_t>(s[i + k]);
        if ((b & 0xC0) != 0x80) { i += 1; return kReplacement; }  // not 10xxxxxx
        cp = (cp << 6) | (b & 0x3F);               // shift in 6 payload bits
    }
    // SECURITY: the bit pattern alone is not enough. Reject the three classic tricks:
    //   * overlong forms: C0 81 is a 2-byte spelling of U+0001. Code that checks the
    //     BYTES for control codes would miss it, code that checks the decoded VALUE
    //     would see it – two checks disagreeing is how filters get bypassed.
    //   * surrogates D800–DFFF: they only exist inside UTF-16, never as characters.
    //   * values above U+10FFFF (F4 90.. and leads F5–F7): no such code point.
    // Each of them decodes as U+FFFD and advances one byte, like any other bad byte.
    static constexpr uint32_t kMinForLength[] = {0, 0x80, 0x800, 0x10000};
    if (cp < kMinForLength[extra] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        i += 1;
        return kReplacement;
    }
    i += 1 + extra;
    return cp;
}

void append(std::string& out, uint32_t cp) {
    // Never WRITE what next() would refuse to read: a surrogate or a value above
    // U+10FFFF (e.g. from "&#x110000;" in a web page) becomes U+FFFD.
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = kReplacement;
    // The inverse of next(): split the bits over 1–4 bytes.
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

bool isValid(std::string_view s) {
    for (size_t i = 0; i < s.size();) {
        const size_t before = i;
        const uint32_t cp = next(s, i);
        // A *real* U+FFFD in the text is 3 bytes long; a decoding error is
        // reported as U+FFFD but advances by only 1 byte. That tells them apart.
        if (cp == kReplacement && i - before == 1) return false;
    }
    return true;
}

std::string_view truncateBytes(std::string_view s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    size_t cut = maxBytes;
    // Walk back while we are pointing at a continuation byte (10xxxxxx).
    while (cut > 0 && (static_cast<uint8_t>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut);
}

size_t prev(std::string_view s, size_t i) {
    if (i == 0) return 0;
    if (i > s.size()) i = s.size();
    --i;
    // Walk back over continuation bytes (10xxxxxx) to the lead byte.
    while (i > 0 && (static_cast<uint8_t>(s[i]) & 0xC0) == 0x80) --i;
    return i;
}

size_t length(std::string_view s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); ++n) next(s, i);
    return n;
}

std::string katakanaToHiragana(std::string_view s) {
    std::string out;
    out.reserve(s.size());   // avoid repeated re-allocation while appending
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = next(s, i);
        // The katakana block is the hiragana block shifted by 0x60.
        // ァ(30A1)..ヶ(30F6) map onto ぁ(3041)..ゖ(3096). ー (30FC) stays.
        if (cp >= 0x30A1 && cp <= 0x30F6) cp -= 0x60;
        append(out, cp);
    }
    return out;
}

}  // namespace utf8
