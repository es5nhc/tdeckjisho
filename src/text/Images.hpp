#pragma once
// ============================================================================
//  Images.hpp – find a document's pictures and learn their size, cheaply
// ============================================================================
//
//  The layout must know how TALL a picture is before anything is drawn (to
//  know where the next paragraph starts), but decoding a JPEG just to learn
//  its size would take ~100 ms per picture on the ESP32. Every image format
//  writes the size near the start of the file, so we read only the HEADER:
//
//    PNG   bytes 16..23 (IHDR chunk), big-endian       one read of 32 bytes
//    BMP   bytes 18..25, little-endian (height < 0 = stored top-down)
//    QOI   bytes 4..11, big-endian
//    JPEG  no fixed place: walk the "segments" (FF xx + length) until the
//          SOF ("start of frame") segment, which holds height and width.
//          Photos often carry a 20–60 KB EXIF segment first, so we SEEK
//          over segments instead of reading them.
//
//  Also decided here: what the decoders in LovyanGFX cannot draw.
//    * progressive JPEG (SOF2): TJpgDec reads only baseline JPEG
//    * huge pictures: decodable, but seconds per picture – resize on the PC
//
//  Pure C++ plus hal::File: unit-tested on the PC (tests/test_core.cpp).
// ============================================================================

#include <string>

#include "text/RichText.hpp"

namespace rt {

// Fill path/format/w/h/problem of every image in `doc`.
// docPath: the SD path of the document; relative srcs are relative to its folder.
void resolveImages(Doc& doc, const std::string& docPath);

// "/tdeckjisho/books/a.md" + "../img/b%20c.jpg" -> "/tdeckjisho/img/b c.jpg"
// Returns "" for remote (http:, https:) and data: URLs.
std::string resolveImagePath(const std::string& docPath, const std::string& src);

// Read the header of one file. Returns false (and sets img.problem) if it
// cannot be shown.
bool probeImage(Image& img);

}  // namespace rt
