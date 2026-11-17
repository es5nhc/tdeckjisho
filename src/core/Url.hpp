#pragma once
// ============================================================================
//  Url.hpp – resolve links found in web pages
// ============================================================================
//
//  A page at  https://example.com/news/today.html  may link to
//      "item.html"            -> https://example.com/news/item.html
//      "../about"             -> https://example.com/about
//      "/rss.xml"             -> https://example.com/rss.xml
//      "//cdn.example.com/x"  -> https://cdn.example.com/x
//      "https://other.org/"   -> unchanged
//  This is RFC 3986 §5.2 "reference resolution", simplified.
//  Python equivalent: urllib.parse.urljoin(base, href)
// ============================================================================

#include <string>
#include <string_view>

namespace url {

std::string resolve(std::string_view base, std::string_view href);

// "https://www3.nhk.or.jp/news/" -> "www3.nhk.or.jp"
std::string host(std::string_view u);

// Adds "https://" when the user typed a bare host name like "err.ee".
std::string normalizeTyped(std::string_view typed);

// SECURITY: the network policy, in one place for both targets.
// The only addresses TDeckJisho ever fetches: http:// and https:// (any letter case),
// with no control bytes anywhere (a CR LF would inject an HTTP header).
bool isWebUrl(std::string_view u);
bool isHttps(std::string_view u);
// May a request to `from` be redirected to `to`? Returns "" if yes, else the reason.
// Checked for EVERY hop: a server could otherwise send us to "file:///..." or "ftp:",
// or from https back to http – a downgrade, after which anyone on the network can
// read and answer the next request.
std::string redirectProblem(std::string_view from, std::string_view to);

}  // namespace url
