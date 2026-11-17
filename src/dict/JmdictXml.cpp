#include "dict/JmdictXml.hpp"

#include <cstdlib>   // strtoul

#include "core/Utf8.hpp"

namespace build {

namespace {
constexpr size_t kBuf = 16 * 1024;
bool isSpace(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
}  // namespace

// ============================================================================
//  XmlPull – byte level
// ============================================================================

XmlPull::XmlPull(ByteSource& src) : _src(src), _buf(kBuf) {}

// get()/peek() are called for every one of the ~60 million bytes, so they are
// tiny and the slow path (refilling the buffer) is rare.
int XmlPull::get() {
    if (_pos == _len) {
        if (_eof) return -1;
        _len = _src.read(_buf.data(), _buf.size());
        _pos = 0;
        if (_len == 0) { _eof = true; return -1; }
    }
    return _buf[_pos++];
}

int XmlPull::peek() {
    const int c = get();
    if (c >= 0) --_pos;     // put it back (get() just filled or advanced the buffer)
    return c;
}

bool XmlPull::skipPast(std::string_view end) {
    // Keep the last end.size() bytes in a little window and compare. Simpler
    // to get right than matching byte by byte: "--->" must still end "-->".
    std::string window;
    for (int c = get(); c >= 0; c = get()) {
        window += static_cast<char>(c);
        if (window.size() > end.size()) window.erase(0, 1);
        if (window == end) return true;
    }
    return false;
}

bool XmlPull::readName(std::string& out) {
    out.clear();
    for (int c = peek(); c >= 0 && !isSpace(c) && c != '>' && c != '/' && c != '=' && c != '<' && c != '[';
         c = peek()) {
        if (out.size() == kMaxXmlName) { if (_error.empty()) _error = "XML name too long"; return false; }
        out += static_cast<char>(get());
    }
    return !out.empty();
}

// "&...;" has been reached (the '&' is consumed). Append its text to `out`.
// Returns true for a DTD entity (and puts its name in `dtdName`), false for
// the five built-in ones and &#...; character references.
bool XmlPull::decodeEntity(std::string& out, std::string& dtdName) {
    std::string name;
    for (int c = get(); c >= 0 && c != ';'; c = get()) {
        name += static_cast<char>(c);
        if (name.size() > 40) break;                // not an entity after all
    }
    if (!name.empty() && name[0] == '#') {          // &#26085; or &#x65E5;
        const bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
        utf8::append(out, static_cast<uint32_t>(std::strtoul(name.c_str() + (hex ? 2 : 1), nullptr, hex ? 16 : 10)));
        return false;
    }
    if (name == "amp") { out += '&'; return false; }
    if (name == "lt") { out += '<'; return false; }
    if (name == "gt") { out += '>'; return false; }
    if (name == "quot") { out += '"'; return false; }
    if (name == "apos") { out += '\''; return false; }
    // A DTD entity. The text gets its description (as in Python's parser),
    // and the caller learns the NAME: for <pos>&v1;</pos> it wants "v1".
    const auto it = _entities.find(name);
    out += (it != _entities.end()) ? it->second : name;
    dtdName = name;
    return true;
}

// <!DOCTYPE JMdict [ ...declarations... ]>  – we keep only <!ENTITY name "text">.
void XmlPull::parseDoctype() {
    for (int c = get(); c >= 0; c = get()) {
        if (c == '>') return;                       // no internal subset
        if (c == '[') break;
    }
    for (int c = get(); c >= 0; c = get()) {
        if (c == ']') { skipPast(">"); return; }
        if (c != '<') continue;
        if (peek() == '?') { skipPast("?>"); continue; }
        if (peek() != '!') continue;
        get();
        if (peek() == '-') { skipPast("-->"); continue; }
        std::string decl;
        readName(decl);
        if (decl != "ENTITY") { skipPast(">"); continue; }
        while (isSpace(peek())) get();
        if (peek() == '%') { skipPast(">"); continue; }   // parameter entity: not used by JMdict
        std::string name;
        if (!readName(name) && !_error.empty()) return;
        while (isSpace(peek())) get();
        const int quote = get();
        std::string value;
        for (int v = get(); v >= 0 && v != quote; v = get()) {
            if (value.size() == kMaxXmlValue) { _error = "XML entity value too long"; return; }
            value += static_cast<char>(v);
        }
        if (_entities.size() == kMaxXmlEntities && !_entities.count(name)) {
            _error = "too many XML entity definitions";
            return;
        }
        _entities[name] = value;
        skipPast(">");
    }
}

void XmlPull::parseAttributes(bool& selfClosing) {
    selfClosing = false;
    _lang.clear();
    while (true) {
        while (isSpace(peek())) get();
        const int c = peek();
        if (c < 0) return;
        if (c == '>') { get(); return; }
        if (c == '/') { get(); skipPast(">"); selfClosing = true; return; }
        std::string attr;
        if (!readName(attr)) { get(); continue; }   // junk: skip one byte
        while (isSpace(peek())) get();
        if (peek() != '=') continue;                // attribute without value
        get();
        while (isSpace(peek())) get();
        const int quote = get();
        std::string value;
        for (int v = get(); v >= 0 && v != quote; v = get()) {
            if (value.size() == kMaxXmlValue) { _error = "XML attribute value too long"; return; }
            value += static_cast<char>(v);
        }
        if (attr == "xml:lang") _lang = value;
    }
}

XmlPull::Ev XmlPull::next() {
    if (_pendingClose) { _pendingClose = false; return Ev::Close; }
    while (true) {
        if (!_error.empty()) return Ev::Error;      // a cap was hit somewhere below
        int c = peek();
        if (c < 0) {
            if (!_src.error().empty()) { _error = _src.error(); return Ev::Error; }
            return Ev::End;
        }
        if (c != '<') {
            // ---- text, up to the next tag --------------------------------
            _text.clear();
            _entity.clear();
            int dtdEntities = 0;                    // how many &name; from the DTD
            bool other = false;                     // any other visible character?
            std::string ename;
            for (c = peek(); c >= 0 && c != '<'; c = peek()) {
                if (_text.size() > kMaxXmlText) { _error = "XML text node larger than 64 KB"; return Ev::Error; }
                get();
                if (c == '&') {
                    if (decodeEntity(_text, ename)) ++dtdEntities;
                    else other = true;
                    continue;
                }
                _text += static_cast<char>(c);
                if (!isSpace(c)) other = true;
            }
            if (dtdEntities == 1 && !other) _entity = ename;
            return Ev::Text;
        }
        get();                                      // the '<'
        c = peek();
        if (c == '?') { skipPast("?>"); continue; } // <?xml version="1.0"?>
        if (c == '!') {
            get();
            if (peek() == '-') { skipPast("-->"); continue; }      // <!-- comment -->
            if (peek() == '[') { skipPast("]]>"); continue; }      // CDATA: not in JMdict
            std::string decl;
            readName(decl);
            if (decl == "DOCTYPE") parseDoctype();
            else skipPast(">");
            continue;
        }
        if (c == '/') {
            get();
            readName(_name);
            skipPast(">");
            return Ev::Close;
        }
        if (!readName(_name)) { _error = "malformed tag"; return Ev::Error; }
        bool selfClosing = false;
        parseAttributes(selfClosing);
        if (!_error.empty()) return Ev::Error;
        _pendingClose = selfClosing;
        return Ev::Open;
    }
}

// ============================================================================
//  JmdictReader – element level
// ============================================================================
//  Mirrors what jmdict2jisho.py takes from each <entry>:
//    k_ele/keb, r_ele/reb, ke_pri/re_pri, sense/{pos,misc,field,gloss}
//  Everything else (ent_seq, xref, ant, s_inf, lsource...) is skipped simply
//  by not reacting to it.
namespace {
// Bytes below 0x20 (TAB, line break...) become spaces. The record format uses '\n'
// and 0x1F as separators, so they must never come from the data – the same rule as
// clean() in tools/jmdict2jisho.py, so both converters stay byte-identical.
std::string clean(std::string s) {
    for (char& c : s)
        if (static_cast<uint8_t>(c) < 0x20) c = ' ';
    return s;
}
}  // namespace

JmdictReader::Result JmdictReader::next(JmEntry& out) {
    out.clear();
    _entryBytes = 0;
    bool inEntry = false;
    std::string text, entity, glossLang;
    while (true) {
        switch (_xml.next()) {
            case XmlPull::Ev::Open: {
                const std::string& n = _xml.name();
                if (!_sawRoot) {
                    if (n != "JMdict") { _error = "not a JMdict file (root element <" + n + ">)"; return Result::Error; }
                    _sawRoot = true;
                } else if (n == "entry") {
                    inEntry = true;
                } else if (n == "sense" && inEntry) {
                    // Empty <sense/> tags cost memory too: count every object.
                    _entryBytes += sizeof(JmSense);
                    if (_entryBytes > kMaxEntryBytes) { _error = "an <entry> larger than 256 KB"; return Result::Error; }
                    out.senses.emplace_back();
                } else if (n == "gloss") {
                    glossLang = _xml.lang();
                }
                text.clear();
                entity.clear();
                break;
            }
            case XmlPull::Ev::Text:
                text = _xml.text();
                entity = _xml.entityName();
                break;
            case XmlPull::Ev::Close: {
                const std::string& n = _xml.name();
                if (!inEntry) break;
                if (n == "entry") return Result::Entry;
                // One entry may hold many strings: cap the total, not just each one.
                _entryBytes += text.size() + entity.size() + sizeof(std::string);
                if (_entryBytes > kMaxEntryBytes) { _error = "an <entry> larger than 256 KB"; return Result::Error; }
                if (n == "keb") out.kanji.push_back(clean(text));
                else if (n == "reb") out.readings.push_back(clean(text));
                else if (n == "ke_pri" || n == "re_pri") out.pri.push_back(clean(text));
                else if (!out.senses.empty()) {
                    JmSense& s = out.senses.back();
                    const std::string& v = entity.empty() ? text : entity;
                    if (n == "pos") s.pos.push_back(clean(v));
                    else if (n == "misc") s.misc.push_back(clean(v));
                    else if (n == "field") s.field.push_back(clean(v));
                    // The full JMdict has glosses in many languages; keep English.
                    else if (n == "gloss" && !text.empty() && (glossLang.empty() || glossLang == "eng"))
                        s.glosses.push_back(clean(text));
                }
                text.clear();
                entity.clear();
                break;
            }
            case XmlPull::Ev::End:
                if (inEntry) { _error = "file ends inside an <entry> (truncated?)"; return Result::Error; }
                if (!_sawRoot) { _error = "no <JMdict> element found"; return Result::Error; }
                return Result::End;
            case XmlPull::Ev::Error:
                return Result::Error;
        }
    }
}

}  // namespace build
