#include "text/RichText.hpp"

namespace rt {

void Builder::paragraph(char block) {
    if (_inLink) endLink();                       // links never span paragraphs
    // Avoid empty paragraphs: if the current one has no visible text yet,
    // reuse it (replace its block code) instead of emitting "\n\n".
    if (!_d.text.empty() && _d.text.back() != '\n') {
        if (paragraphEmpty()) {
            // Drop the dangling codes of the empty paragraph.
            // If one of them was a LINK code, its entry in `links` must go
            // too, or every later link would be off by one.
            while (!_d.text.empty() && _d.text.back() != '\n') {
                if (_d.text.back() == LINK && !_d.links.empty()) _d.links.pop_back();
                _d.text.pop_back();
            }
        } else {
            _d.text += '\n';
        }
    }
    if (block) _d.text += block;
    _lastWasSpace = true;
}

void Builder::blankLine() {
    paragraph();                         // close the current paragraph...
    if (!_d.text.empty()) _d.text += '\n';   // ...and add an empty one after it
    _lastWasSpace = true;
}

bool Builder::paragraphEmpty() const {
    // Scan back to the previous '\n'; any byte that is not a control code
    // means there is visible text. RULE paragraphs count as non-empty.
    for (size_t i = _d.text.size(); i > 0; --i) {
        const char c = _d.text[i - 1];
        if (c == '\n') return true;
        if (c == RULE) return false;
        if (static_cast<uint8_t>(c) >= 0x20) return false;
    }
    return true;
}

void Builder::text(std::string_view s, bool keepSpaces) {
    for (char c : s) {
        // Every byte below 0x20 (tab, newline, our codes...) becomes a space.
        // Multi-byte UTF-8 characters only contain bytes >= 0x80, so they pass
        // through unharmed.
        if (static_cast<uint8_t>(c) < 0x20) c = ' ';
        if (c == ' ' && !keepSpaces) {
            if (_lastWasSpace) continue;          // collapse "a   b" to "a b"
            _lastWasSpace = true;
        } else {
            _lastWasSpace = false;
        }
        _d.text += c;
        if (_inLink) _d.links.back().label += c;
    }
}

void Builder::image(const std::string& src, const std::string& alt) {
    if (src.empty()) return;
    paragraph(IMAGE);
    _d.images.push_back({src, alt});
    // The caption. Never empty: an empty paragraph would be dropped by
    // paragraph(), and then images[n] would belong to the wrong paragraph.
    std::string caption = alt.empty() ? src.substr(src.rfind('/') + 1) : alt;
    text(caption);
    if (paragraphEmpty()) text("image");
    paragraph();
}

void Builder::beginLink(const std::string& url) {
    if (_inLink) endLink();
    _d.links.push_back({url, ""});
    _d.text += LINK;
    _inLink = true;
}

void Builder::endLink() {
    if (!_inLink) return;
    _d.text += ENDLINK;
    _inLink = false;
    // A label never needs trailing spaces (a block tag inside the link leaves one).
    std::string& label = _d.links.back().label;
    while (!label.empty() && label.back() == ' ') label.pop_back();
}

}  // namespace rt
