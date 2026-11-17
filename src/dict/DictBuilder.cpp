#include "dict/DictBuilder.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <new>

#include "core/I18n.hpp"
#include "core/Str.hpp"

namespace build {

namespace {
// Share of the total progress bar per phase (measured on the full JMdict_e:
// parsing dominates; the English merge moves 3.4x the data of the kana one).
constexpr float kParseEnd = 0.72f, kKanaEnd = 0.78f, kKanjiEnd = 0.80f, kEngEnd = 0.98f;
}  // namespace

// ============================================================================
//  Start
// ============================================================================

bool DictBuilder::start(const Options& opt) {
    closeAll();
    _opt = opt;
    _error.clear();
    _entries = 0;
    _msParse = _msKana = _msKanji = _msEng = _msFinish = 0;
    _tStart = _tPhase = hal::millis();
    _phase = Phase::Failed;                          // until everything is open

    if (!_srcFile.open(opt.source)) { fail("cannot open " + opt.source); return false; }
    _srcSize = _srcFile.size();

    // Plain XML or gzip? Look at the first bytes, NOT the file name: users
    // rename files, and some web servers unpack .gz on the fly.
    if (GzipSource::looksLikeGzip(_srcFile)) {
        auto gz = std::make_unique<GzipSource>(_srcFile);
        if (!gz->begin()) { fail(gz->error()); return false; }
        _gz = gz.get();
        _src = std::move(gz);
    } else {
        _src = std::make_unique<FileSource>(_srcFile);
    }

    // Free space. JMdict_e: 61 MB XML (10 MB gzipped) -> 72 MB of output, plus
    // up to 42 MB of run files at the worst moment. "2x the XML" covers that.
    const uint64_t xml = static_cast<uint64_t>(_srcSize) * (_gz ? 6 : 1);
    const uint64_t free = hal::freeBytes();
    if (free && free < 2 * xml) {
        fail("not enough space on the SD card: need about " + str::humanSize(static_cast<uint32_t>(2 * xml)) +
             ", free " + str::humanSize(static_cast<uint32_t>(std::min<uint64_t>(free, 0xFFFFFFFFu))));
        return false;
    }

    // A fresh work folder (a previous attempt may have left one behind).
    hal::removeTree(opt.workDir);
    if (!hal::makeDirs(opt.workDir)) { fail("cannot create " + opt.workDir); return false; }
    if (!_dat.open(opt.workDir + "/entries.dat")) { fail("cannot create entries.dat"); return false; }

    // RAM for the run buffers, in the ratio of their record counts (kana 1/5,
    // kanji 1/10, English the rest). If PSRAM is short, try smaller: more, shorter
    // runs are slower to merge but still correct.
    for (size_t ram = opt.ramBytes; ; ram /= 2) {
        if (ram < 256 * 1024) { fail("not enough RAM for sorting"); return false; }
        if (_kanaRuns.open(opt.workDir + "/kana.runs", ram / 5) &&
            _kanjiRuns.open(opt.workDir + "/kanji.runs", ram / 10) &&
            _engRuns.open(opt.workDir + "/eng.runs", ram - ram / 5 - ram / 10)) {
            hal::log("[build] sort buffers: %u KB\n", static_cast<unsigned>(ram / 1024));
            break;
        }
        _kanaRuns.abort();
        _kanjiRuns.abort();
        _engRuns.abort();
    }

    _reader = std::make_unique<JmdictReader>(*_src);
    _phase = Phase::Parse;
    hal::log("[build] %s (%s, %s) -> %s\n", opt.source.c_str(), _gz ? "gzip" : "xml",
             str::humanSize(_srcSize).c_str(), opt.workDir.c_str());
    return true;
}

// ============================================================================
//  Steps
// ============================================================================

void DictBuilder::step(uint32_t budgetMs) {
    switch (_phase) {
        case Phase::Parse:
            stepParse(budgetMs);
            break;
        case Phase::MergeKana:
        case Phase::MergeKanji:
        case Phase::MergeEng: {
            if (!_merger.step(budgetMs)) { fail(_merger.error()); return; }
            if (!_merger.done()) return;
            const uint32_t ms = hal::millis() - _tPhase;
            _tPhase = hal::millis();
            if (_phase == Phase::MergeKana) {
                _msKana = ms;
                hal::removeFile(_opt.workDir + "/kana.runs");      // free the space early
                if (!_merger.open(_opt.workDir + "/kanji.runs", _kanjiRuns.runs(), _opt.workDir + "/kanji.idx")) {
                    fail(_merger.error());
                    return;
                }
                _phase = Phase::MergeKanji;
            } else if (_phase == Phase::MergeKanji) {
                _msKanji = ms;
                hal::removeFile(_opt.workDir + "/kanji.runs");
                if (!_merger.open(_opt.workDir + "/eng.runs", _engRuns.runs(), _opt.workDir + "/eng.idx")) {
                    fail(_merger.error());
                    return;
                }
                _phase = Phase::MergeEng;
            } else {
                _msEng = ms;
                hal::removeFile(_opt.workDir + "/eng.runs");
                _phase = Phase::Finish;
            }
            return;
        }
        case Phase::Finish: {
            if (!writeInfo()) { fail("cannot write info.txt"); return; }
            if (_opt.swap && !swapIn()) return;                   // swapIn() set the error
            _msFinish = hal::millis() - _tPhase;
            _tEnd = hal::millis();
            _phase = Phase::Done;
            hal::log("[build] done:\n%s\n", report().c_str());
            return;
        }
        default:
            return;
    }
}

void DictBuilder::stepParse(uint32_t budgetMs) {
    const uint32_t t0 = hal::millis();
    uint8_t rec[kRecordSize];
    while (hal::millis() - t0 < budgetMs) {
        const JmdictReader::Result r = _reader->next(_entry);
        if (r == JmdictReader::Result::Error) { fail(_reader->error()); return; }
        if (r == JmdictReader::Result::End) { endParse(); return; }
        if (!encodeEntry(_entry, _payload, _kanaKeys, _kanjiKeys, _engKeys)) continue;

        // entries.dat: u16 length + payload. The offset is what the index stores.
        const uint32_t offset = _dat.pos();
        uint8_t len[2];
        putLe16(len, static_cast<uint16_t>(_payload.size()));
        _dat.write(len, 2);
        _dat.write(_payload.data(), _payload.size());

        for (const KeyScore& k : _kanaKeys) {
            makeRecord(rec, k.key, k.score, offset);
            if (!_kanaRuns.add(rec)) { fail("write error in kana.runs (SD card full?)"); return; }
        }
        for (const KeyScore& k : _kanjiKeys) {
            makeRecord(rec, k.key, k.score, offset);
            if (!_kanjiRuns.add(rec)) { fail("write error in kanji.runs (SD card full?)"); return; }
        }
        for (const KeyScore& k : _engKeys) {
            makeRecord(rec, k.key, k.score, offset);
            if (!_engRuns.add(rec)) { fail("write error in eng.runs (SD card full?)"); return; }
        }
        if (++_entries % 20000 == 0)
            hal::log("[build] %u entries, %u s\n", static_cast<unsigned>(_entries),
                     static_cast<unsigned>((hal::millis() - _tStart) / 1000));
    }
    if (!_dat.ok()) fail("write error in entries.dat (SD card full?)");
}

void DictBuilder::endParse() {
    if (_gz && !_gz->finish()) { fail(_gz->error()); return; }   // CRC + length check
    if (!_dat.close()) { fail("write error in entries.dat"); return; }
    if (!_kanaRuns.finish() || !_kanjiRuns.finish() || !_engRuns.finish()) { fail("write error in run files"); return; }
    _reader.reset();
    _src.reset();
    _gz = nullptr;
    _srcFile.close();
    if (_entries == 0) { fail("no entries found"); return; }

    _msParse = hal::millis() - _tPhase;
    _tPhase = hal::millis();
    hal::log("[build] parsed %u entries in %u ms (sorting %u ms); runs: kana %u, eng %u\n",
             static_cast<unsigned>(_entries), static_cast<unsigned>(_msParse),
             static_cast<unsigned>(_kanaRuns.sortMs() + _kanjiRuns.sortMs() + _engRuns.sortMs()),
             static_cast<unsigned>(_kanaRuns.runs().size()), static_cast<unsigned>(_engRuns.runs().size()));
    if (!_merger.open(_opt.workDir + "/kana.runs", _kanaRuns.runs(), _opt.workDir + "/kana.idx")) {
        fail(_merger.error());
        return;
    }
    _phase = Phase::MergeKana;
}

// ============================================================================
//  Finish: info.txt, then the folder swap
// ============================================================================

bool DictBuilder::writeInfo() {
    std::string built = "unknown (clock not set)";
    if (hal::timeValid()) {
        char buf[16];
        const std::time_t now = std::time(nullptr);
        std::strftime(buf, sizeof buf, "%Y-%m-%d", std::localtime(&now));
        built = buf;
    }
    const std::string name = _opt.source.substr(_opt.source.rfind('/') + 1);
    // Same fields as the Python converter writes, plus who built it.
    const std::string info =
        "source=" + name + "\n" +
        "built=" + built + "\n" +
        "entries=" + std::to_string(_entries) + "\n" +
        "kana_keys=" + std::to_string(_kanaRuns.total()) + "\n" +
        "kanji_keys=" + std::to_string(_kanjiRuns.total()) + "\n" +
        "eng_keys=" + std::to_string(_engRuns.total()) + "\n" +
        "converter=TDeckJisho on-device builder\n"
        "licence=JMdict (c) Electronic Dictionary Research and Development Group, "
        "CC BY-SA 4.0, https://www.edrdg.org/edrdg/licence.html\n";
    return hal::writeFile(_opt.workDir + "/info.txt", info);   // the commit marker
}

bool DictBuilder::swapIn() {
    // The Dictionary must have closed its files before this point (the
    // screen does that): FAT cannot rename a folder with open files in it.
    if (hal::exists(_opt.oldDir)) hal::removeTree(_opt.oldDir);
    if (hal::exists(_opt.finalDir) && !hal::renamePath(_opt.finalDir, _opt.oldDir)) {
        fail("cannot rename " + _opt.finalDir);
        return false;
    }
    if (!hal::renamePath(_opt.workDir, _opt.finalDir)) {
        hal::renamePath(_opt.oldDir, _opt.finalDir);             // put the old one back
        fail("cannot rename " + _opt.workDir);
        return false;
    }
    hal::removeTree(_opt.oldDir);
    return true;
}

void DictBuilder::recoverSwap() {
    const std::string& fin = paths::kDict;
    const std::string& work = paths::kDictNew;
    const std::string& old = paths::kDictOld;
    // Power cut between "jisho -> jisho.old" and "jisho.new -> jisho":
    if (!hal::exists(fin) && hal::exists(work + "/info.txt")) {
        hal::log("[build] finishing an interrupted dictionary swap\n");
        hal::renamePath(work, fin);
    }
    // The new one never arrived: bring the old one back.
    if (!hal::exists(fin) && hal::exists(old)) hal::renamePath(old, fin);
    // Leftovers.
    if (hal::exists(fin) && hal::exists(old)) hal::removeTree(old);
    if (hal::exists(work) && !hal::exists(work + "/info.txt")) {
        hal::log("[build] removing an unfinished build\n");
        hal::removeTree(work);
    }
}

std::vector<hal::DirEntry> DictBuilder::findSources() {
    std::vector<hal::DirEntry> all, out;
    hal::listDir(paths::kJmdict, all);
    for (auto& e : all) {
        if (e.isDir || e.size < 1024) continue;
        if (str::endsWith(e.name, ".part") || str::endsWith(e.name, ".tmp")) continue;
        out.push_back(std::move(e));
    }
    return out;
}

// ============================================================================
//  Bookkeeping
// ============================================================================

void DictBuilder::fail(const std::string& why) {
    _error = why;
    _phase = Phase::Failed;
    _tEnd = hal::millis();
    hal::log("[build] FAILED: %s\n", why.c_str());
    closeAll();
    hal::removeTree(_opt.workDir);                // never leave half a dictionary behind
}

void DictBuilder::cancel() {
    if (!running()) return;
    fail("cancelled");
}

void DictBuilder::closeAll() {
    // Order matters: the parser reads from _src, which reads from _srcFile.
    _reader.reset();
    _src.reset();
    _gz = nullptr;
    _srcFile.close();
    _dat.close();
    _kanaRuns.abort();
    _kanjiRuns.abort();
    _engRuns.abort();
    _merger.close();
}

const char* DictBuilder::phaseName() const {
    switch (_phase) {
        // Shown on the Update screen, so they are translated (core/Strings.def).
        case Phase::Idle:      return i18n::tr(i18n::Str::PhaseIdle);
        case Phase::Parse:     return i18n::tr(_gz ? i18n::Str::PhaseParseGz : i18n::Str::PhaseParse);
        case Phase::MergeKana: return i18n::tr(i18n::Str::PhaseKana);
        case Phase::MergeKanji: return i18n::tr(i18n::Str::PhaseKanji);
        case Phase::MergeEng:  return i18n::tr(i18n::Str::PhaseEng);
        case Phase::Finish:    return i18n::tr(i18n::Str::PhaseFinish);
        case Phase::Done:      return i18n::tr(i18n::Str::PhaseDone);
        case Phase::Failed:    return i18n::tr(i18n::Str::PhaseFailed);
    }
    return "";
}

float DictBuilder::progress() const {
    auto frac = [](uint32_t a, uint32_t b) { return b ? static_cast<float>(a) / b : 0.f; };
    switch (_phase) {
        case Phase::Parse:
            return kParseEnd * frac(_src ? _src->filePosition() : 0, _srcSize);
        case Phase::MergeKana:
            return kParseEnd + (kKanaEnd - kParseEnd) * frac(_merger.written(), _merger.total());
        case Phase::MergeKanji:
            return kKanaEnd + (kKanjiEnd - kKanaEnd) * frac(_merger.written(), _merger.total());
        case Phase::MergeEng:
            return kKanjiEnd + (kEngEnd - kKanjiEnd) * frac(_merger.written(), _merger.total());
        case Phase::Finish:
        case Phase::Done:
            return _phase == Phase::Done ? 1.f : kEngEnd;
        default:
            return 0.f;
    }
}

uint32_t DictBuilder::elapsedMs() const {
    return (running() ? hal::millis() : _tEnd) - _tStart;
}

std::string DictBuilder::report() const {
    // Several short lines: fits the 320 px screen and reads well in the log.
    char buf[240];
    std::snprintf(buf, sizeof buf,
                  "%u entries in %u:%02u\n"
                  "parse %u s (of which sorting %u s)\n"
                  "kana index %u s · English index %u s",
                  static_cast<unsigned>(_entries), static_cast<unsigned>(elapsedMs() / 60000),
                  static_cast<unsigned>(elapsedMs() / 1000 % 60), static_cast<unsigned>(_msParse / 1000),
                  static_cast<unsigned>((_kanaRuns.sortMs() + _kanjiRuns.sortMs() + _engRuns.sortMs()) / 1000),
                  static_cast<unsigned>((_msKana + _msKanji) / 1000), static_cast<unsigned>(_msEng / 1000));
    return buf;
}

}  // namespace build
