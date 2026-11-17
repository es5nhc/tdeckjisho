#include "dict/BuildIo.hpp"

#include <algorithm>
#include <cstring>

// miniz as bundled with LovyanGFX. Every symbol carries an `lgfx_` prefix, so
// it can never clash with another miniz copy (the ESP32 ROM has one too).
#include <lgfx/utility/lgfx_miniz.h>

namespace build {

namespace {
constexpr size_t kWindow = TINFL_LZ_DICT_SIZE;   // 32768, a power of two
constexpr size_t kInBuf = 16 * 1024;

lgfx_tinfl_decompressor* dec(void* p) { return static_cast<lgfx_tinfl_decompressor*>(p); }
}  // namespace

// ============================================================================
//  GzipSource
// ============================================================================

GzipSource::GzipSource(hal::File& f) : _f(f) {}

GzipSource::~GzipSource() { delete dec(_decomp); }

bool GzipSource::looksLikeGzip(hal::File& f) {
    uint8_t m[2] = {0, 0};
    f.seek(0);
    const bool gz = f.read(m, 2) == 2 && m[0] == 0x1F && m[1] == 0x8B;
    f.seek(0);
    return gz;
}

int GzipSource::byte() {
    if (_backLen) return _back[--_backLen];          // bytes handed back by the decoder
    if (_inPos == _inLen) {
        if (_eof) return -1;
        _inLen = _f.read(_in.data(), _in.size());
        _inPos = 0;
        _filePos += static_cast<uint32_t>(_inLen);
        if (_inLen == 0) { _eof = true; return -1; }
    }
    return _in[_inPos++];
}

bool GzipSource::begin() {
    _in.resize(kInBuf);
    _win.resize(kWindow);
    _decomp = new lgfx_tinfl_decompressor;
    lgfx_tinfl_init(dec(_decomp));

    // ---- the header (RFC 1952, section 2.3) --------------------------------
    //   1F 8B   magic          08  method = DEFLATE     FLG  flags
    //   MTIME (4)  XFL (1)  OS (1)                      then optional parts:
    //   FEXTRA (4): u16 length + data     FNAME (8): original file name, 0-terminated
    //   FCOMMENT (16): 0-terminated       FHCRC (2): u16 header CRC
    uint8_t h[10];
    for (uint8_t& b : h) {
        const int c = byte();
        if (c < 0) { _error = "file too short for gzip"; return false; }
        b = static_cast<uint8_t>(c);
    }
    if (h[0] != 0x1F || h[1] != 0x8B) { _error = "not a gzip file"; return false; }
    if (h[2] != 8) { _error = "unknown gzip compression method"; return false; }
    const uint8_t flg = h[3];
    if (flg & 4) {                                   // FEXTRA
        const int lo = byte(), hi = byte();
        for (int n = lo | (hi << 8); n > 0; --n) byte();
    }
    if (flg & 8) while (byte() > 0) {}               // FNAME: gzip stores "JMdict_e" here
    if (flg & 16) while (byte() > 0) {}              // FCOMMENT
    if (flg & 2) { byte(); byte(); }                 // FHCRC
    if (_eof) { _error = "truncated gzip header"; return false; }
    return true;
}

bool GzipSource::refill() {
    // Keep calling the decoder until it produced something, finished, or failed.
    while (_outLen == 0 && !_done) {
        if (_inPos == _inLen && !_eof) {             // input buffer empty: read more
            _inLen = _f.read(_in.data(), _in.size());
            _inPos = 0;
            _filePos += static_cast<uint32_t>(_inLen);
            if (_inLen == 0) _eof = true;
        }
        size_t inBytes = _inLen - _inPos;             // in: available; out: consumed
        size_t outBytes = kWindow - _winPos;          // in: space;     out: produced
        // No TINFL_FLAG_PARSE_ZLIB_HEADER: gzip has its own header (done above).
        // No TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF: _win is the 32 KB ring.
        const lgfx_tinfl_status st = lgfx_tinfl_decompress(
            dec(_decomp), _in.data() + _inPos, &inBytes, _win.data(), _win.data() + _winPos,
            &outBytes, _eof ? 0 : TINFL_FLAG_HAS_MORE_INPUT);
        _inPos += inBytes;
        _out = _win.data() + _winPos;
        _outLen = outBytes;
        _winPos = (_winPos + outBytes) & (kWindow - 1);   // wrap: & works because 32768 = 2^15
        _crc = static_cast<uint32_t>(lgfx_mz_crc32(_crc, _out, _outLen));
        _size += static_cast<uint32_t>(outBytes);
        if (st < 0) { _error = _eof ? "gzip file is truncated" : "corrupt gzip data"; return false; }
        if (st == TINFL_STATUS_DONE) { _done = true; takeBackLookahead(); }
    }
    return _outLen > 0;
}

// The decoder reads input in whole bytes but uses it bit by bit, so it keeps a
// "bit buffer" of up to 8 bytes it has already taken from us. When the DEFLATE
// stream ends, the whole bytes left in there were never part of it: they are
// the start of the gzip trailer. Newer miniz versions hand them back; this
// one does not, so we take them out of the bit buffer ourselves. (Found the
// hard way: the first version reported "trailer missing" on every real file.)
void GzipSource::takeBackLookahead() {
    lgfx_tinfl_decompressor* d = dec(_decomp);
    lgfx_tinfl_bit_buf_t bits = d->m_bit_buf;
    unsigned n = d->m_num_bits;
    bits >>= (n & 7);                                // drop the last byte's unused bits
    n -= n & 7;
    uint8_t tmp[sizeof bits];
    size_t k = 0;
    for (; n >= 8 && k < sizeof tmp; n -= 8) { tmp[k++] = static_cast<uint8_t>(bits & 0xFF); bits >>= 8; }
    // byte() pops from the END of _back, so store them reversed.
    _backLen = 0;
    while (k) _back[_backLen++] = tmp[--k];
}

size_t GzipSource::read(uint8_t* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        if (_outLen == 0 && !refill()) break;
        const size_t k = std::min(n - got, _outLen);
        std::memcpy(buf + got, _out, k);
        _out += k;
        _outLen -= k;
        got += k;
    }
    return got;
}

bool GzipSource::finish() {
    uint8_t scratch[512];
    while (read(scratch, sizeof scratch) > 0) {}      // the XML parser may stop early
    if (!_error.empty()) return false;
    // ---- the trailer: CRC-32 and ISIZE (size mod 2^32), both little-endian --
    uint8_t t[8];
    for (uint8_t& b : t) {
        const int c = byte();
        if (c < 0) { _error = "gzip trailer missing (truncated download?)"; return false; }
        b = static_cast<uint8_t>(c);
    }
    const uint32_t crc = t[0] | (t[1] << 8) | (t[2] << 16) | (static_cast<uint32_t>(t[3]) << 24);
    const uint32_t size = t[4] | (t[5] << 8) | (t[6] << 16) | (static_cast<uint32_t>(t[7]) << 24);
    if (crc != _crc) { _error = "gzip CRC mismatch: the file is damaged"; return false; }
    if (size != _size) { _error = "gzip length mismatch: the file is damaged"; return false; }
    return true;
}

// ============================================================================
//  BufferedWriter
// ============================================================================

void BufferedWriter::write(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    _pos += static_cast<uint32_t>(n);
    if (_buf.size() + n > _cap) flush();
    if (n >= _cap) {                                  // huge write: skip the buffer
        if (_f.write(b, n) != n) _ok = false;
        return;
    }
    _buf.insert(_buf.end(), b, b + n);
}

bool BufferedWriter::flush() {
    if (!_buf.empty() && _f.write(_buf.data(), _buf.size()) != _buf.size()) _ok = false;
    _buf.clear();
    return _ok;
}

bool BufferedWriter::close() {
    if (!_f.isOpen()) return _ok;
    flush();
    _f.close();
    return _ok;
}

}  // namespace build
