#pragma once
// ============================================================================
//  ExternalSort.hpp – sort more records than fit in RAM
// ============================================================================
//
//  eng.idx has ~880 000 records of 48 bytes = 42 MB. The T-Deck has 8 MB of
//  PSRAM. Python simply calls records.sort(); we cannot. The classic answer
//  (from the days when RAM was small and data lived on tape) is the EXTERNAL
//  MERGE SORT:
//
//   pass 1 – RUNS     fill a RAM buffer, sort it, write it out as a "run";
//                     repeat. Every run is sorted, but not the whole.
//   pass 2 – MERGE    open all runs at once and repeatedly take the smallest
//                     front record among them (a min-heap finds it in
//                     log2(k) steps). The output is fully sorted.
//
//      buffer ─sort─► run 0 ┐
//      buffer ─sort─► run 1 ├─► k-way merge (heap of k fronts) ─► eng.idx
//      buffer ─sort─► run 2 ┘
//
//  Every record is written twice and read twice: 4 × 42 MB of SD traffic,
//  which is why the merge reads in big blocks.
//
//  On the device, all runs go into ONE file, one after the other, and are
//  read back through ONE handle with seek+read. The SD driver allows only a
//  limited number of open files (hal/tdeck/Storage.cpp), and k separate run
//  files would hit that limit.
//
//  ORDER. The records must come out in exactly the order Python's
//      records.sort(key=lambda r: (r[0], -r[1]))
//  produces: key bytes ascending, score descending, and – because Python's
//  sort is STABLE and records were appended in file order – the entry
//  offset ascending as the final tie-break. recordLess() says exactly that.
// ============================================================================

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dict/BuildIo.hpp"
#include "dict/JmdictEncode.hpp"

namespace build {

bool recordLess(const uint8_t* a, const uint8_t* b);

// Pass 1: collects records, writes sorted runs to one file.
class RunWriter {
public:
    // bufferBytes: RAM for one run (a multiple of kRecordSize is used).
    bool open(const std::string& path, size_t bufferBytes);
    bool add(const uint8_t* rec);                  // may sort+write a run (slow call)
    bool finish();                                 // write the last run, close
    void abort();                                  // close without writing (on failure)
    const std::vector<uint32_t>& runs() const { return _runs; }   // records per run
    uint32_t total() const { return _total; }
    uint32_t sortMs() const { return _sortMs; }    // time spent in std::sort, for the log

private:
    bool flushRun();
    BufferedWriter _out{32 * 1024};
    std::unique_ptr<uint8_t[]> _buf;
    size_t _cap = 0, _n = 0;                        // capacity / used, in records
    std::vector<uint32_t> _runs;
    uint32_t _total = 0, _sortMs = 0;
};

// Pass 2: merges the runs into a finished .idx file, a slice per step().
class RunMerger {
public:
    bool open(const std::string& runsPath, const std::vector<uint32_t>& runs,
              const std::string& outPath);
    // Merge for about budgetMs. Returns false on error; check done().
    bool step(uint32_t budgetMs);
    void close();                                  // release files and buffers
    bool done() const { return _done; }
    uint32_t written() const { return _written; }
    uint32_t total() const { return _total; }
    const std::string& error() const { return _error; }

private:
    struct Run {
        uint32_t next = 0;                // file offset of the next unread record
        uint32_t left = 0;                // records not yet read from the file
        std::vector<uint8_t> buf;         // block read from the file
        size_t pos = 0, len = 0;          // current record / bytes in buf
        const uint8_t* front() const { return buf.data() + pos; }
    };
    bool refill(Run& r);
    bool finishFile();

    hal::File _in;
    BufferedWriter _out{32 * 1024};
    std::vector<Run> _runs;
    std::vector<int> _heap;               // indices into _runs, a min-heap by front()
    std::string _sparse;                  // every 256th key: the sparse index
    uint32_t _written = 0, _total = 0;
    bool _done = false;
    std::string _error;
};

}  // namespace build
