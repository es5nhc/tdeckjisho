#include "dict/JmdictEncode.hpp"

#include <algorithm>
#include <cstdlib>   // atoi
#include <cstring>
#include <map>

#include "core/Utf8.hpp"
#include "dict/BuildIo.hpp"

namespace build {

namespace {

constexpr char kSep = '\x1f';    // field separator inside an entry record

// Keep in sync with STOP_WORDS in the Python converter and isStopWord() in
// Dictionary.cpp. Three copies of one list is a smell; it is a good exercise
// to generate them from one file.
bool isStopWord(std::string_view w) {
    static const char* const kStop[] = {"a", "an", "the", "to", "of", "in", "on", "at", "for",
                                        "and", "or", "be", "is", "by", "with", "as", "sb",
                                        "sth", "etc", "one", "s"};
    for (const char* s : kStop) if (w == s) return true;
    return false;
}

// Python's str.isspace(), for the code points that can actually occur.
bool pySpace(uint32_t c) {
    return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0 ||
           c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 ||
           c == 0x202F || c == 0x205F || c == 0x3000;
}

bool hasTag(const std::vector<std::string>& v, const char* t) {
    return std::find(v.begin(), v.end(), t) != v.end();
}

// Python: re.sub(r"\([^)]*\)", "", s). Leftmost "(" up to the FIRST ")"
// after it; an unmatched "(" stays.
std::string removeParens(std::string_view s) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '(') {
            const size_t close = s.find(')', i + 1);
            if (close != std::string_view::npos) { i = close + 1; continue; }
        }
        out += s[i++];
    }
    return out;
}

void join(std::string& out, const std::vector<std::string>& parts, char sep) {
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
}

}  // namespace

std::string pyLower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const size_t at = i;
        const uint32_t cp = utf8::next(s, i);
        if (cp >= 'A' && cp <= 'Z') out += static_cast<char>(cp + 32);
        else if (cp == 0x212A) out += 'k';               // KELVIN SIGN -> k
        else if (cp == 0x0130) out += "i\xCC\x87";       // İ -> i + COMBINING DOT ABOVE
        else out.append(s.substr(at, i - at));           // everything else: unchanged
    }
    return out;
}

std::string_view pyStrip(std::string_view s) {
    size_t b = 0;
    while (b < s.size()) {
        size_t k = b;
        if (!pySpace(utf8::next(s, k))) break;
        b = k;
    }
    size_t e = s.size();
    while (e > b) {
        size_t start = e - 1;                            // back to the lead byte
        while (start > b && (static_cast<uint8_t>(s[start]) & 0xC0) == 0x80) --start;
        size_t k = start;
        if (!pySpace(utf8::next(s, k))) break;
        e = start;
    }
    return s.substr(b, e - b);
}

std::string glossCore(std::string_view g) {
    const std::string noParens = removeParens(pyLower(g));
    std::string_view t = pyStrip(noParens);
    if (t.substr(0, 3) == "to ") t = t.substr(3);
    return std::string(pyStrip(t));
}

int priority(const std::vector<std::string>& tags) {
    int p = 0;
    p += hasTag(tags, "news1") ? 25 : hasTag(tags, "news2") ? 5 : 0;
    p += hasTag(tags, "ichi1") ? 25 : hasTag(tags, "ichi2") ? 5 : 0;
    p += hasTag(tags, "spec1") ? 20 : hasTag(tags, "spec2") ? 10 : 0;
    p += hasTag(tags, "gai1") ? 20 : hasTag(tags, "gai2") ? 5 : 0;
    int best = -1;                                       // smallest nfXX
    for (const auto& t : tags) {
        if (t.size() < 3 || t[0] != 'n' || t[1] != 'f') continue;
        if (!std::all_of(t.begin() + 2, t.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
        const int n = std::atoi(t.c_str() + 2);
        if (best < 0 || n < best) best = n;
    }
    if (best >= 0) p += std::max(0, (49 - best) / 2);
    return std::min(p, 100);
}

bool isCommon(const std::vector<std::string>& tags) {
    for (const char* t : {"news1", "ichi1", "spec1", "spec2", "gai1"})
        if (hasTag(tags, t)) return true;
    return false;
}

void makeRecord(uint8_t* rec, std::string_view key, uint8_t score, uint32_t offset) {
    std::memset(rec, 0, kKeySize);                       // zero padding (Python: ljust)
    std::memcpy(rec, key.data(), std::min(key.size(), kKeySize));
    rec[kKeySize] = score;
    putLe32(rec + kKeySize + 1, offset);
}

bool encodeEntry(const JmEntry& e, std::string& payload, std::vector<KeyScore>& kana,
                 std::vector<KeyScore>& kanji, std::vector<KeyScore>& eng) {
    payload.clear();
    kana.clear();
    kanji.clear();
    eng.clear();

    // ---- senses: tags, the <pos> inheritance rule, English glosses only ----
    struct Sense { std::string tags; const std::vector<std::string>* glosses; };
    std::vector<Sense> senses;
    const std::vector<std::string>* lastPos = nullptr;
    for (const JmSense& s : e.senses) {
        // JMdict rule: a sense without <pos> inherits the previous one. Note
        // that this happens BEFORE the "has English glosses?" check, as in
        // Python (`last_pos = pos` comes first).
        const std::vector<std::string>* pos = s.pos.empty() && lastPos ? lastPos : &s.pos;
        lastPos = pos;
        if (s.glosses.empty()) continue;
        std::vector<std::string> tags(*pos);
        tags.insert(tags.end(), s.misc.begin(), s.misc.end());
        tags.insert(tags.end(), s.field.begin(), s.field.end());
        Sense out;
        join(out.tags, tags, ',');
        out.glosses = &s.glosses;
        senses.push_back(std::move(out));
    }
    if (e.readings.empty() || senses.empty()) return false;
    const int pri = priority(e.pri);

    // ---- the entry record: lines joined by \n, fields by 0x1F ----------------
    std::vector<std::string> lines;
    lines.push_back(isCommon(e.pri) ? "c" : "");
    lines.emplace_back();
    join(lines.back(), e.kanji, kSep);
    lines.emplace_back();
    join(lines.back(), e.readings, kSep);
    for (const Sense& s : senses) {
        std::string l = s.tags;
        for (const auto& g : *s.glosses) { l += kSep; l += g; }
        lines.push_back(std::move(l));
    }
    join(payload, lines, '\n');
    while (payload.size() > 65535 && !lines.empty()) {   // u16 length limit: drop senses
        lines.pop_back();
        payload.clear();
        join(payload, lines, '\n');
    }

    // ---- kana index: every reading, katakana folded to hiragana --------------
    for (size_t i = 0; i < e.readings.size(); ++i) {
        const std::string hira = utf8::katakanaToHiragana(e.readings[i]);
        kana.push_back({std::string(utf8::truncateBytes(hira, kKeySize)),
                        static_cast<uint8_t>(std::min(255, pri + (i == 0 ? 50 : 0)))});
    }

    // ---- kanji index: every written form as written (no folding), the main one first --------
    for (size_t i = 0; i < e.kanji.size(); ++i) {
        kanji.push_back({std::string(utf8::truncateBytes(e.kanji[i], kKeySize)),
                         static_cast<uint8_t>(std::min(255, pri + (i == 0 ? 50 : 0)))});
    }

    // ---- english index: every word of every gloss, best score per word ------
    // (see the Python version for the reasoning behind each number)
    std::map<std::string, int> best;                     // word -> best score
    for (size_t si = 0; si < senses.size(); ++si) {
        const auto& gl = *senses[si].glosses;
        for (size_t gi = 0; gi < gl.size(); ++gi) {
            const std::string core = glossCore(gl[gi]);
            const std::string lower = pyLower(gl[gi]);
            // re.findall(r"[a-z0-9]+", lower): runs of ASCII letters/digits.
            for (size_t i = 0; i < lower.size();) {
                auto isWord = [&](size_t k) {
                    const char c = lower[k];
                    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
                };
                if (!isWord(i)) { ++i; continue; }
                size_t j = i;
                while (j < lower.size() && isWord(j)) ++j;
                const std::string w = lower.substr(i, j - i);
                i = j;
                if (w.size() < 2 || isStopWord(w)) continue;
                int score = pri * 40 / 100;
                if (core == w) score += 100;
                else if (core.compare(0, w.size() + 1, w + " ") == 0) score += 30;
                if (si < 3) score += (si == 0 ? 60 : si == 1 ? 30 : 15);
                if (gi == 0) score += 10;
                score = std::min(score, 255);
                const std::string key = w.substr(0, kKeySize);   // ASCII: no UTF-8 cut issue
                auto it = best.find(key);
                if (it == best.end()) best.emplace(key, score);
                else it->second = std::max(it->second, score);
            }
        }
    }
    for (const auto& [word, score] : best) eng.push_back({word, static_cast<uint8_t>(score)});
    return true;
}

}  // namespace build
