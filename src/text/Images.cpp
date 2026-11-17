#include "text/Images.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "core/Paths.hpp"
#include "core/Str.hpp"
#include "hal/Hal.hpp"

namespace rt {

namespace {

constexpr uint32_t kMaxPixels = 4u * 1000u * 1000u;   // 4 megapixels

uint32_t be16(const uint8_t* p) { return (p[0] << 8) | p[1]; }
uint32_t be32(const uint8_t* p) { return (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
int32_t le32s(const uint8_t* p) {
    return static_cast<int32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24));
}

// "%20" -> " " (Markdown and HTML writers escape spaces in file names).
// `bad` is set for "%00" and other control bytes: no real file name contains them,
// and a NUL would silently cut the path short in the C file functions below us.
std::string percentDecode(std::string_view s, bool& bad) {
    std::string out;
    bad = false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const long v = str::toLong(s.substr(i + 1, 2), -1, 16);
            if (v >= 0) {
                if (v < 0x20) bad = true;
                out += static_cast<char>(v);
                i += 2;
                continue;
            }
        }
        if (static_cast<uint8_t>(s[i]) < 0x20) bad = true;
        out += s[i];
    }
    return out;
}

// A width or height from a file header: 1..65535, or 0 for "damaged". Never
// clamp: a header claiming 70 000 px is not a 65 535 px picture, it's a lie.
uint16_t dimension(int64_t v) {
    if (v < 0) v = -v;                       // BMP: negative height = top-down rows
    return (v >= 1 && v <= 65535) ? static_cast<uint16_t>(v) : 0;
}

// JPEG: walk the segments to the SOF marker.
bool probeJpeg(hal::File& f, Image& img) {
    uint32_t pos = 2;                                  // after FF D8 (start of image)
    uint8_t m[4];
    for (int guard = 0; guard < 200; ++guard) {
        if (!f.seek(pos) || f.read(m, 4) != 4) break;
        if (m[0] != 0xFF) break;                       // lost: not a marker
        const uint8_t marker = m[1];
        if (marker == 0xFF) { ++pos; continue; }       // fill byte
        const uint32_t len = be16(m + 2);              // includes the 2 length bytes
        // SOF0..SOF15, except C4 (Huffman tables), C8 (reserved), CC (arithmetic tables)
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            uint8_t sof[5];                            // precision, height, width
            if (f.read(sof, 5) != 5) break;
            img.h = static_cast<uint16_t>(be16(sof + 1));
            img.w = static_cast<uint16_t>(be16(sof + 3));
            if (marker == 0xC2) img.problem = "progressive JPEG (save it as baseline)";
            else if (marker != 0xC0 && marker != 0xC1) img.problem = "unsupported JPEG type";
            return true;
        }
        if (marker == 0xD9 || marker == 0xDA) break;   // end of image / image data: no SOF?!
        pos += 2 + len;                                // SEEK over the segment (EXIF can be 60 KB)
    }
    img.problem = "damaged JPEG";
    return false;
}

}  // namespace

std::string resolveImagePath(const std::string& docPath, const std::string& srcIn) {
    bool bad = false;
    const std::string src = percentDecode(str::trim(srcIn), bad);
    if (bad || src.empty() || src.find("://") != std::string::npos || str::startsWith(src, "data:")) return "";
    // Absolute ("/tdeckjisho/img/x.jpg") or relative to the document's folder.
    std::string joined = src[0] == '/' ? src : docPath.substr(0, docPath.rfind('/') + 1) + src;
    // Normalise "a/./b" and "a/x/../b" like a shell would.
    std::vector<std::string> parts;
    for (std::string_view p : str::split(joined, '/')) {
        if (p.empty() || p == ".") continue;
        if (p == "..") { if (!parts.empty()) parts.pop_back(); continue; }
        parts.emplace_back(p);
    }
    std::string out;
    for (const auto& p : parts) out += "/" + p;
    // SECURITY: a book may only show pictures from the books folder. Without this,
    // ![x](/tdeckjisho/config/wifi.txt) or ../../config/... would make the reader
    // open any file on the card. ".." was already resolved above, so a simple
    // prefix test is enough here.
    if (!str::startsWith(out, paths::kBooks + "/")) return "";
    return out;
}

bool probeImage(Image& img) {
    img.w = img.h = 0;
    hal::File f;
    if (img.path.empty()) { img.problem = "remote image (not loaded)"; return false; }
    if (!f.open(img.path)) { img.problem = "not found: " + img.path; return false; }
    uint8_t h[32] = {};
    const size_t n = f.read(h, sizeof h);

    bool ok = false;
    if (n >= 24 && be32(h) == 0x89504E47u) {                       // \x89 P N G
        img.format = ImageFormat::Png;
        img.w = dimension(be32(h + 16));
        img.h = dimension(be32(h + 20));
        ok = true;
    } else if (n >= 26 && h[0] == 'B' && h[1] == 'M') {
        img.format = ImageFormat::Bmp;
        // int64_t first: std::abs(INT32_MIN) does not fit in an int32_t (undefined behaviour).
        img.w = dimension(le32s(h + 18));
        img.h = dimension(le32s(h + 22));                          // negative = top-down rows
        ok = true;
    } else if (n >= 12 && h[0] == 'q' && h[1] == 'o' && h[2] == 'i' && h[3] == 'f') {
        img.format = ImageFormat::Qoi;
        img.w = dimension(be32(h + 4));
        img.h = dimension(be32(h + 8));
        ok = true;
    } else if (n >= 4 && h[0] == 0xFF && h[1] == 0xD8) {
        img.format = ImageFormat::Jpeg;
        ok = probeJpeg(f, img);
    } else {
        img.problem = "unknown image format";
    }
    if (ok && (img.w == 0 || img.h == 0)) { img.problem = "damaged image"; ok = false; }
    if (ok && img.problem.empty() && static_cast<uint32_t>(img.w) * img.h > kMaxPixels)
        img.problem = "too large (" + std::to_string(img.w) + "×" + std::to_string(img.h) + "), resize it on the PC";
    if (!img.problem.empty()) { img.w = img.h = 0; return false; }
    return true;
}

void resolveImages(Doc& doc, const std::string& docPath) {
    for (Image& img : doc.images) {
        img.path = resolveImagePath(docPath, img.src);
        if (!probeImage(img))
            hal::log("[image] %s: %s\n", img.src.c_str(), img.problem.c_str());
    }
}

}  // namespace rt
