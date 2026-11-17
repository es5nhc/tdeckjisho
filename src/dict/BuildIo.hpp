#pragma once
// ============================================================================
//  BuildIo.hpp – streaming input and buffered output for the dictionary builder
// ============================================================================
//
//  The on-device builder (dict/DictBuilder.hpp) reads a 60 MB XML file that
//  is usually gzip-compressed, on a machine with 8 MB of PSRAM. So nothing is
//  ever loaded whole: bytes flow through small buffers, front to back.
//
//      SD card ──► FileSource ──► (GzipSource) ──► XmlPull ──► JmEntry
//                  raw bytes       inflated XML     tags/text   one entry
//
//  ByteSource is the interface the XML parser reads from. FileSource and
//  GzipSource both implement it, so the parser does not know or care whether
//  the user copied JMdict_e or JMdict_e.gz to the card.
//  (Python: a file object vs gzip.open(); Rust: impl Read for both.)
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "hal/Hal.hpp"

namespace build {

// Anything that can hand out bytes in order.
class ByteSource {
public:
    virtual ~ByteSource() = default;
    // Up to n bytes into buf. 0 = end of data OR an error (check error()).
    virtual size_t read(uint8_t* buf, size_t n) = 0;
    // How far into the FILE on the card we are: for the progress bar. For a
    // gzip file this is the compressed position, which is what grows evenly.
    virtual uint32_t filePosition() const = 0;
    const std::string& error() const { return _error; }

protected:
    std::string _error;
};

// A plain (uncompressed) file.
class FileSource : public ByteSource {
public:
    explicit FileSource(hal::File& f) : _f(f) {}
    size_t read(uint8_t* buf, size_t n) override {
        const size_t got = _f.read(buf, n);
        _pos += static_cast<uint32_t>(got);
        return got;
    }
    uint32_t filePosition() const override { return _pos; }

private:
    hal::File& _f;
    uint32_t _pos = 0;
};

// A gzip file (RFC 1952), inflated on the fly.
//
// gzip = a 10+ byte header, then DEFLATE data (RFC 1951), then an 8-byte
// trailer: CRC-32 and length of the uncompressed data. The DEFLATE part is
// decoded by "tinfl", the decompressor from the miniz library, which
// LovyanGFX already contains (its PNG decoder needs it) – compiled for both
// the ESP32 and the PC, so we did not have to add a library.
//
// DEFLATE refers back up to 32 KB into the data already produced ("copy the
// 12 bytes that appeared 3000 bytes ago"), so the decoder writes into a
// 32 KB RING BUFFER (the "window"), and we hand out bytes from there.
class GzipSource : public ByteSource {
public:
    explicit GzipSource(hal::File& f);
    ~GzipSource() override;

    bool begin();                                 // parse the header
    size_t read(uint8_t* buf, size_t n) override;
    uint32_t filePosition() const override { return _filePos - static_cast<uint32_t>(_inLen - _inPos); }
    // Read whatever is left and compare the trailer's CRC-32 and length with
    // what we actually produced. This is the free integrity check: a cut-off
    // download or a flipped bit on the card is found here.
    bool finish();

    // `true` if the file starts with the gzip magic bytes 1F 8B.
    static bool looksLikeGzip(hal::File& f);

private:
    bool refill();          // decode the next piece into the window
    int byte();             // next raw (compressed) byte, -1 at end of file
    void takeBackLookahead();

    hal::File& _f;
    void* _decomp = nullptr;          // lgfx_tinfl_decompressor (~11 KB), opaque here
    std::vector<uint8_t> _in;         // compressed input buffer
    std::vector<uint8_t> _win;        // 32 KB output ring
    size_t _inPos = 0, _inLen = 0;
    size_t _winPos = 0;               // where the decoder writes next
    const uint8_t* _out = nullptr;    // decoded bytes not yet handed out...
    size_t _outLen = 0;               // ...and how many
    uint32_t _filePos = 0;            // bytes read from the file so far
    uint32_t _crc = 0, _size = 0;
    bool _eof = false, _done = false;
    uint8_t _back[8];                 // input bytes the decoder read too far
    size_t _backLen = 0;
};

// Collects small writes into one big buffer: "write 48 bytes" a million times
// would mean a million trips through the SD driver. With a 16 KB buffer it is
// ~3000 trips. pos() is the logical file position (Python: f.tell()).
class BufferedWriter {
public:
    explicit BufferedWriter(size_t bufSize = 16 * 1024) { _buf.reserve(bufSize); _cap = bufSize; }
    ~BufferedWriter() { close(); }

    bool open(const std::string& path) { _pos = 0; _ok = _f.open(path); _buf.clear(); return _ok; }
    void write(const void* p, size_t n);
    void put(uint8_t b) { write(&b, 1); }
    bool flush();
    bool close();                     // flush + close; returns "everything OK?"
    uint32_t pos() const { return _pos; }
    bool ok() const { return _ok; }

private:
    hal::WFile _f;
    std::vector<uint8_t> _buf;
    size_t _cap;
    uint32_t _pos = 0;
    bool _ok = false;
};

// Little-endian helpers, byte by byte (see docs/02-dictionary.md for why not memcpy).
inline void putLe16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
inline void putLe32(uint8_t* p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = v >> 24;
}

}  // namespace build
