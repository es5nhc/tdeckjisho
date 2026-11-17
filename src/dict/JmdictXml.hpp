#pragma once
// ============================================================================
//  JmdictXml.hpp – read JMdict entries one at a time from a byte stream
// ============================================================================
//
//  Python's converter uses xml.etree.iterparse. There is no XML library on
//  the device, and a general one would be far more than we need: JMdict uses
//  a small, regular subset of XML. So two layers, both hand-written:
//
//    XmlPull       a "pull" tokenizer: each next() returns ONE event
//                  (open tag, close tag, text). Python: the events of
//                  iterparse; Rust: quick-xml's Reader::read_event().
//    JmdictReader  knows JMdict's element names and fills a JmEntry from
//                  those events, one <entry> per call.
//
//  "Pull" means the CALLER asks for the next event, as opposed to SAX-style
//  "push" parsers that call you back. Pulling fits our step()-based builder:
//  stop after any entry, continue next frame, no state hidden in callbacks.
//
//  Entities. JMdict writes part-of-speech tags as entities: <pos>&v1;</pos>,
//  declared in the DTD as <!ENTITY v1 "Ichidan verb">. A real XML parser
//  expands them to "Ichidan verb", which is why the Python converter needs
//  a reverse map. Here we see the raw "&v1;" and simply keep "v1". The DTD
//  is still parsed: text elsewhere gets the description, like in Python.
// ============================================================================

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "dict/BuildIo.hpp"

namespace build {

// SECURITY: the input is a download. Every buffer that grows with it has a cap,
// far above anything the real JMdict needs; past it, the build stops with an error
// instead of eating PSRAM until the ESP32 aborts. (Real JMdict: text nodes of a few
// hundred bytes, ~250 entity definitions, entries of a few KB.)
constexpr size_t kMaxXmlText = 64 * 1024;     // one text node
constexpr size_t kMaxXmlName = 64;            // a tag, attribute or entity name
constexpr size_t kMaxXmlValue = 4 * 1024;     // one attribute or entity value
constexpr size_t kMaxXmlEntities = 1000;      // <!ENTITY> definitions
constexpr size_t kMaxEntryBytes = 256 * 1024; // all text of one <entry>

class XmlPull {
public:
    enum class Ev { Open, Close, Text, End, Error };

    explicit XmlPull(ByteSource& src);
    Ev next();

    const std::string& name() const { return _name; }   // tag name (Open/Close)
    const std::string& text() const { return _text; }   // decoded text (Text)
    // If the text was exactly one DTD entity (&v1;), its NAME ("v1"), else "".
    const std::string& entityName() const { return _entity; }
    // xml:lang of the last Open tag ("" if it had none).
    const std::string& lang() const { return _lang; }
    const std::string& error() const { return _error; }
    // Entities declared in the DTD: name -> description.
    const std::unordered_map<std::string, std::string>& entities() const { return _entities; }

private:
    int get();                              // next byte, -1 at end
    int peek();
    bool skipPast(std::string_view end);    // consume up to and including `end`
    bool readName(std::string& out);
    void parseDoctype();
    void parseAttributes(bool& selfClosing);
    bool decodeEntity(std::string& out, std::string& dtdName);

    ByteSource& _src;
    std::vector<uint8_t> _buf;
    size_t _pos = 0, _len = 0;
    bool _eof = false;

    std::string _name, _text, _entity, _lang, _error;
    bool _pendingClose = false;             // <tag/> = Open now, Close next
    std::unordered_map<std::string, std::string> _entities;
};

// One <entry>, with only the parts the converter uses.
struct JmSense {
    std::vector<std::string> pos, misc, field;   // entity NAMES: "v1", "uk", "comp"
    std::vector<std::string> glosses;            // English only
};
struct JmEntry {
    std::vector<std::string> kanji;      // <keb>
    std::vector<std::string> readings;   // <reb>
    std::vector<std::string> pri;        // every <ke_pri>/<re_pri>: "news1", "nf12"...
    std::vector<JmSense> senses;         // ALL senses (some may have no English gloss)
    void clear() { kanji.clear(); readings.clear(); pri.clear(); senses.clear(); }
};

class JmdictReader {
public:
    explicit JmdictReader(ByteSource& src) : _xml(src) {}
    enum class Result { Entry, End, Error };
    Result next(JmEntry& out);
    const std::string& error() const { return _error.empty() ? _xml.error() : _error; }

private:
    XmlPull _xml;
    bool _sawRoot = false;
    std::string _error;
    size_t _entryBytes = 0;                 // text kept for the current entry (capped)
};

}  // namespace build
