#pragma once
// ============================================================================
//  Parsers.hpp – turn source formats into rt::Doc
// ============================================================================
//
//  Every parser is a plain function: (source text) -> Doc. No globals, no
//  hardware. That makes them easy to unit-test on the PC (tests/test_core.cpp).
//
//  Supported:
//    parseMarkdown  .md   – # headings, lists, > quotes, **bold**, *em*,
//                           `code`, [links](url), --- rules, ``` code blocks,
//                           and {漢字|かんじ} ruby (the "DenDen Markdown" syntax)
//    parsePlain     .txt  – one paragraph per line; Aozora Bunko ruby:
//                           漢字《かんじ》 and ｜base《ruby》; ［＃...］ notes dimmed
//    parseHtml      web   – readable text, headings, lists, links, <ruby>
// ============================================================================

#include <string>
#include <string_view>

#include "text/RichText.hpp"

namespace rt {

void parseMarkdown(std::string_view src, Doc& out);
void parsePlain(std::string_view src, Doc& out);
// Where did this HTML come from? A trust decision, so the CALLER states it – the
// parser never guesses (e.g. from an empty baseUrl). Pictures use it: only a
// Local file may refer to pictures on the SD card.
enum class Source { Local, Web };
void parseHtml(std::string_view html, const std::string& baseUrl, Source source, Doc& out);

// "&amp;" -> "&", "&#x65E5;" -> "日", "&otilde;" -> "õ" ...
std::string decodeEntities(std::string_view s);

// Index of the '>' that ends the tag starting at s[lt] ('<'), skipping '>' inside
// quoted attribute values; npos if none. Shared by the HTML and feed parsers.
size_t tagEnd(std::string_view s, size_t lt);

}  // namespace rt
