#pragma once
// ============================================================================
//  DictBuilder.hpp – build the dictionary ON THE DEVICE from raw JMdict
// ============================================================================
//
//  The same job as tools/jmdict2jisho.py, with the same output files, but
//  running on the ESP32 with 8 MB of PSRAM, while the UI keeps working:
//
//    Parse      JMdict(.gz) ─► entries.dat + kana.runs + eng.runs   (one pass)
//    MergeKana  kana.runs   ─► kana.idx                            (k-way merge)
//    MergeKanji kanji.runs  ─► kanji.idx
//    MergeEng   eng.runs    ─► eng.idx
//    Finish     info.txt, then swap the new folder in
//
//  STEP-BASED. Nothing here blocks for long: the screen calls step(30) every
//  frame and the builder does ~30 ms of work, then returns. That's the same
//  start-then-poll shape as hal::wifiScanStart/Poll and hal::Download. A
//  second FreeRTOS task would also work on the device, but not in the SDL
//  simulator, and the SD card shares its SPI bus with the display: with one
//  thread, the two can never get in each other's way.
//  (One exception: sorting a full run buffer takes a second or so. The UI
//  freezes for that moment. Exercise: do it on the other core.)
//
//  CRASH SAFETY. Everything is written into /tdeckjisho/jisho.new/. info.txt
//  is written LAST: it is the "commit marker". Only then are the folders
//  swapped:   jisho -> jisho.old,   jisho.new -> jisho,   delete jisho.old.
//  recoverSwap() (called at boot) finishes or undoes a swap that a power cut
//  interrupted. The old dictionary stays usable until the new one is complete.
// ============================================================================

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/Paths.hpp"
#include "dict/BuildIo.hpp"
#include "dict/ExternalSort.hpp"
#include "dict/JmdictEncode.hpp"
#include "dict/JmdictXml.hpp"

namespace build {

// Where the "Update dictionary" screen downloads from (settings key "jmdict_url"
// overrides it, e.g. http://192.168.1.10:8000/JMdict_e.gz for a local server).
// https: the dictionary is input to a parser, so it must come from the real EDRDG.
// (Their ftp.edrdg.org host fails certificate checks; www.edrdg.org serves the same file.)
inline const char* const kDefaultJmdictUrl = "https://www.edrdg.org/pub/Nihongo/JMdict_e.gz";

class DictBuilder {
public:
    enum class Phase { Idle, Parse, MergeKana, MergeKanji, MergeEng, Finish, Done, Failed };

    struct Options {
        std::string source;                       // e.g. /tdeckjisho/jmdict/JMdict_e.gz
        std::string workDir = paths::kDictNew;
        std::string finalDir = paths::kDict;
        std::string oldDir = paths::kDictOld;
        bool swap = true;                         // false: leave the result in workDir (tests)
        size_t ramBytes = 4 * 1024 * 1024;        // for the two run buffers together
    };

    ~DictBuilder() { closeAll(); }

    // Checks the source and the free space, opens everything. On false, see error().
    bool start(const Options& opt);
    // Work for about budgetMs. Call while phase() is Parse..Finish.
    void step(uint32_t budgetMs);
    // Stop and delete the half-built folder.
    void cancel();

    Phase phase() const { return _phase; }
    bool running() const { return _phase != Phase::Idle && _phase != Phase::Done && _phase != Phase::Failed; }
    const char* phaseName() const;
    float progress() const;                       // 0..1, all phases together
    const std::string& error() const { return _error; }
    uint32_t entries() const { return _entries; }
    uint32_t elapsedMs() const;
    std::string report() const;                   // timings per phase, for the log / screen

    // Call once at boot, before opening the dictionary.
    static void recoverSwap();
    // Candidate source files in /tdeckjisho/jmdict/.
    static std::vector<hal::DirEntry> findSources();

private:
    void fail(const std::string& why);
    void stepParse(uint32_t budgetMs);
    void endParse();
    bool writeInfo();
    bool swapIn();
    void closeAll();

    Options _opt;
    Phase _phase = Phase::Idle;
    std::string _error;

    hal::File _srcFile;
    uint32_t _srcSize = 0;
    std::unique_ptr<ByteSource> _src;
    GzipSource* _gz = nullptr;                    // == _src when the source is gzip
    std::unique_ptr<JmdictReader> _reader;
    JmEntry _entry;
    std::string _payload;
    std::vector<KeyScore> _kanaKeys, _kanjiKeys, _engKeys;

    BufferedWriter _dat{32 * 1024};
    RunWriter _kanaRuns, _kanjiRuns, _engRuns;
    RunMerger _merger;

    uint32_t _entries = 0;
    uint32_t _tStart = 0, _tPhase = 0;
    uint32_t _msParse = 0, _msKana = 0, _msKanji = 0, _msEng = 0, _msFinish = 0;
    uint32_t _tEnd = 0;
};

}  // namespace build
