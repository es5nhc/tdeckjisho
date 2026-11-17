#pragma once
// ============================================================================
//  Feed.hpp – RSS 2.0 and Atom feeds
// ============================================================================
//
//  RSS:   <rss><channel><title>..</title>
//             <item><title/><link/><description/><pubDate/></item>...
//  Atom:  <feed><title/>
//             <entry><title/><link href=".."/><summary/><updated/></entry>...
//
//  Both are XML, but we do not need a full XML parser: we find each
//  <item>/<entry> block and pull out a few child elements by name.
//  Descriptions often contain (escaped) HTML; that goes through parseHtml().
// ============================================================================

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "text/RichText.hpp"

namespace rt {

struct FeedItem {
    std::string title;
    std::string link;
    std::string date;          // as the feed writes it
    int64_t time = 0;          // the same as seconds since 1970 UTC; 0 = unknown (see feedTime)
    std::string summaryHtml;   // may contain HTML markup
};

struct Feed {
    std::string title;
    std::vector<FeedItem> items;
};

// Items come out NEWEST FIRST (feeds promise no order); undated ones last, as they came.
bool parseFeed(std::string_view xml, Feed& out);

// A feed date as seconds since 1970-01-01 UTC, or 0 if it can't be read. Understands RSS's
// "Sun, 15 Nov 2026 06:21:00 +0900" (RFC 822) and Atom's "2026-11-15T06:21:00+09:00" (ISO 8601).
int64_t feedTime(std::string_view date);

// Build a readable article page: title, date, summary, "open full article".
void feedItemToDoc(const FeedItem& item, const std::string& feedTitle, Doc& out);

}  // namespace rt
