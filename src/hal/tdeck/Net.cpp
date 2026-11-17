// ============================================================================
//  hal/tdeck/Net.cpp – Wi-Fi and HTTP(S) on the ESP32-S3
// ============================================================================
//
//  Wi-Fi runs inside ESP-IDF tasks in the background. WiFi.begin() returns
//  immediately; WiFi.status() tells us later whether it worked. Scans are
//  started with scanNetworks(async=true) and polled with scanComplete().
//
//  HTTPS: WiFiClientSecure runs TLS (mbedTLS) on the ESP32. Encryption alone is
//  worthless if we don't know WHO we are talking to: anyone on the café Wi-Fi
//  could answer instead of the real server. So every connection is checked
//  against the root certificates browsers trust (Mozilla's list), which
//  tools/make_ca_bundle.py compiles into the firmware. No bundle, no HTTPS:
//  we FAIL CLOSED, never fall back to an unchecked connection.
// ============================================================================

#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "ca_bundle.h"   // kCaBundle, kCaBundleCerts: generated at build time

#include <algorithm>   // std::min
#include <ctime>

#include "core/Url.hpp"
#include "hal/Hal.hpp"

#ifndef JISHO_VERSION            // set by tools/name_firmware.py from platformio.ini
#define JISHO_VERSION "dev"
#endif

namespace {

hal::WifiState g_state = hal::WifiState::Off;
uint32_t g_connectStart = 0;
bool g_ntpStarted = false;
constexpr uint32_t kConnectTimeoutMs = 20000;

// Arduino's HTTPClient writes the body into a `Stream`. We want it in a
// std::string, with a size limit. So we write a tiny Stream subclass – the
// adapter pattern. Only write() matters; the read side is never used.
class StringSink : public Stream {
public:
    StringSink(std::string& out, size_t limit) : _out(out), _limit(limit) {}
    bool truncated = false;

    size_t write(uint8_t b) override { return write(&b, 1); }
    size_t write(const uint8_t* buf, size_t n) override {
        if (_out.size() + n > _limit) { truncated = true; return 0; }   // 0 = abort
        _out.append(reinterpret_cast<const char*>(buf), n);
        return n;
    }
    // Pure virtual functions of Stream that we must provide:
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}

private:
    std::string& _out;
    // NOT `_max`: Arduino.h defines a MACRO called _max(a, b), and the
    // preprocessor would mangle this line before the compiler sees it.
    // Macros ignore namespaces and scopes – one reason modern C++ avoids them.
    size_t _limit;
};

}  // namespace

namespace hal {

void wifiConnect(const std::string& ssid, const std::string& password) {
    WiFi.mode(WIFI_STA);                 // station = client (not access point)
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid.c_str(), password.c_str());
    g_state = WifiState::Connecting;
    g_connectStart = ::millis();
    log("[wifi] connecting to '%s'\n", ssid.c_str());
}

void wifiDisconnect() {
    WiFi.disconnect(true);               // true = also turn the radio off
    g_state = WifiState::Off;
}

WifiState wifiState() {
    const wl_status_t s = WiFi.status();
    if (s == WL_CONNECTED) {
        if (g_state != WifiState::Connected) {
            log("[wifi] connected, IP %s\n", WiFi.localIP().toString().c_str());
            g_state = WifiState::Connected;
        }
        if (!g_ntpStarted) {
            // Ask NTP servers for the time; the ESP-IDF SNTP client keeps it
            // in sync from now on. Uses the TZ set in hal::begin().
            configTzTime("EET-2EEST,M3.5.0/3,M10.5.0/4", "pool.ntp.org", "time.google.com");
            g_ntpStarted = true;
        }
    } else if (g_state == WifiState::Connected) {
        g_state = WifiState::Connecting;  // lost it; auto-reconnect is trying
        g_connectStart = ::millis();
    } else if (g_state == WifiState::Connecting) {
        if (s == WL_CONNECT_FAILED || s == WL_NO_SSID_AVAIL ||
            ::millis() - g_connectStart > kConnectTimeoutMs) {
            g_state = WifiState::Failed;
        }
    }
    return g_state;
}

std::string wifiSsid() { return WiFi.SSID().c_str(); }
std::string wifiIp() { return WiFi.localIP().toString().c_str(); }

bool wifiScanStart() {
    if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);
    // async=true: returns at once; scanComplete() reports progress.
    return WiFi.scanNetworks(/*async=*/true) == WIFI_SCAN_RUNNING;
}

int wifiScanPoll(std::vector<WifiNetwork>& out) {
    const int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return -1;
    out.clear();
    for (int i = 0; i < n; ++i)
        out.push_back({WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.encryptionType(i) != WIFI_AUTH_OPEN});
    WiFi.scanDelete();                   // free the driver's result memory
    return n < 0 ? 0 : n;
}

HttpResult httpGet(const std::string& startUrl, size_t maxBytes) {
    HttpResult r;
    std::string current = startUrl;
    if (!url::isWebUrl(current)) { r.error = "refused: not an http(s) address"; return r; }

    // We follow redirects OURSELVES (max 5). HTTPClient can do it, but not
    // from http:// to https:// (that needs a different client object), and
    // we want to know the final URL to resolve relative links – and to check
    // every hop against our policy (url::redirectProblem).
    for (int hop = 0; hop < 5; ++hop) {
        const bool https = current.rfind("https://", 0) == 0;
        WiFiClientSecure secure;
        WiFiClient plain;
        if (https) {
            // Fail closed: without trusted roots we cannot check the server.
            if (kCaBundleCerts == 0) { r.error = "no CA certificates in this build (see README)"; return r; }
            secure.setCACertBundle(kCaBundle);   // verify the server's certificate chain
        }

        HTTPClient http;
        http.setTimeout(15000);
        http.setUserAgent("TDeckJisho/" JISHO_VERSION " (ESP32; text reader)");
        http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
        const char* keep[] = {"Location", "Content-Type"};
        http.collectHeaders(keep, 2);       // HTTPClient discards headers unless asked
        // `?:` cannot mix WiFiClientSecure and WiFiClient, so pick by reference:
        WiFiClient& client = https ? static_cast<WiFiClient&>(secure) : plain;
        if (!http.begin(client, current.c_str())) { r.error = "bad URL"; return r; }

        const int code = http.GET();
        r.status = code;
        r.finalUrl = current;
        if (code <= 0) {
            r.error = http.errorToString(code).c_str();
            http.end();
            break;
        }
        if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
            const std::string next = url::resolve(current, http.header("Location").c_str());
            http.end();
            r.error = url::redirectProblem(current, next);
            if (!r.error.empty()) { r.status = -1; break; }
            current = next;
            continue;
        }
        r.contentType = http.header("Content-Type").c_str();
        if (code == 200) {
            StringSink sink(r.body, maxBytes);
            http.writeToStream(&sink);     // also decodes "chunked" transfers
            r.truncated = sink.truncated;
        }
        http.end();
        break;
    }
    log("[http] GET %s -> %d, %u bytes, heap %u\n", startUrl.c_str(), r.status,
        static_cast<unsigned>(r.body.size()), static_cast<unsigned>(ESP.getFreeHeap()));
    return r;
}

// ---- secrets in NVS ----------------------------------------------------------
// Preferences is Arduino's wrapper around ESP-IDF's NVS: named "namespaces" of
// key/value pairs in a flash partition of their own. Not on the SD card, so the card
// can be lent, lost or copied without leaking the Wi-Fi password. (It is not
// ENCRYPTED: someone holding the device and a USB cable can still dump the flash.
// Protecting against that needs flash encryption, which doesn't mix with Launcher.)
namespace {
bool validSecretName(const std::string& name) {
    if (name.empty() || name.size() > 15) return false;          // NVS key limit
    for (char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}
}  // namespace

bool secretRead(const std::string& name, std::string& out) {
    if (!validSecretName(name)) return false;
    Preferences p;
    if (!p.begin("tdeckjisho", /*readOnly=*/true)) return false;  // nothing stored yet
    out = p.getString(name.c_str(), "").c_str();
    p.end();
    return !out.empty();
}

bool secretWrite(const std::string& name, const std::string& value) {
    if (!validSecretName(name) || value.size() > 3000) return false;   // NVS strings ~4000 max
    Preferences p;
    if (!p.begin("tdeckjisho", /*readOnly=*/false)) return false;
    bool ok = true;
    if (value.empty()) p.remove(name.c_str());               // "forget": no key at all
    else ok = p.putString(name.c_str(), value.c_str()) == value.size();
    p.end();
    return ok;
}

}  // namespace hal

// ============================================================================
//  Download – HTTPClient's raw stream, a few KB per poll()
// ============================================================================
//  httpGet() above lets HTTPClient::writeToStream() pull the whole body in one
//  blocking call. For a 10 MB file that would freeze the UI for 20+ seconds,
//  so here we take the socket ("stream") and read only what has already
//  arrived, every frame, straight into a file on the SD card.
//
//  Limitation, on purpose: the server must send Content-Length. With
//  "Transfer-Encoding: chunked" the raw stream contains chunk headers between
//  the data, and we would have to decode them ourselves. Static files (EDRDG,
//  `python3 -m http.server`) always send Content-Length.
// ============================================================================
namespace hal {

struct Download::Impl {
    HTTPClient http;
    WiFiClient plain;
    WiFiClientSecure secure;
    WiFiClient* stream = nullptr;
    WFile out;                    // our own HAL type: works the same on the PC
    std::string path;
    uint32_t lastData = 0;
    uint8_t buf[4096];
};

Download::Download() = default;
Download::~Download() = default;

bool Download::start(const std::string& startUrl, const std::string& path) {
    _impl = std::make_unique<Impl>();
    _done = _total = 0;
    _error.clear();
    _state = State::Failed;                 // until proven otherwise
    Impl& d = *_impl;
    d.path = path;

    // Connect and send the request. This part BLOCKS (DNS + TCP + maybe TLS:
    // 0.2–2 s); only the body is read without blocking. Redirects are followed
    // by hand, as in httpGet().
    std::string current = startUrl;
    if (!url::isWebUrl(current)) { _error = "refused: not an http(s) address"; return false; }
    for (int hop = 0; hop < 5; ++hop) {
        const bool https = current.rfind("https://", 0) == 0;
        if (https) {
            if (kCaBundleCerts == 0) { _error = "no CA certificates in this build (see README)"; return false; }
            d.secure.setCACertBundle(kCaBundle);
        }
        WiFiClient& client = https ? static_cast<WiFiClient&>(d.secure) : d.plain;
        d.http.setTimeout(15000);
        d.http.setUserAgent("TDeckJisho/" JISHO_VERSION " (ESP32; dictionary download)");
        d.http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
        const char* keep[] = {"Location"};
        d.http.collectHeaders(keep, 1);
        if (!d.http.begin(client, current.c_str())) { _error = "bad URL"; return false; }
        const int code = d.http.GET();
        if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
            const std::string next = url::resolve(current, d.http.header("Location").c_str());
            d.http.end();
            _error = url::redirectProblem(current, next);
            if (!_error.empty()) return false;
            current = next;
            continue;
        }
        if (code != 200) {
            _error = code < 0 ? std::string(d.http.errorToString(code).c_str()) : "HTTP " + std::to_string(code);
            d.http.end();
            return false;
        }
        break;
    }
    const int size = d.http.getSize();
    if (size <= 0) { _error = "server sent no Content-Length"; d.http.end(); return false; }
    if (static_cast<uint32_t>(size) > kMaxBytes) { _error = "file too large"; d.http.end(); return false; }
    _total = static_cast<uint32_t>(size);
    d.stream = d.http.getStreamPtr();
    if (!d.out.open(path + ".part")) { _error = "cannot create " + path + ".part"; d.http.end(); return false; }
    d.lastData = ::millis();
    _state = State::Running;
    log("[download] %s -> %s, %u bytes\n", current.c_str(), path.c_str(), static_cast<unsigned>(_total));
    return true;
}

Download::State Download::poll() {
    if (_state != State::Running) return _state;
    Impl& d = *_impl;
    const uint32_t t0 = ::millis();
    // Read for at most ~25 ms per call, so the UI keeps drawing.
    while (_done < _total && ::millis() - t0 < 25) {
        const int avail = d.stream->available();
        if (avail <= 0) {
            if (!d.stream->connected()) { _error = "connection closed"; break; }
            if (::millis() - d.lastData > 20000) { _error = "timeout"; break; }
            return _state;                     // nothing yet: come back next frame
        }
        const size_t want = std::min<size_t>(static_cast<size_t>(avail), sizeof d.buf);
        const int n = d.stream->read(d.buf, want);
        if (n <= 0) break;
        if (d.out.write(d.buf, n) != static_cast<size_t>(n)) { _error = "SD card write failed (full?)"; break; }
        _done += n;
        d.lastData = ::millis();
    }
    if (_error.empty() && _done < _total) return _state;   // budget used, more to come

    d.out.close();
    d.http.end();
    if (_error.empty()) {
        removeFile(d.path);                    // replace an older download
        if (renamePath(d.path + ".part", d.path)) _state = State::Done;
        else _error = "rename failed";
    }
    if (!_error.empty()) { removeFile(d.path + ".part"); _state = State::Failed; }
    log("[download] %s: %u bytes %s, heap %u\n", d.path.c_str(), static_cast<unsigned>(_done),
        _error.empty() ? "OK" : _error.c_str(), static_cast<unsigned>(ESP.getFreeHeap()));
    return _state;
}

void Download::cancel() {
    if (_state != State::Running) return;
    _impl->out.close();
    _impl->http.end();
    removeFile(_impl->path + ".part");
    _error = "cancelled";
    _state = State::Failed;
}

}  // namespace hal
