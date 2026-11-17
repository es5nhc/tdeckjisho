#include "core/Url.hpp"

#include <vector>

#include "core/Str.hpp"

namespace url {

namespace {

// Split "https://host:port/path?query#frag" into "https://host:port" and the
// rest. Returns false if there is no scheme.
bool splitOrigin(std::string_view u, std::string_view& origin, std::string_view& rest) {
    const size_t schemeEnd = u.find("://");
    if (schemeEnd == std::string_view::npos) return false;
    const size_t pathStart = u.find('/', schemeEnd + 3);
    if (pathStart == std::string_view::npos) { origin = u; rest = "/"; }
    else { origin = u.substr(0, pathStart); rest = u.substr(pathStart); }
    return true;
}

// Remove "." and ".." segments: "/a/b/../c/./d" -> "/a/c/d"
std::string normalizePath(std::string_view path) {
    // Keep "?query" aside – dots in the query are not path segments.
    std::string_view query;
    if (size_t q = path.find('?'); q != std::string_view::npos) {
        query = path.substr(q);
        path = path.substr(0, q);
    }
    std::vector<std::string_view> out;
    for (std::string_view seg : str::split(path, '/')) {
        if (seg == "." ) continue;
        if (seg == "..") { if (out.size() > 1) out.pop_back(); continue; }
        out.push_back(seg);
    }
    std::string result;
    for (size_t i = 0; i < out.size(); ++i) {
        if (i) result += '/';
        result += out[i];
    }
    // A trailing "/." or "/.." means "this directory": keep the slash.
    if (str::endsWith(path, "/.") || str::endsWith(path, "/..")) result += '/';
    if (result.empty() || result[0] != '/') result.insert(result.begin(), '/');
    result += query;
    return result;
}

}  // namespace

std::string resolve(std::string_view base, std::string_view href) {
    href = str::trim(href);
    if (href.empty()) return std::string(base);

    // Absolute URL already?
    if (href.find("://") != std::string_view::npos && href.find("://") < 10)
        return std::string(href);
    if (str::startsWith(href, "mailto:") || str::startsWith(href, "javascript:"))
        return std::string(href);   // the caller decides what to do with these

    std::string_view origin, basePath;
    if (!splitOrigin(base, origin, basePath)) return std::string(href);

    // "//host/path": same scheme as the base.
    if (str::startsWith(href, "//")) {
        const std::string_view scheme = base.substr(0, base.find("://"));
        return std::string(scheme) + ":" + std::string(href);
    }
    // Strip the fragment from the base, then handle "#frag" and "?query".
    if (size_t h = basePath.find('#'); h != std::string_view::npos) basePath = basePath.substr(0, h);
    if (href[0] == '#') return std::string(origin) + std::string(basePath) + std::string(href);
    if (href[0] == '?') {
        const std::string_view p = basePath.substr(0, basePath.find('?'));
        return std::string(origin) + std::string(p) + std::string(href);
    }
    if (href[0] == '/') return std::string(origin) + normalizePath(href);

    // Relative path: replace everything after the last '/' of the base path.
    std::string_view dir = basePath.substr(0, basePath.find('?'));
    dir = dir.substr(0, dir.rfind('/') + 1);
    return std::string(origin) + normalizePath(std::string(dir) + std::string(href));
}

std::string host(std::string_view u) {
    const size_t s = u.find("://");
    if (s == std::string_view::npos) return "";
    u = u.substr(s + 3);
    return std::string(u.substr(0, u.find_first_of("/:?#")));
}

std::string normalizeTyped(std::string_view typed) {
    std::string t(str::trim(typed));
    if (t.find("://") == std::string::npos) t = "https://" + t;
    return t;
}

bool isWebUrl(std::string_view u) {
    // Control bytes (and DEL) never belong in an address. A link like
    // href="/x&#13;&#10;Host: evil" decodes to a CR LF, and sent as-is it would end
    // the HTTP request line early and inject a header of the page's choosing.
    for (char c : u)
        if (static_cast<uint8_t>(c) < 0x20 || c == '\x7F') return false;
    const std::string head = str::toLower(u.substr(0, 8));
    return str::startsWith(head, "http://") || str::startsWith(head, "https://");
}

bool isHttps(std::string_view u) { return str::startsWith(str::toLower(u.substr(0, 8)), "https://"); }

std::string redirectProblem(std::string_view from, std::string_view to) {
    if (!isWebUrl(to)) return "refused: redirect to a non-web address";
    if (isHttps(from) && !isHttps(to)) return "refused: redirect from https to plain http";
    return "";
}

}  // namespace url
