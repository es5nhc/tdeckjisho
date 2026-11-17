// ============================================================================
//  tests/bench_dict.cpp – how fast is a dictionary search, and WHY?
// ============================================================================
//
//    tests/bench.sh                      # PC disk speed + I/O counts
//    JISHO_SD_LATENCY_US=1000 tests/bench.sh   # pretend each read costs 1 ms
//
//  The PC's SSD (and the OS file cache) make every read nearly free, so raw
//  PC timings say little about the T-Deck. The I/O COUNTS do: they are the
//  same on both machines. With the latency emulation, "ms" approximates what
//  you'll see on the device. Then confirm with the [search] log lines on the
//  real hardware – measuring beats guessing.
// ============================================================================

#include <chrono>
#include <cstdarg>
#include <cstdio>

#include "dict/Dictionary.hpp"
#include "hal/Hal.hpp"

// hal::log() and friends: tests/hal_stub.cpp

namespace {

double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

template <class Fn>   // a template: works with any callable (here: lambdas)
void bench(const char* label, Fn search) {
    hal::resetIoStats();
    const double t0 = nowMs();
    const size_t hits = search();
    const double ms = nowMs() - t0;
    const hal::IoStats s = hal::ioStats();
    std::printf("  %-22s %3zu hits %8.1f ms %6u reads %6u seeks %8.1f KB\n", label, hits, ms,
                s.reads, s.seeks, s.bytes / 1024.0);
}

}  // namespace

int main() {
    Dictionary dict;
    hal::resetIoStats();
    const double t0 = nowMs();
    if (!dict.open("/tdeckjisho/jisho")) {
        std::printf("cannot open dictionary: %s\n", dict.error().c_str());
        return 1;
    }
    const hal::IoStats o = hal::ioStats();
    std::printf("open: %.1f ms, %u reads, %.1f KB\n\n", nowMs() - t0, o.reads, o.bytes / 1024.0);

    // describeFirst = 8: what the search screen does (lazy loading).
    std::printf("kana (romaji already converted), as the search screen does it:\n");
    for (const char* q : {"か", "たべ", "たべる", "にほんご", "しんぶん", "こーひー", "きょう"})
        bench(q, [&] { return dict.searchKana(q, 40, 8).size(); });

    std::printf("english:\n");
    for (const char* q : {"e", "eat", "water", "time", "ice cream", "good morning"})
        bench(q, [&] { return dict.searchEnglish(q, 40, 8).size(); });

    std::printf("for comparison, describing all 40 hits eagerly:\n");
    bench("たべ (eager)", [&] { return dict.searchKana("たべ", 40).size(); });
    bench("water (eager)", [&] { return dict.searchEnglish("water", 40).size(); });

    // What the user sees for "taberu": the top 3 results.
    std::printf("\nsanity check, top results:\n");
    for (const char* q : {"たべる", "にほん"})
        for (auto& h : dict.searchKana(q, 3)) std::printf("  %s  %s【%s】 %s\n", q, h.headword.c_str(), h.reading.c_str(), h.gloss.c_str());
    for (const char* q : {"eat", "water", "ice cream"})
        for (auto& h : dict.searchEnglish(q, 3)) std::printf("  %s  %s【%s】 %s\n", q, h.headword.c_str(), h.reading.c_str(), h.gloss.c_str());
    return 0;
}
