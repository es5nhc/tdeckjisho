#pragma once
// ============================================================================
//  Dictionary.hpp – search JMdict data stored on the SD card
// ============================================================================
//
//  Files produced by tools/jmdict2jisho.py (full spec: docs/02-dictionary.md):
//
//    /jisho/entries.dat   all entries, each = u16 length + UTF-8 text record
//    /jisho/kana.idx      sorted (hiragana reading -> entry offset) records
//    /jisho/kanji.idx     the same for every written form 食べる, 喰べる (optional: look-up of a selected word)
//    /jisho/eng.idx       sorted (english word     -> entry offset) records
//    /jisho/info.txt      human-readable build info + licence
//
//  Index file layout, version 2 (all integers little-endian):
//
//    offset 0   char[4]  magic "JIDX"
//           4   u16      version (2)
//           6   u16      recordSize (48)
//           8   u16      keySize (43)
//          10   u16      blockRecords (256)
//          12   u32      recordCount
//          16   records...   each: key[keySize] (UTF-8, zero padded)
//                                  u8  score   (higher = more relevant)
//                                  u32 offset  (into entries.dat)
//          then the SPARSE INDEX: key[keySize] of record 0, 256, 512, ...
//
//  HOW A LOOKUP WORKS (two levels, like a tiny B-tree):
//    1. at open(), load the sparse index into RAM (≈ 50–150 KB, PSRAM)
//    2. binary-search it IN RAM -> which 256-record block can hold the key
//    3. read that ONE block (12 KB) from the card, search inside it
//  An SD card charges per operation (~1 ms), not per byte, so "one 12 KB
//  read" beats "twenty 48-byte reads" by a wide margin. Run tests/bench.sh
//  to see the numbers.
// ============================================================================

#include <cstdint>
#include <cstddef>     // SIZE_MAX
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>     // std::pair
#include <vector>

#include "hal/Hal.hpp"

// One parsed dictionary entry (what the detail screen shows).
struct DictEntry {
    struct Sense {
        std::string tags;                  // "v1,vt" – part of speech etc.
        std::vector<std::string> glosses;  // "to eat", "to live on"
    };
    bool common = false;                   // ★ marks frequent words
    std::vector<std::string> kanji;        // 食べる, 喰べる ... (may be empty)
    std::vector<std::string> readings;     // たべる
    std::vector<Sense> senses;

    const std::string& headword() const { return kanji.empty() ? readings.at(0) : kanji[0]; }
};

// One line in the result list. Small on purpose: we keep ~40 of these.
struct DictHit {
    uint32_t offset = 0;      // where the full entry lives in entries.dat
    uint16_t score = 0;       // u16: multi-word searches add up word scores
    bool exact = false;       // key == query (not just starts with it)
    uint8_t keyLen = 0;       // shorter keys rank higher among equals
    std::string headword, reading, gloss;   // filled by Dictionary::describe()
    bool common = false;
    bool described = false;   // headword/reading/gloss loaded yet?
};

// One dictionary entry found for the text under a cursor (see Dictionary::lookupAt).
struct LookupHit {
    DictHit hit;            // offset, headword, reading, gloss, common (all described)
    size_t chars = 0;       // how many characters of the text it matches
    std::string matched;    // those characters as they stand in the text: "食べました"
    std::string form;       // the dictionary form found: "食べる" (de-inflected), "子熊" (for 子グマ).
                            // == matched when the text is written as in the dictionary
};

// Wraps one .idx file.
class DictIndex {
public:
    bool open(const std::string& path);
    uint32_t count() const { return _count; }

    // Record i -> key/score/offset. Served from a one-block cache, so
    // reading records i, i+1, i+2... costs one SD read per 256 records.
    bool record(uint32_t i, std::string& key, uint8_t& score, uint32_t& offset);

    // First record whose key is >= prefix (std::lower_bound semantics).
    uint32_t lowerBound(std::string_view prefix);

private:
    std::string_view sparseKey(uint32_t j) const;   // key of record j*blockRecords
    bool loadBlock(uint32_t block);

    hal::File _file;
    uint16_t _recSize = 0, _keySize = 0, _blockRecs = 0;
    uint32_t _count = 0;
    std::string _sparse;                 // all sparse keys, keySize bytes each
    std::vector<uint8_t> _block;         // the cached block of records
    int64_t _blockNo = -1;               // which block is cached (-1 = none)
    static constexpr uint32_t kHeaderSize = 16;
};

class Dictionary {
public:
    bool open(const std::string& dir);   // e.g. paths::kDict = "/tdeckjisho/jisho"
    bool isOpen() const { return _open; }
    // Release the files (the dictionary builder is about to replace them:
    // FAT cannot rename a folder while files in it are open).
    void close() {
        _open = false;
        _error = "closed for update";
        _entries.close();
        _kana = DictIndex();          // move-assign a fresh one: closes its file
        _eng = DictIndex();
        _kanji = DictIndex();
        _hasKanji = false;
    }
    const std::string& error() const { return _error; }

    // Prefix search by reading (hiragana). Best matches first.
    // Only the first `describeFirst` hits get their text loaded (one SD read
    // each); call describe() for more when the user scrolls ("lazy loading").
    std::vector<DictHit> searchKana(const std::string& hiragana, size_t limit,
                                    size_t describeFirst = SIZE_MAX);
    // English search. Several words narrow the result ("ice cream").
    // (Multi-word searches always describe all hits: ranking needs the text.)
    std::vector<DictHit> searchEnglish(const std::string& query, size_t limit,
                                       size_t describeFirst = SIZE_MAX);
    // Load headword/reading/gloss for hits [from, to); skips loaded ones.
    void describe(std::vector<DictHit>& hits, size_t from, size_t to);

    bool loadEntry(uint32_t offset, DictEntry& out);

    // A pseudo-random entry, for the quiz. `r` is any random number: record (r % count) of
    // kana.idx is taken (so every entry can come up, entries with several readings a bit
    // more often) and its entry loaded. False if the dictionary is empty or unreadable.
    bool randomEntry(uint32_t r, DictEntry& out, uint32_t& offset);

    // ---- look up the text under a cursor --------------------------------------
    // `text` is what follows the cursor in the document, plain (no codes, no furigana readings).
    // Japanese: LONGEST MATCH. Japanese has no spaces, so we do not know where the word ends:
    // try the first 12 characters, then 11, ... down to 1, and for each length look the text up
    // as it is AND in the dictionary forms de-inflection suggests ("食べた" -> "食べる"). The
    // longest match comes first. Latin text: the word under the cursor, looked up in English.
    // Empty if nothing matches. Needs kanji.idx for words written with kanji (hasKanjiIndex()).
    // `before`: the text just before the cursor in the same paragraph (only its last character
    // counts). After a number, counters come first (3匹, 五人); after a kanji, a voiced katakana
    // word is also tried un-voiced (雄ジカ -> シカ, rendaku).
    std::vector<LookupHit> lookupAt(std::string_view text, size_t maxHits = 4, std::string_view before = {});
    bool hasKanjiIndex() const { return _hasKanji; }

private:
    // Entries whose key is exactly `form`: via kanji.idx if it has kanji, else via kana.idx.
    std::vector<DictHit> exactForm(const std::string& form);
    // A word written half in kanji, half in katakana, which JMdict doesn't list as such:
    // 子グマ (子熊, こぐま), ガン細胞 (癌細胞). Only forms made of exactly one kanji run and one
    // katakana run. Each hit comes with the entry's kanji form that fits (子熊), for display.
    std::vector<std::pair<DictHit, std::string>> mixedForm(const std::string& form);
    // A katakana noun, possibly voiced by a compound: グマ -> クマ (熊). Only entries that list
    // the katakana spelling; at most one hit, with its kanji form for display.
    std::vector<std::pair<DictHit, std::string>> katakanaNoun(const std::string& kata, bool unvoicedOnly = false);
    // Does the entry list exactly this kana spelling as a reading? (熊: くま and クマ.) One SD read.
    bool listsSpelling(uint32_t offset, const std::string& kana);
    // Collect hits for one key. exactKey=true: only records whose key IS
    // `key` (a complete word); false: every key starting with `key`.
    std::vector<DictHit> scan(DictIndex& idx, const std::string& key, size_t maxScan, bool exactKey);
    static void rank(std::vector<DictHit>& hits);
    // Fill headword/reading/gloss of the hits, reading entries in FILE
    // ORDER (forward seeks only). `phrase`, if given, marks hits whose
    // gloss equals it as exact.
    void describeRange(std::vector<DictHit>& hits, size_t from, size_t to,
                       const std::string* phrase = nullptr);

    bool _open = false;
    std::string _error;
    DictIndex _kana, _eng, _kanji;
    bool _hasKanji = false;
    hal::File _entries;
    uint32_t _entriesSize = 0;
};
