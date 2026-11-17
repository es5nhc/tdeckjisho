#include "dict/ExternalSort.hpp"

#include <algorithm>
#include <cstring>
#include <new>          // std::nothrow

namespace build {

namespace {
constexpr size_t kRunReadBytes = 8 * 1024;     // per run, during the merge
constexpr uint32_t kHeaderSize = 16;

uint32_t le32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// One record as a type, so std::sort can swap whole records.
struct Rec { uint8_t b[kRecordSize]; };
}  // namespace

bool recordLess(const uint8_t* a, const uint8_t* b) {
    // Zero-padded keys: memcmp over all 43 bytes gives the same order as
    // comparing the unpadded byte strings ("eat" < "eats" because 0 < 's').
    if (const int c = std::memcmp(a, b, kKeySize)) return c < 0;
    if (a[kKeySize] != b[kKeySize]) return a[kKeySize] > b[kKeySize];   // score DEscending
    return le32(a + kKeySize + 1) < le32(b + kKeySize + 1);              // offset: stable order
}

// ============================================================================
//  RunWriter
// ============================================================================

bool RunWriter::open(const std::string& path, size_t bufferBytes) {
    _cap = bufferBytes / kRecordSize;
    // new (std::nothrow) returns nullptr instead of throwing when memory is
    // short. On the ESP32 a block this big comes from PSRAM automatically.
    _buf.reset(new (std::nothrow) uint8_t[_cap * kRecordSize]);
    if (!_buf) return false;
    _n = 0;
    _total = 0;
    _runs.clear();
    return _out.open(path);
}

bool RunWriter::add(const uint8_t* rec) {
    std::memcpy(_buf.get() + _n * kRecordSize, rec, kRecordSize);
    ++_total;
    if (++_n == _cap) return flushRun();
    return true;
}

bool RunWriter::flushRun() {
    if (_n == 0) return true;
    const uint32_t t0 = hal::millis();
    Rec* recs = reinterpret_cast<Rec*>(_buf.get());
    std::sort(recs, recs + _n, [](const Rec& a, const Rec& b) { return recordLess(a.b, b.b); });
    _sortMs += hal::millis() - t0;
    _out.write(_buf.get(), _n * kRecordSize);
    _runs.push_back(static_cast<uint32_t>(_n));
    _n = 0;
    return _out.ok();
}

bool RunWriter::finish() {
    const bool ok = flushRun() && _out.close();
    _buf.reset();                                   // give the RAM back at once
    return ok;
}

void RunWriter::abort() {
    _buf.reset();
    _n = 0;
    _out.close();
}

// ============================================================================
//  RunMerger
// ============================================================================

void RunMerger::close() {
    _in.close();
    _out.close();
    _runs.clear();
    _heap.clear();
    _sparse.clear();
}

bool RunMerger::open(const std::string& runsPath, const std::vector<uint32_t>& runs,
                     const std::string& outPath) {
    _error.clear();
    _done = false;
    _written = 0;
    _total = 0;
    _sparse.clear();
    if (!_in.open(runsPath)) { _error = "cannot open " + runsPath; return false; }
    if (!_out.open(outPath)) { _error = "cannot create " + outPath; return false; }

    // Where each run starts in the runs file.
    _runs.assign(runs.size(), Run{});
    uint32_t offset = 0;
    for (size_t i = 0; i < runs.size(); ++i) {
        _runs[i].next = offset;
        _runs[i].left = runs[i];
        offset += runs[i] * kRecordSize;
        _total += runs[i];
    }

    // The header. The record count is known before merging: it is the sum
    // of the runs, so no seeking back to patch it later.
    uint8_t h[kHeaderSize];
    std::memcpy(h, "JIDX", 4);
    putLe16(h + 4, kIndexVersion);
    putLe16(h + 6, kRecordSize);
    putLe16(h + 8, kKeySize);
    putLe16(h + 10, kBlockRecords);
    putLe32(h + 12, _total);
    _out.write(h, sizeof h);

    // Fill every run's buffer and build the heap.
    _heap.clear();
    for (size_t i = 0; i < _runs.size(); ++i) {
        if (!refill(_runs[i])) return false;
        if (_runs[i].len) _heap.push_back(static_cast<int>(i));
    }
    // std::make_heap builds a MAX-heap by the comparator. "a after b" as the
    // comparator turns it into a MIN-heap: the smallest record on top.
    std::make_heap(_heap.begin(), _heap.end(), [this](int a, int b) {
        return recordLess(_runs[b].front(), _runs[a].front());
    });
    return true;
}

bool RunMerger::refill(Run& r) {
    r.pos = 0;
    r.len = 0;
    if (r.left == 0) return true;
    const uint32_t n = std::min<uint32_t>(r.left, kRunReadBytes / kRecordSize);
    r.buf.resize(n * kRecordSize);
    // All runs share ONE file handle, so every refill seeks first.
    if (!_in.seek(r.next) || _in.read(r.buf.data(), r.buf.size()) != r.buf.size()) {
        _error = "read error in run file";
        return false;
    }
    r.next += n * kRecordSize;
    r.left -= n;
    r.len = r.buf.size();
    return true;
}

bool RunMerger::step(uint32_t budgetMs) {
    if (_done) return true;
    auto after = [this](int a, int b) { return recordLess(_runs[b].front(), _runs[a].front()); };
    const uint32_t t0 = hal::millis();
    // Check the clock only every 1024 records: millis() is cheap, not free.
    while (!_heap.empty()) {
        if ((_written & 1023) == 0 && hal::millis() - t0 >= budgetMs) return true;
        std::pop_heap(_heap.begin(), _heap.end(), after);   // smallest -> back()
        Run& r = _runs[_heap.back()];
        if (_written % kBlockRecords == 0)                    // first key of a block
            _sparse.append(reinterpret_cast<const char*>(r.front()), kKeySize);
        _out.write(r.front(), kRecordSize);
        ++_written;
        r.pos += kRecordSize;
        if (r.pos == r.len && !refill(r)) return false;
        if (r.len == 0) _heap.pop_back();                     // this run is used up
        else std::push_heap(_heap.begin(), _heap.end(), after);
    }
    return finishFile();
}

bool RunMerger::finishFile() {
    _out.write(_sparse.data(), _sparse.size());   // level 1 of the two-level index
    _in.close();
    if (!_out.close()) { _error = "write error (SD card full?)"; return false; }
    if (_written != _total) { _error = "merge lost records"; return false; }
    _done = true;
    return true;
}

}  // namespace build
