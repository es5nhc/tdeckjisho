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

// Send a GET and follow up to 5 redirects BY HAND. HTTPClient can do it, but not
// from http:// to https:// (that needs a different client object), and every hop
// must pass our policy (url::redirectProblem). Shared by Fetch and Download.
//
// On return `current` is the final URL and, if the result is > 0, `http` holds that
// response (headers read, body not yet): the caller reads it, then calls http.end().
// A result <= 0 means failure, with `error` set and `http` already ended.
// `http10`: ask for HTTP/1.0, so the server sends the body as one plain stream.
// HTTP/1.1 servers may send it "chunked" (pieces with size headers in between),
// which only HTTPClient's blocking writeToStream() decodes for us.
int beginGet(HTTPClient& http, WiFiClient& plain, WiFiClientSecure& secure, std::string& current,
             const char* userAgent, bool http10, std::string& error) {
    if (!url::isWebUrl(current)) { error = "refused: not an http(s) address"; return -1; }
    for (int hop = 0; hop < 5; ++hop) {
        const bool https = current.rfind("https://", 0) == 0;
        if (https) {
            // Fail closed: without trusted roots we cannot check the server.
            if (kCaBundleCerts == 0) { error = "no CA certificates in this build (see README)"; return -1; }
            secure.setCACertBundle(kCaBundle);   // verify the server's certificate chain
        }
        // `?:` cannot mix WiFiClientSecure and WiFiClient, so pick by reference:
        WiFiClient& client = https ? static_cast<WiFiClient&>(secure) : plain;
        http.setTimeout(10000);              // per read, incl. waiting for the headers
        http.setUserAgent(userAgent);
        http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
        http.useHTTP10(http10);
        const char* keep[] = {"Location", "Content-Type"};
        http.collectHeaders(keep, 2);       // HTTPClient discards headers unless asked
        if (!http.begin(client, current.c_str())) { error = "bad URL"; return -1; }

        const int code = http.GET();         // BLOCKS: DNS, TCP, TLS, headers
        if (code <= 0) {
            error = http.errorToString(code).c_str();
            http.end();
            return code < 0 ? code : -1;
        }
        const bool redirect = code == 301 || code == 302 || code == 303 || code == 307 || code == 308;
        if (!redirect || http.header("Location").length() == 0) return code;
        const std::string next = url::resolve(current, http.header("Location").c_str());
        http.end();
        error = url::redirectProblem(current, next);
        if (!error.empty()) return -1;
        current = next;
    }
    error = "too many redirects";
    return -1;
}

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
    // The ESP32 cannot scan while it is still trying to JOIN a network:
    // scanNetworks() then fails at once. And with auto-reconnect on, that
    // attempt never ends when the saved network is out of range (Arduino-ESP32
    // calls WiFi.begin() again after every "no AP found"). So a scan request
    // abandons the attempt. Connected is fine: scanning while connected works.
    if (WiFi.status() != WL_CONNECTED && g_state != WifiState::Off) {
        WiFi.setAutoReconnect(false);    // no retry from a queued disconnect event
        WiFi.disconnect();               // wifioff=false: the radio stays on
        g_state = WifiState::Off;
        log("[wifi] connection attempt dropped for a scan\n");
    }
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

// ---- Fetch: a page into memory, the body read a few KB per poll() -----------
struct Fetch::Impl {
    HTTPClient http;
    WiFiClient plain;
    WiFiClientSecure secure;
    WiFiClient* stream = nullptr;
    uint32_t lastData = 0;
    std::string url;
    uint8_t buf[2048];
};

Fetch::Fetch() = default;
Fetch::~Fetch() = default;

void Fetch::start(const std::string& startUrl, size_t maxBytes) {
    _impl = std::make_unique<Impl>();
    _result = HttpResult{};
    _total = 0;
    _limit = maxBytes;
    _state = State::Done;                       // until a body is on its way
    Impl& d = *_impl;
    d.url = startUrl;
    std::string current = startUrl;
    const int code = beginGet(d.http, d.plain, d.secure, current, "TDeckJisho/" JISHO_VERSION " (ESP32; text reader)",
                              /*http10=*/true, _result.error);
    _result.status = code;
    _result.finalUrl = current;
    if (code <= 0) { log("[http] GET %s -> %s\n", startUrl.c_str(), _result.error.c_str()); return; }
    _result.contentType = d.http.header("Content-Type").c_str();
    if (code != 200) {                          // 404 & co: the caller only shows the code
        d.http.end();
        log("[http] GET %s -> %d\n", startUrl.c_str(), code);
        return;
    }
    const int size = d.http.getSize();          // -1 = no Content-Length: read until closed
    if (size > 0) _total = static_cast<size_t>(size);
    _result.body.reserve(std::min(_total ? _total : 64 * 1024, _limit));
    d.stream = d.http.getStreamPtr();
    d.lastData = ::millis();
    _state = State::Running;
}

Fetch::State Fetch::poll() {
    if (_state != State::Running) return _state;
    Impl& d = *_impl;
    std::string& body = _result.body;
    const uint32_t t0 = ::millis();
    bool finished = false;
    // Read for at most ~25 ms per call, so the UI keeps drawing.
    while (::millis() - t0 < 25) {
        if (_total && body.size() >= _total) { finished = true; break; }
        const int avail = d.stream->available();
        if (avail <= 0) {
            if (!d.stream->connected()) {        // HTTP/1.0: the end of the body = the server closes
                if (_total && body.size() < _total) { _result.status = -1; _result.error = "connection closed"; }
                finished = true;
                break;
            }
            if (::millis() - d.lastData > 15000) { _result.status = -1; _result.error = "timeout"; finished = true; break; }
            return _state;                       // nothing yet: come back next frame
        }
        const int n = d.stream->read(d.buf, std::min<size_t>(static_cast<size_t>(avail), sizeof d.buf));
        if (n <= 0) break;
        d.lastData = ::millis();
        if (body.size() + n >= _limit) {          // enough: keep what fits, drop the rest
            body.append(reinterpret_cast<const char*>(d.buf), _limit - body.size());
            _result.truncated = true;
            finished = true;
            break;
        }
        body.append(reinterpret_cast<const char*>(d.buf), n);
    }
    if (!finished) return _state;
    d.http.end();
    _state = State::Done;
    log("[http] GET %s -> %d, %u bytes%s, heap %u\n", d.url.c_str(), _result.status,
        static_cast<unsigned>(body.size()), _result.truncated ? " (truncated)" : "",
        static_cast<unsigned>(ESP.getFreeHeap()));
    return _state;
}

void Fetch::cancel() {
    if (_state != State::Running) return;
    _impl->http.end();                          // closes the socket
    _result.status = -1;
    _result.error = "cancelled";
    _state = State::Done;
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
//  The same idea as Fetch above, but straight into a file on the SD card: a
//  10 MB file does not fit in RAM. We take the socket ("stream") and read only
//  what has already arrived, every frame.
//
//  Limitation, on purpose: the server must send Content-Length, and we ask with
//  HTTP/1.1 (unlike Fetch). Without a length we could not show a real progress
//  bar or tell a finished file from a broken connection. Static files (EDRDG,
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
    // 0.2–2 s); only the body is read without blocking.
    std::string current = startUrl;
    const int code = beginGet(d.http, d.plain, d.secure, current,
                              "TDeckJisho/" JISHO_VERSION " (ESP32; dictionary download)", /*http10=*/false, _error);
    if (code <= 0) return false;
    if (code != 200) { _error = "HTTP " + std::to_string(code); d.http.end(); return false; }
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
