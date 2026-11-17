#pragma once
// ============================================================================
//  Str.hpp – small string helpers that Python gives you for free
// ============================================================================
//
//  std::string_view is a (pointer, length) pair that LOOKS AT someone else's
//  characters without copying them – similar to a Rust `&str` slice. It is
//  cheap to pass by value. The danger: if the string it points into is
//  destroyed or modified, the view dangles. Rule of thumb used in this
//  project: take string_view as a function PARAMETER, return std::string
//  when you create new text.
//
//  All functions are `inline` because they are defined in a header (see the
//  note in hal/Keys.hpp).
// ============================================================================

#include <cstdint>
#include <cstdio>    // snprintf
#include <cstdlib>   // strtol
#include <string>
#include <string_view>
#include <vector>

namespace str {

inline bool startsWith(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

inline bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Python: s.strip()
inline std::string_view trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && isSpace(s[b])) ++b;
    while (e > b && isSpace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

// Python: s.lower() – ASCII only. Non-ASCII bytes are left untouched, which is
// exactly right for UTF-8 (we never corrupt a multi-byte character).
inline std::string toLower(std::string_view s) {
    std::string out(s);
    for (char& c : out)                       // `char&` = modify in place
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

// Python: s.split(sep) – keeps empty fields, like Python does.
inline std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> parts;
    size_t start = 0;
    while (true) {
        const size_t pos = s.find(sep, start);
        if (pos == std::string_view::npos) { parts.push_back(s.substr(start)); break; }
        parts.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}

// Case-insensitive (ASCII) substring search.
inline bool containsNoCase(std::string_view hay, std::string_view needle) {
    return toLower(hay).find(toLower(needle)) != std::string::npos;
}

// Like Python's int(s, base) but returns `fallback` instead of raising.
inline long toLong(std::string_view s, long fallback = 0, int base = 10) {
    std::string tmp(s);                        // strtol needs a 0-terminated string
    char* end = nullptr;
    const long v = std::strtol(tmp.c_str(), &end, base);
    return (end == tmp.c_str()) ? fallback : v;
}

// "1234567" -> "1.2 MB"
inline std::string humanSize(uint32_t bytes) {
    char buf[24];
    if (bytes < 1024) snprintf(buf, sizeof buf, "%u B", static_cast<unsigned>(bytes));
    else if (bytes < 1024 * 1024) snprintf(buf, sizeof buf, "%.1f KB", bytes / 1024.0);
    else snprintf(buf, sizeof buf, "%.1f MB", bytes / (1024.0 * 1024.0));
    return buf;
}

}  // namespace str
