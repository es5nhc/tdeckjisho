#include "dict/Dictionary.hpp"

#include <algorithm>       // std::sort, std::min
#include <cstring>         // memcmp, memchr
#include <unordered_map>

#include "core/Deinflect.hpp"
#include "core/Str.hpp"
#include "core/Utf8.hpp"

namespace {

// Read little-endian integers from a byte buffer.
// We do NOT `memcpy` the bytes into a struct: struct layout (padding,
// alignment, byte order) is compiler- and CPU-specific. Assembling the value
// byte by byte is portable and states the file format explicitly.
// Python equivalent: struct.unpack_from("<H", buf, 0)
uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// A zero-padded key -> its real length.
size_t keyLength(const char* p, size_t max) {
    const void* zero = std::memchr(p, 0, max);
    return zero ? static_cast<const char*>(zero) - p : max;
}

// Words that appear in thousands of glosses and are therefore not indexed
// (the converter skips the same list – keep them in sync!).
bool isStopWord(const std::string& w) {
    static const char* const kStop[] = {"a", "an", "the", "to", "of", "in", "on", "at",
                                        "for", "and", "or", "be", "is", "by", "with",
                                        "as", "sb", "sth", "etc", "one", "s"};
    for (const char* s : kStop) if (w == s) return true;
    return w.size() < 2;   // the converter also skips 1-letter words
}

std::vector<std::string> englishWords(const std::string& q) {
    std::vector<std::string> words;
    std::string cur;
    for (char c : str::toLower(q) + " ") {          // trailing " " flushes the last word
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) cur += c;
        else if (!cur.empty()) { if (!isStopWord(cur)) words.push_back(cur); cur.clear(); }
    }
    // Typing "e" or "ea" is a prefix in progress: keep it even if short.
    if (words.empty()) {
        const std::string t = str::toLower(std::string(str::trim(q)));
        if (!t.empty() && t.find(' ') == std::string::npos) words.push_back(t);
    }
    return words;
}

// "To eat (food)" -> "eat"  (same rule as gloss_core() in the converter)
std::string normalizeGloss(std::string_view g) {
    std::string out;
    int depth = 0;
    for (char c : str::toLower(g)) {
        if (c == '(') ++depth;
        else if (c == ')') depth = std::max(0, depth - 1);
        else if (depth == 0) out += c;
    }
    std::string t(str::trim(out));
    if (str::startsWith(t, "to ")) t = t.substr(3);
    return std::string(str::trim(t));
}

}  // namespace

// ============================================================================
//  DictIndex
// ============================================================================

bool DictIndex::open(const std::string& path) {
    if (!_file.open(path)) return false;
    uint8_t h[kHeaderSize];
    if (_file.read(h, sizeof h) != sizeof h || std::memcmp(h, "JIDX", 4) != 0) return false;
    if (le16(h + 4) != 2) return false;          // v1 files: re-run the converter
    _recSize = le16(h + 6);
    _keySize = le16(h + 8);
    _blockRecs = le16(h + 10);
    _count = le32(h + 12);
    // SECURITY: a header is a CLAIM, not a fact – the file may be damaged, truncated
    // or made by someone else. Accept only the exact layout the converters write (a
    // key size of 0 would divide by zero in lowerBound()), and do the size arithmetic
    // in 64 bits: 100 million records * 48 bytes overflows 32 bits and would then
    // "fit" into a tiny file.
    if (_keySize != 43 || _recSize != 48 || _blockRecs != 256) return false;
    const uint64_t nSparse = (static_cast<uint64_t>(_count) + _blockRecs - 1) / _blockRecs;
    const uint64_t sparseAt = kHeaderSize + static_cast<uint64_t>(_count) * _recSize;
    if (_file.size() < sparseAt + nSparse * _keySize) return false;   // truncated

    // Level 1 of the index: one sequential read, kept in RAM for good.
    // (≈ 150 KB for the English index: std::string's buffer is > 4 KB, so on
    //  the ESP32 it lands in PSRAM automatically.)
    _sparse.assign(nSparse * _keySize, '\0');
    if (!_file.seek(sparseAt) || _file.read(&_sparse[0], _sparse.size()) != _sparse.size()) return false;
    _blockNo = -1;
    return true;
}

std::string_view DictIndex::sparseKey(uint32_t j) const {
    const char* p = _sparse.data() + static_cast<size_t>(j) * _keySize;
    return std::string_view(p, keyLength(p, _keySize));
}

bool DictIndex::loadBlock(uint32_t block) {
    if (static_cast<int64_t>(block) == _blockNo) return true;     // cache hit
    const uint32_t first = block * _blockRecs;
    const uint32_t n = std::min<uint32_t>(_blockRecs, _count - first);
    _block.resize(static_cast<size_t>(n) * _recSize);
    if (!_file.seek(kHeaderSize + first * _recSize)) return false;
    if (_file.read(_block.data(), _block.size()) != _block.size()) { _blockNo = -1; return false; }
    _blockNo = block;
    return true;
}

bool DictIndex::record(uint32_t i, std::string& key, uint8_t& score, uint32_t& offset) {
    if (i >= _count || !loadBlock(i / _blockRecs)) return false;
    const uint8_t* p = _block.data() + static_cast<size_t>(i % _blockRecs) * _recSize;
    const char* k = reinterpret_cast<const char*>(p);
    key.assign(k, keyLength(k, _keySize));
    score = p[_keySize];
    offset = le32(p + _keySize + 1);
    return true;
}

uint32_t DictIndex::lowerBound(std::string_view prefix) {
    // Classic binary search for the first position where key >= prefix,
    // done in two levels. Invariant in both loops: everything before `lo`
    // is < prefix, everything from `hi` onwards is >= prefix.
    //
    // The converter sorted the keys as raw BYTES (Python: sorted by bytes),
    // and string_view::compare compares bytes as unsigned values too. If the
    // two programs disagreed on the order, binary search would silently miss
    // entries – so this pairing is essential.

    // Level 1 – in RAM: the first BLOCK whose first key is >= prefix.
    uint32_t lo = 0, hi = static_cast<uint32_t>(_sparse.size() / _keySize);
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;      // not (lo+hi)/2: avoids overflow
        if (sparseKey(mid).compare(prefix) < 0) lo = mid + 1;
        else hi = mid;
    }
    if (lo == 0) return 0;              // even the very first key is >= prefix

    // Level 2 – the answer is inside block lo-1 (whose first key is still
    // < prefix), or it is the first record of block lo. One SD read.
    uint32_t l = (lo - 1) * _blockRecs + 1;
    uint32_t h = std::min(_count, lo * _blockRecs);
    std::string key;
    uint8_t score;
    uint32_t off;
    while (l < h) {
        const uint32_t mid = l + (h - l) / 2;
        if (!record(mid, key, score, off)) return _count;
        if (key.compare(prefix) < 0) l = mid + 1;
        else h = mid;
    }
    return l;
}

// ============================================================================
//  Dictionary
// ============================================================================

bool Dictionary::open(const std::string& dir) {
    _open = false;
    if (!hal::storageReady()) { _error = "SD card not found"; return false; }
    if (!_entries.open(dir + "/entries.dat")) { _error = "missing " + dir + "/entries.dat"; return false; }
    _entriesSize = _entries.size();
    // "bad" includes version-1 files: re-run tools/jmdict2jisho.py.
    if (!_kana.open(dir + "/kana.idx")) { _error = "missing/old " + dir + "/kana.idx (re-run converter)"; return false; }
    if (!_eng.open(dir + "/eng.idx")) { _error = "missing/old " + dir + "/eng.idx (re-run converter)"; return false; }
    // kanji.idx is optional: a dictionary from an older converter has none, and everything
    // but the look-up of kanji words keeps working.
    _hasKanji = _kanji.open(dir + "/kanji.idx");
    _open = true;
    hal::log("[dict] %u kana keys, %u kanji keys, %u english keys\n", _kana.count(),
             _hasKanji ? _kanji.count() : 0u, _eng.count());
    return true;
}

std::vector<DictHit> Dictionary::scan(DictIndex& idx, const std::string& query, size_t maxScan,
                                      bool exactKey) {
    std::vector<DictHit> hits;
    // Keys longer than the stored key size were truncated by the converter,
    // so we must truncate the query the same way before comparing.
    const std::string key(utf8::truncateBytes(query, 43));
    if (key.empty()) return hits;

    // The same entry can appear under several keys ("eat", "eaten"...).
    // offset -> position in `hits`, so duplicates are found in O(1).
    // (Python: a dict. The first version used a linear search here – fine
    //  for 300 hits, far too slow for the 5000 of a multi-word search.)
    std::unordered_map<uint32_t, size_t> seen;

    std::string k;
    uint8_t score;
    uint32_t off;
    size_t scanned = 0;
    for (uint32_t i = idx.lowerBound(key); i < idx.count() && scanned < maxScan; ++i, ++scanned) {
        if (!idx.record(i, k, score, off) || !str::startsWith(k, key)) break;
        const bool exact = (k == key);
        // Sorted order: the key itself comes first, then longer keys. So in
        // exact mode the first non-equal key ends the scan.
        if (exactKey && !exact) break;

        const auto it = seen.find(off);
        if (it != seen.end()) {
            DictHit& d = hits[it->second];
            if (exact && !d.exact) {
                // The EXACT key decides the score, not a longer key of the
                // same entry. (Bug found with real data: 冠 has readings
                // かんむり (main, high score) and かん; typing "kan" ranked it
                // first because かんむり's score leaked into the かん match.)
                d.exact = true;
                d.keyLen = static_cast<uint8_t>(k.size());
                d.score = score;
            } else if (exact == d.exact) {
                d.score = std::max<uint16_t>(d.score, score);
            }
            continue;
        }
        DictHit h;
        h.offset = off;
        h.score = score;
        h.exact = exact;
        h.keyLen = static_cast<uint8_t>(k.size());
        seen[off] = hits.size();
        hits.push_back(std::move(h));   // move: no copy of the (empty) strings
    }
    return hits;
}

void Dictionary::rank(std::vector<DictHit>& hits) {
    // Exact matches first, then by score, then shorter keys.
    // std::sort takes a "less than" function, like Python's
    //   hits.sort(key=lambda h: (not h.exact, -h.score, h.keyLen))
    std::sort(hits.begin(), hits.end(), [](const DictHit& a, const DictHit& b) {
        if (a.exact != b.exact) return a.exact;
        if (a.score != b.score) return a.score > b.score;
        return a.keyLen < b.keyLen;
    });
}

std::vector<DictHit> Dictionary::searchKana(const std::string& hiragana, size_t limit,
                                            size_t describeFirst) {
    if (!_open) return {};
    // 1000 records ≈ 4 blocks: cheap now, and a short prefix like "か" gets
    // a fairer sample of candidates to rank.
    auto hits = scan(_kana, hiragana, 1000, false);
    rank(hits);
    if (hits.size() > limit) hits.resize(limit);
    describeRange(hits, 0, describeFirst);
    return hits;
}

std::vector<DictHit> Dictionary::searchEnglish(const std::string& query, size_t limit,
                                               size_t describeFirst) {
    if (!_open) return {};
    const std::vector<std::string> words = englishWords(query);
    if (words.empty()) return {};

    std::vector<DictHit> hits;
    if (words.size() == 1) {
        hits = scan(_eng, words[0], 1000, false);   // prefix: the user may still be typing
        rank(hits);
    } else {
        // POSTING-LIST INTERSECTION – how search engines answer "ice cream":
        // each word has a list of entries containing it (its "postings").
        // Sort each list by entry offset; then walking two sorted lists side
        // by side finds the common entries in one pass. No entry text has to
        // be read until we know an entry contains ALL the words.
        for (size_t w = 0; w < words.size(); ++w) {
            // Every word but the last is complete -> exact key. The last one
            // may still be being typed -> prefix.
            const bool complete = w + 1 < words.size();
            auto postings = scan(_eng, words[w], 5000, complete);
            std::sort(postings.begin(), postings.end(),
                      [](const DictHit& a, const DictHit& b) { return a.offset < b.offset; });
            if (w == 0) { hits = std::move(postings); continue; }

            std::vector<DictHit> both;
            size_t i = 0, j = 0;
            while (i < hits.size() && j < postings.size()) {   // the "merge" walk
                if (hits[i].offset < postings[j].offset) ++i;
                else if (hits[i].offset > postings[j].offset) ++j;
                else {
                    DictHit h = std::move(hits[i]);
                    h.score = static_cast<uint16_t>(h.score + postings[j].score);
                    both.push_back(std::move(h));
                    ++i, ++j;
                }
            }
            hits = std::move(both);
            if (hits.empty()) break;
        }
        // "exact" from scan() only meant "one word matched exactly" – for a
        // phrase, exact must mean "a gloss IS the phrase". describe() checks.
        for (auto& h : hits) h.exact = false;
        rank(hits);
    }

    if (hits.size() > limit) hits.resize(limit);
    if (words.size() > 1) {
        const std::string phrase = normalizeGloss(query);
        describeRange(hits, 0, hits.size(), &phrase);
        rank(hits);   // phrase bonuses changed the scores
    } else {
        describeRange(hits, 0, describeFirst);
    }
    return hits;
}

void Dictionary::describe(std::vector<DictHit>& hits, size_t from, size_t to) {
    describeRange(hits, from, to);
}

void Dictionary::describeRange(std::vector<DictHit>& hits, size_t from, size_t to,
                               const std::string* phrase) {
    // Visit the entries in FILE order, not rank order. On FAT, a forward
    // seek continues along the cluster chain; a backward seek may restart
    // from the beginning of the file. Same reads, much less walking.
    std::vector<DictHit*> order;
    to = std::min(to, hits.size());
    for (size_t i = from; i < to; ++i)
        if (!hits[i].described) order.push_back(&hits[i]);
    std::sort(order.begin(), order.end(),
              [](const DictHit* a, const DictHit* b) { return a->offset < b->offset; });

    for (DictHit* hit : order) {
        DictEntry e;
        hit->described = true;
        if (!loadEntry(hit->offset, e)) { hit->headword = "?"; continue; }
        hit->headword = e.headword();
        hit->reading = e.kanji.empty() ? "" : e.readings.at(0);
        hit->common = e.common;
        hit->gloss.clear();
        if (!e.senses.empty()) {   // first sense, glosses joined: one line in the list
            for (size_t i = 0; i < e.senses[0].glosses.size(); ++i) {
                if (i) hit->gloss += "; ";
                hit->gloss += e.senses[0].glosses[i];
            }
        }
        if (phrase) {
            // Where does a gloss equal the phrase? As the MAIN meaning (first
            // gloss of the first sense) it should win; deep in sense 4 it is
            // only a minor reading. Real data taught us this: クリーム lists
            // "ice cream" too, but アイスクリーム means nothing else.
            int bonus = 0;
            for (size_t si = 0; si < e.senses.size(); ++si)
                for (size_t gi = 0; gi < e.senses[si].glosses.size(); ++gi)
                    if (normalizeGloss(e.senses[si].glosses[gi]) == *phrase)
                        bonus = std::max(bonus, si == 0 ? (gi == 0 ? 300 : 120) : 60);
            hit->exact = bonus > 0;
            hit->score = static_cast<uint16_t>(hit->score + bonus);
        }
    }
}

// ============================================================================
//  Look-up of the text under a cursor
// ============================================================================
namespace {
constexpr size_t kMaxLookupChars = 12;     // the longest word we try (most words are 2–6 characters)

// Characters a Japanese word is made of. Punctuation, digits and Latin letters end the search.
bool isWordChar(uint32_t cp) {
    return utf8::isHiragana(cp) || utf8::isKatakana(cp) || utf8::isKanji(cp);
}
bool isLatinLetter(uint32_t cp) { return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z'); }

// A number just before a counter: 3匹, ３本, 五人, 百円, and 何匹 / 数匹 (how many / several).
bool isNumberChar(uint32_t cp) {
    if ((cp >= '0' && cp <= '9') || (cp >= 0xFF10 && cp <= 0xFF19)) return true;   // 0-9, ０-９
    static constexpr std::string_view kNumerals = "〇一二三四五六七八九十百千万億兆何数幾";
    for (size_t i = 0; i < kNumerals.size();)
        if (utf8::next(kNumerals, i) == cp) return true;
    return false;
}

// The index of the first sense with this tag ("ctr" = counter, "suf" = suffix), or -1.
int senseWithTag(const DictEntry& e, std::string_view tag) {
    for (size_t i = 0; i < e.senses.size(); ++i)
        for (std::string_view t : str::split(e.senses[i].tags, ','))
            if (t == tag) return static_cast<int>(i);
    return -1;
}

// The first katakana without its voicing marks: グマ -> クマ, ジカ -> シカ, ブタ / プラ -> フタ / フラ.
// Rendaku, the voicing of a compound's second part, only ever adds ゛ (and ハ-row ゜ is undone
// the same way). Anything else is returned unchanged.
std::string unvoiceFirst(const std::string& kata) {
    static constexpr std::string_view kPairs[][2] = {
        {"ガ", "カ"}, {"ギ", "キ"}, {"グ", "ク"}, {"ゲ", "ケ"}, {"ゴ", "コ"},
        {"ザ", "サ"}, {"ジ", "シ"}, {"ズ", "ス"}, {"ゼ", "セ"}, {"ゾ", "ソ"},
        {"ダ", "タ"}, {"ヂ", "チ"}, {"ヅ", "ツ"}, {"デ", "テ"}, {"ド", "ト"},
        {"バ", "ハ"}, {"ビ", "ヒ"}, {"ブ", "フ"}, {"ベ", "ヘ"}, {"ボ", "ホ"},
        {"パ", "ハ"}, {"ピ", "ヒ"}, {"プ", "フ"}, {"ペ", "ヘ"}, {"ポ", "ホ"},
    };
    for (const auto& p : kPairs)
        if (str::startsWith(kata, p[0])) return std::string(p[1]) + kata.substr(p[0].size());
    return kata;
}
}  // namespace

bool Dictionary::listsSpelling(uint32_t offset, const std::string& kana) {
    DictEntry e;
    return loadEntry(offset, e) && std::find(e.readings.begin(), e.readings.end(), kana) != e.readings.end();
}

std::vector<DictHit> Dictionary::exactForm(const std::string& form) {
    bool kanji = false, kana = true, allKatakana = true;
    size_t chars = 0;
    for (size_t i = 0; i < form.size(); ++chars) {
        const uint32_t cp = utf8::next(form, i);
        if (!utf8::isKatakana(cp)) allKatakana = false;
        if (utf8::isKanji(cp)) kanji = true;
        else if (!utf8::isHiragana(cp) && !utf8::isKatakana(cp)) kana = false;
    }
    std::vector<DictHit> hits;
    if (kanji) {
        if (_hasKanji) hits = scan(_kanji, form, 30, /*exactKey=*/true);
    } else if (kana) {
        hits = scan(_kana, utf8::katakanaToHiragana(form), 30, /*exactKey=*/true);   // the index is hiragana
    }
    rank(hits);
    // Text in katakana: entries that list exactly this katakana spelling come first. JMdict
    // does that for words usually written in katakana, above all species names, which news
    // writes in katakana: シカ is 鹿 (listed as しか AND シカ) before 歯科 (only しか), although
    // 歯科 is the more common word. Only the top hits are checked (one SD read each): the
    // popup shows 4 rows. One character (シ) is too short to tell anything.
    if (allKatakana && chars >= 2) {
        const size_t n = std::min<size_t>(hits.size(), 12);
        std::vector<char> listed(n);
        for (size_t k = 0; k < n; ++k) listed[k] = listsSpelling(hits[k].offset, form);
        std::vector<DictHit> ordered;
        ordered.reserve(hits.size());
        for (size_t k = 0; k < n; ++k) if (listed[k]) ordered.push_back(std::move(hits[k]));
        for (size_t k = 0; k < n; ++k) if (!listed[k]) ordered.push_back(std::move(hits[k]));
        for (size_t k = n; k < hits.size(); ++k) ordered.push_back(std::move(hits[k]));
        hits = std::move(ordered);
    }
    return hits;
}

std::vector<std::pair<DictHit, std::string>> Dictionary::mixedForm(const std::string& form) {
    std::vector<std::pair<DictHit, std::string>> out;
    if (!_hasKanji) return out;

    // Split into runs of kanji and katakana; anything else (hiragana...) means "not this case".
    std::string kanjiPart, kataPart;
    bool kanjiFirst = false;
    int runs = 0, last = 0;                          // last run: 1 = kanji, 2 = katakana
    for (size_t i = 0; i < form.size();) {
        const size_t at = i;
        const uint32_t cp = utf8::next(form, i);
        const int kind = utf8::isKanji(cp) ? 1 : utf8::isKatakana(cp) ? 2 : 0;
        if (kind == 0) return out;
        if (kind != last) { ++runs; if (runs == 1) kanjiFirst = (kind == 1); last = kind; }
        (kind == 1 ? kanjiPart : kataPart) += form.substr(at, i - at);
    }
    if (runs != 2) return out;

    // The readings of the kanji part on its own: 子 -> こ, し, ね...
    std::vector<std::string> readings;
    std::vector<DictHit> parts = exactForm(kanjiPart);
    if (parts.size() > 8) parts.resize(8);
    for (const DictHit& p : parts) {
        DictEntry e;
        if (!loadEntry(p.offset, e)) continue;
        for (const std::string& r : e.readings)
            if (std::find(readings.begin(), readings.end(), r) == readings.end()) readings.push_back(r);
    }
    if (readings.size() > 12) readings.resize(12);

    // Each reading + the katakana as hiragana is a whole reading: こ + ぐま = こぐま. It counts
    // only if the entry also has a kanji form that starts (ends) with the kanji part: 子熊.
    const std::string kataHira = utf8::katakanaToHiragana(kataPart);
    for (const std::string& r : readings) {
        const std::string key = utf8::katakanaToHiragana(kanjiFirst ? r + kataHira : kataHira + r);
        for (DictHit& h : scan(_kana, key, 30, /*exactKey=*/true)) {
            if (std::any_of(out.begin(), out.end(), [&](const auto& o) { return o.first.offset == h.offset; })) continue;
            DictEntry e;
            if (!loadEntry(h.offset, e)) continue;
            for (const std::string& k : e.kanji) {
                const bool fits = k.size() > kanjiPart.size() &&
                                  (kanjiFirst ? str::startsWith(k, kanjiPart) : str::endsWith(k, kanjiPart));
                if (fits) { out.emplace_back(std::move(h), k); break; }
            }
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first.score > b.first.score; });

    // No whole word: 親グマ ("parent bear") is not in JMdict at all. In news such a compound is
    // a kanji prefix + a species in katakana: offer the species, the compound's head noun.
    if (out.empty() && kanjiFirst) out = katakanaNoun(kataPart);
    return out;
}

// A katakana noun that may carry rendaku, the voicing a compound adds to its second part's first
// sound: クマ -> 子グマ, シカ -> 雄ジカ, サル -> 野ザル. The cursor may also sit on the katakana half of
// such a compound: on グ of 子グマ the text starts with グマ, which is no word. Un-voiced FIRST:
// a voiced sound there is nearly always rendaku, and 野ザル is a wild 猿, not a 笊 (colander, also
// ザル). A word that really starts voiced (ゴリラ) is still found as written. Only entries that
// list the katakana spelling itself count (熊 lists クマ), so a random homophone of a fragment
// does not slip in, and only the best one is offered: this is a guess.
// unvoicedOnly: don't try the spelling as written (the caller already has those hits).
std::vector<std::pair<DictHit, std::string>> Dictionary::katakanaNoun(const std::string& kata, bool unvoicedOnly) {
    std::vector<std::pair<DictHit, std::string>> out;
    size_t chars = 0;
    for (size_t i = 0; i < kata.size(); ++chars)
        if (!utf8::isKatakana(utf8::next(kata, i))) return out;
    if (chars < 2) return out;                   // グ alone: too short to mean anything
    std::vector<std::string> spellings;
    const std::string plain = unvoiceFirst(kata);
    if (plain != kata) spellings.push_back(plain);
    if (!unvoicedOnly) spellings.push_back(kata);
    for (const std::string& k : spellings) {
        for (DictHit& h : exactForm(k)) {         // katakana spelling listed first, then by score
            DictEntry e;
            if (!loadEntry(h.offset, e)) continue;
            if (std::find(e.readings.begin(), e.readings.end(), k) == e.readings.end()) continue;
            out.emplace_back(std::move(h), e.kanji.empty() ? k : e.kanji[0]);
            return out;
        }
    }
    return out;
}

std::vector<LookupHit> Dictionary::lookupAt(std::string_view text, size_t maxHits, std::string_view before) {
    std::vector<LookupHit> out;
    if (!_open || text.empty()) return out;

    // ---- Latin text: one English word ---------------------------------------------------------
    size_t i0 = 0;
    if (isLatinLetter(utf8::next(text, i0))) {
        size_t end = 0;
        while (end < text.size() && (isLatinLetter(static_cast<uint8_t>(text[end])) || text[end] == '\'')) ++end;
        const std::string word(text.substr(0, end));
        std::vector<DictHit> hits = searchEnglish(word, maxHits, SIZE_MAX);
        for (DictHit& h : hits) {
            LookupHit l;
            l.hit = std::move(h);
            l.chars = word.size();
            l.matched = l.form = word;
            out.push_back(std::move(l));
        }
        return out;
    }

    // ---- Japanese: the longest match first -----------------------------------------------------
    // The character before the cursor tells something about the word under it.
    uint32_t prevCp = 0;
    if (!before.empty()) { size_t b = utf8::prev(before, before.size()); prevCp = utf8::next(before, b); }
    const bool afterNumber = isNumberChar(prevCp);       // 3匹: a counter
    const bool afterKanji = utf8::isKanji(prevCp) && !afterNumber;   // 雄ジカ: rendaku
    std::unordered_map<uint32_t, std::string> counterGloss;          // entry offset -> its counter sense
    std::vector<uint32_t> afterNumberOk;           // entries that fit after a number: counters, suffixes

    std::vector<size_t> ends;                      // byte offset just after each of the first characters
    for (size_t i = 0; i < text.size() && ends.size() < kMaxLookupChars;) {
        const uint32_t cp = utf8::next(text, i);
        if (!isWordChar(cp)) break;
        ends.push_back(i);
    }
    // After a number, collect a few more: the reordering below may lift shorter matches.
    const size_t collect = afterNumber ? maxHits * 3 : maxHits;
    for (size_t len = ends.size(); len >= 1 && out.size() < collect; --len) {
        const std::string matched(text.substr(0, ends[len - 1]));
        std::vector<std::string> forms{matched};
        // Endings only change in kana: if the last character is kanji there is nothing to strip.
        size_t lastAt = utf8::prev(matched, matched.size()), p = lastAt;
        if (len >= 2 && utf8::isHiragana(utf8::next(matched, p))) {
            for (std::string& f : deinflect::candidates(matched)) forms.push_back(std::move(f));
        }
        for (const std::string& form : forms) {
            // Each hit with the form to show for it: normally `form` itself; for 子グマ the
            // entry's kanji form 子熊 (see mixedForm).
            std::vector<std::pair<DictHit, std::string>> found;
            for (DictHit& h : exactForm(form)) found.emplace_back(std::move(h), form);
            if (found.empty()) found = mixedForm(form);
            if (found.empty()) found = katakanaNoun(form);       // グマ (in 子グマ) -> クマ
            // The cursor on ジ of 雄ジカ: ジカ IS a word (時価), so the step above didn't run, but
            // after a kanji the un-voiced シカ (鹿) is likelier. It goes first, unless the top hit
            // lists the spelling as written: 窓ガラス is glass (硝子 lists ガラス), not a crow.
            else if (afterKanji) {
                auto extra = katakanaNoun(form, /*unvoicedOnly=*/true);
                if (!extra.empty()) {
                    const bool writtenFirst = listsSpelling(found[0].first.offset, form);
                    found.insert(writtenFirst ? found.begin() + 1 : found.begin(), std::move(extra[0]));
                }
            }
            // After a number: counters first, then suffixes, then the rest. 3人 is さんにん (人,
            // counter), not ひと (person); 3時 is じ (o'clock, a suffix), not とき (time). Among hits
            // of the same length only: the longest match still comes first (3時間 is 時間).
            if (afterNumber && !found.empty()) {
                const size_t n = std::min<size_t>(found.size(), 12);
                std::vector<std::pair<DictHit, std::string>> groups[3];   // counter, suffix, other
                for (size_t k = 0; k < n; ++k) {
                    DictEntry e;
                    int group = 2;
                    if (loadEntry(found[k].first.offset, e)) {
                        if (const int c = senseWithTag(e, "ctr"); c >= 0) {
                            group = 0;
                            std::string g;
                            for (const std::string& x : e.senses[c].glosses) g += (g.empty() ? "" : "; ") + x;
                            counterGloss[found[k].first.offset] = g;
                        } else if (senseWithTag(e, "suf") >= 0 || senseWithTag(e, "n-suf") >= 0) {
                            group = 1;
                        }
                    }
                    if (group < 2) afterNumberOk.push_back(found[k].first.offset);
                    groups[group].push_back(std::move(found[k]));
                }
                for (size_t k = n; k < found.size(); ++k) groups[2].push_back(std::move(found[k]));
                found.clear();
                for (auto& g : groups) for (auto& x : g) found.push_back(std::move(x));
            }
            for (auto& [h, shown] : found) {
                const bool dup = std::any_of(out.begin(), out.end(), [&](const LookupHit& l) { return l.hit.offset == h.offset; });
                if (dup || out.size() >= collect) continue;
                LookupHit l;
                l.hit = std::move(h);
                l.chars = len;
                l.matched = matched;
                l.form = std::move(shown);
                out.push_back(std::move(l));
            }
        }
    }
    // After a number, a longer match that ends in hiragana is most likely the counter plus a
    // particle: ３本の is 本 + の, not 本の ("mere"). It moves below the shorter matches, unless it
    // is a counter or suffix itself. Kanji compounds keep their place: 3時間 is 時間, and JMdict
    // tags 時間 only as a noun, so the tags alone can't tell.
    if (afterNumber) {
        std::stable_partition(out.begin(), out.end(), [&](const LookupHit& l) {
            if (std::find(afterNumberOk.begin(), afterNumberOk.end(), l.hit.offset) != afterNumberOk.end()) return true;
            size_t p = utf8::prev(l.matched, l.matched.size());
            return !utf8::isHiragana(utf8::next(l.matched, p));
        });
        if (out.size() > maxHits) out.resize(maxHits);
    }
    // Text for the rows on screen: read the entries once, in file order (see describeRange).
    std::vector<DictHit> hits;
    for (const LookupHit& l : out) hits.push_back(l.hit);
    describe(hits, 0, hits.size());
    for (size_t k = 0; k < out.size(); ++k) out[k].hit = std::move(hits[k]);
    // A kanji form matched exactly, but the entry's headword is its FIRST form, which can be a
    // different kanji: 領 is an alternative spelling of 襟 (えり). Show the form that matched, or
    // the row looks like a wrong result (領空 → 領 → "襟").
    for (LookupHit& l : out) {
        // After a number show the counter meaning: 3本 is "counter for long objects", not "book".
        if (auto it = counterGloss.find(l.hit.offset); it != counterGloss.end()) l.hit.gloss = it->second;
        bool hasKanji = false;
        for (size_t i = 0; i < l.form.size();) hasKanji |= utf8::isKanji(utf8::next(l.form, i));
        if (hasKanji) l.hit.headword = l.form;
    }
    return out;
}

bool Dictionary::randomEntry(uint32_t r, DictEntry& out, uint32_t& offset) {
    if (!_open || _kana.count() == 0) return false;
    std::string key;
    uint8_t score = 0;
    return _kana.record(r % _kana.count(), key, score, offset) && loadEntry(offset, out);
}

bool Dictionary::loadEntry(uint32_t offset, DictEntry& out) {
    // Record = u16 length + payload. Payload (UTF-8 text), lines split by '\n':
    //   line 0: flags ("c" = common)
    //   line 1: kanji forms, separated by 0x1F (ASCII "unit separator")
    //   line 2: readings,    separated by 0x1F
    //   line 3+: one sense per line: tags 0x1F gloss 0x1F gloss ...
    // 0x1F can never appear in real text, so no escaping is needed.
    //
    // We don't know the length before reading it – but in the real JMdict
    // 99 % of records are under 256 bytes (median 72), so we read 256 bytes
    // right away: ONE SD operation for the length and the text together,
    // and a second one only for the rare long entry.
    // 64-bit sum: offset 0xFFFFFFFF + 2 would wrap around to 1 in 32 bits.
    if (static_cast<uint64_t>(offset) + 2 > _entriesSize || !_entries.seek(offset)) return false;
    const uint32_t first = std::min<uint32_t>(256, _entriesSize - offset);
    std::string buf(first, '\0');
    if (_entries.read(&buf[0], first) != first) return false;
    const uint16_t len = le16(reinterpret_cast<const uint8_t*>(buf.data()));
    if (static_cast<uint64_t>(offset) + 2 + len > _entriesSize) return false;   // claims past the end
    std::string payload = buf.substr(2, std::min<uint32_t>(len, first - 2));
    if (payload.size() < len) {                       // a long entry: read the rest
        const size_t more = len - payload.size();
        const size_t at = payload.size();
        payload.resize(len);
        if (_entries.read(&payload[at], more) != more) return false;
    }

    out = DictEntry{};
    const auto lines = str::split(payload, '\n');
    if (lines.size() < 3) return false;
    out.common = lines[0].find('c') != std::string_view::npos;
    auto fields = [](std::string_view line) {        // split a line into strings
        std::vector<std::string> v;
        if (line.empty()) return v;
        for (auto f : str::split(line, '\x1f')) v.emplace_back(f);
        return v;
    };
    out.kanji = fields(lines[1]);
    out.readings = fields(lines[2]);
    if (out.readings.empty()) return false;
    for (size_t i = 3; i < lines.size(); ++i) {
        if (lines[i].empty()) continue;
        auto f = fields(lines[i]);
        DictEntry::Sense s;
        s.tags = f[0];
        s.glosses.assign(f.begin() + 1, f.end());
        out.senses.push_back(std::move(s));
    }
    return true;
}
