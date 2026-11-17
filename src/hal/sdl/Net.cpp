// ============================================================================
//  hal/sdl/Net.cpp – networking on the PC
// ============================================================================
//
//  Wi-Fi is simulated: the PC is already online, so "connecting" just
//  succeeds after a short delay. The scan returns two fake networks so the
//  Wi-Fi screen can be tried out.
//
//  HTTP uses libcurl, the C library behind the `curl` command. Its "easy"
//  API: create a handle, set options, perform, read results, clean up.
//  Python equivalent: requests.get(url, timeout=15, allow_redirects=True)
// ============================================================================

#include <fcntl.h>      // open(): create the secrets file with permissions 0600
#include <sys/stat.h>   // mkdir()
#include <unistd.h>     // write(), close()

#include <cstdio>
#include <cstdlib>

#include <curl/curl.h>

#include "core/Url.hpp"
#include "hal/Hal.hpp"

#ifndef JISHO_VERSION            // set by tools/name_firmware.py from platformio.ini
#define JISHO_VERSION "dev"
#endif

namespace {

hal::WifiState g_state = hal::WifiState::Connected;   // PC: online from the start
std::string g_ssid = "(PC network)";
uint32_t g_connectAt = 0;
uint32_t g_scanDoneAt = 0;
bool g_scanning = false;

struct Sink {
    std::string* body;
    size_t max;
    bool truncated = false;
};

// libcurl calls this for every chunk of the body. It is a plain C callback,
// so our state comes in through the `void* userdata` pointer.
// SECURITY: libcurl speaks some 25 protocols, including file:// (read any file on
// this PC) and ftp://. We want exactly two, for the first request AND redirects.
// `httpsOnly`: for transfers that let libcurl follow redirects itself (the
// Download below): once we start on https, every redirect must stay on https.
void webOnly(CURL* c, bool httpsOnly = false) {
#if LIBCURL_VERSION_NUM >= 0x075500   // 7.85.0 introduced the string form
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, httpsOnly ? "https" : "http,https");
#else
    curl_easy_setopt(c, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, httpsOnly ? CURLPROTO_HTTPS : CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
    // libcurl checks certificates by default. We say it out loud anyway, so nobody
    // "fixes" a failing site by switching it off: that would silently make the PC
    // build trust anyone, while the device still refuses.
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
}

size_t onData(char* data, size_t size, size_t n, void* userdata) {
    Sink* s = static_cast<Sink*>(userdata);
    const size_t len = size * n;
    if (s->body->size() + len > s->max) {
        s->truncated = true;
        return 0;   // returning less than len tells curl to abort the transfer
    }
    s->body->append(data, len);
    return len;
}

}  // namespace

namespace hal {

void wifiConnect(const std::string& ssid, const std::string&) {
    g_ssid = ssid;
    g_state = WifiState::Connecting;
    g_connectAt = millis() + 800;
}

void wifiDisconnect() { g_state = WifiState::Off; }

WifiState wifiState() {
    if (g_state == WifiState::Connecting && millis() >= g_connectAt) g_state = WifiState::Connected;
    return g_state;
}

std::string wifiSsid() { return g_ssid; }
std::string wifiIp() { return "127.0.0.1"; }

bool wifiScanStart() {
    g_scanning = true;
    g_scanDoneAt = millis() + 1000;
    return true;
}

int wifiScanPoll(std::vector<WifiNetwork>& out) {
    if (!g_scanning || millis() < g_scanDoneAt) return -1;
    g_scanning = false;
    out = {{"(PC network)", -40, false}, {"Naabri WiFi", -78, true}};
    return static_cast<int>(out.size());
}

HttpResult httpGet(const std::string& startUrl, size_t maxBytes) {
    HttpResult r;
    if (!url::isWebUrl(startUrl)) { r.error = "refused: not an http(s) address"; return r; }
    std::string current = startUrl;

    // Redirects by hand, exactly like the device: every hop passes the same policy
    // (url::redirectProblem), so the simulator is never more trusting than the T-Deck.
    for (int hop = 0; hop < 5; ++hop) {
        CURL* c = curl_easy_init();
        if (!c) { r.error = "curl init failed"; return r; }
        r.body.clear();
        Sink sink{&r.body, maxBytes};
        curl_easy_setopt(c, CURLOPT_URL, current.c_str());
        webOnly(c);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);       // we follow them ourselves
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(c, CURLOPT_USERAGENT, "TDeckJisho/" JISHO_VERSION " (text reader)");
        curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");      // allow gzip, auto-decoded
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, onData);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);

        const CURLcode res = curl_easy_perform(c);
        long code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        char* ct = nullptr;
        curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &ct);
        char* loc = nullptr;                                   // absolute Location, if any
        curl_easy_getinfo(c, CURLINFO_REDIRECT_URL, &loc);

        r.status = static_cast<int>(code);
        r.contentType = ct ? ct : "";
        r.finalUrl = current;
        r.truncated = sink.truncated;
        const std::string next = loc ? loc : "";
        curl_easy_cleanup(c);                                  // ct and loc die here
        if (res != CURLE_OK && !sink.truncated) {
            r.error = curl_easy_strerror(res);
            if (r.status == 200) r.status = -1;
            break;
        }
        if ((code == 301 || code == 302 || code == 303 || code == 307 || code == 308) && !next.empty()) {
            r.error = url::redirectProblem(current, next);
            if (!r.error.empty()) { r.status = -1; break; }
            current = next;
            continue;
        }
        break;
    }
    log("[http] GET %s -> %d, %u bytes\n", startUrl.c_str(), r.status, static_cast<unsigned>(r.body.size()));
    return r;
}

// ---- secrets: a private file, NOT inside the simulated SD card -----------------
// $JISHO_SECRETS, or ./.jisho-secrets: a folder only you may enter (0700), files only
// you may read (0600). The device keeps them in NVS instead (hal/tdeck/Net.cpp).
namespace {
bool validSecretName(const std::string& name) {
    if (name.empty() || name.size() > 15) return false;          // same rule as NVS
    for (char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}
std::string secretDir() {
    const char* env = std::getenv("JISHO_SECRETS");
    return env ? env : ".jisho-secrets";
}
}  // namespace

bool secretRead(const std::string& name, std::string& out) {
    out.clear();
    if (!validSecretName(name)) return false;
    std::FILE* f = std::fopen((secretDir() + "/" + name).c_str(), "rb");
    if (!f) return false;
    char buf[512];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && out.size() < 4096) out.append(buf, n);
    std::fclose(f);
    return !out.empty();
}

bool secretWrite(const std::string& name, const std::string& value) {
    if (!validSecretName(name) || value.size() > 3000) return false;   // the NVS limit, for parity
    const std::string path = secretDir() + "/" + name;
    if (value.empty()) { std::remove(path.c_str()); return true; }
    ::mkdir(secretDir().c_str(), 0700);
    // open() with O_CREAT and mode 0600: the file is born private (fopen() would create
    // it readable by everyone first, and a chmod afterwards leaves a window open).
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    const bool ok = ::write(fd, value.data(), value.size()) == static_cast<ssize_t>(value.size());
    ::close(fd);
    return ok;
}

// ============================================================================
//  Download – libcurl's "multi" interface = curl without blocking
// ============================================================================
//  curl_easy_perform() (above) blocks until the transfer is over. The multi
//  interface instead lets US call curl_multi_perform() whenever we like; each
//  call moves the transfer forward by whatever the socket has ready and returns
//  at once. That is exactly the shape of the device version (read what is
//  available, return), so the BuildScreen cannot tell the two apart.
// ============================================================================
struct Download::Impl {
    CURL* easy = nullptr;
    CURLM* multi = nullptr;
    FILE* out = nullptr;
    std::string path;       // final SD path
    std::string partReal;   // real PC path of "<path>.part"
    uint32_t* done = nullptr;

    // curl -> file. Also counts bytes for the progress bar.
    static size_t onBody(char* data, size_t size, size_t n, void* user) {
        Impl* self = static_cast<Impl*>(user);
        const size_t len = size * n;
        if (*self->done + len > kMaxBytes) return 0;   // a server without Content-Length
        const size_t put = std::fwrite(data, 1, len, self->out);
        *self->done += static_cast<uint32_t>(put);
        return put;          // less than len = "abort" for curl (disk full...)
    }

    void cleanup() {
        if (multi && easy) curl_multi_remove_handle(multi, easy);
        if (easy) curl_easy_cleanup(easy);
        if (multi) curl_multi_cleanup(multi);
        if (out) std::fclose(out);
        easy = nullptr; multi = nullptr; out = nullptr;
    }
    ~Impl() { cleanup(); }
};

Download::Download() = default;
Download::~Download() = default;   // here, Impl is a complete type: unique_ptr can delete it

bool Download::start(const std::string& url, const std::string& path) {
    _impl = std::make_unique<Impl>();
    _done = _total = 0;
    _error.clear();
    if (!url::isWebUrl(url)) { _error = "refused: not an http(s) address"; _state = State::Failed; return false; }
    Impl& d = *_impl;
    d.path = path;
    d.done = &_done;
    const char* env = std::getenv("JISHO_SD");
    d.partReal = std::string(env ? env : "sdcard") + path + ".part";
    d.out = std::fopen(d.partReal.c_str(), "wb");
    if (!d.out) { _error = "cannot create " + path + ".part"; _state = State::Failed; return false; }

    d.easy = curl_easy_init();
    d.multi = curl_multi_init();
    curl_easy_setopt(d.easy, CURLOPT_URL, url.c_str());
    // libcurl follows the redirects here (the multi interface makes doing it by hand
    // awkward), restricted to the same policy: web only, and https stays https.
    webOnly(d.easy, /*httpsOnly=*/url::isHttps(url));
    curl_easy_setopt(d.easy, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(kMaxBytes));
    curl_easy_setopt(d.easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(d.easy, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(d.easy, CURLOPT_USERAGENT, "TDeckJisho/" JISHO_VERSION " (dictionary download)");
    // NOT CURLOPT_ACCEPT_ENCODING here (httpGet uses it). Some servers label a
    // .gz file "Content-Encoding: gzip", and curl would then quietly unpack it:
    // the PC would get XML while the device gets gzip. The builder checks the
    // magic bytes anyway, but the two targets should not differ for no reason.
    curl_easy_setopt(d.easy, CURLOPT_WRITEFUNCTION, Impl::onBody);
    curl_easy_setopt(d.easy, CURLOPT_WRITEDATA, &d);
    curl_easy_setopt(d.easy, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(d.easy, CURLOPT_LOW_SPEED_LIMIT, 1L);     // abort if < 1 byte/s...
    curl_easy_setopt(d.easy, CURLOPT_LOW_SPEED_TIME, 20L);     // ...for 20 s
    if (const char* kbps = std::getenv("JISHO_NET_KBPS"))      // pretend to be Wi-Fi
        curl_easy_setopt(d.easy, CURLOPT_MAX_RECV_SPEED_LARGE,
                         static_cast<curl_off_t>(std::atol(kbps)) * 1024);
    curl_multi_add_handle(d.multi, d.easy);
    _state = State::Running;
    log("[download] %s -> %s\n", url.c_str(), path.c_str());
    return true;
}

Download::State Download::poll() {
    if (_state != State::Running) return _state;
    Impl& d = *_impl;
    int running = 0;
    curl_multi_perform(d.multi, &running);   // returns immediately

    curl_off_t len = -1;
    curl_easy_getinfo(d.easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &len);
    if (len > 0) _total = static_cast<uint32_t>(len);
    if (running) return _state;

    // Finished – successfully or not. Ask curl how it went.
    CURLcode result = CURLE_OK;
    int left = 0;
    while (CURLMsg* m = curl_multi_info_read(d.multi, &left))
        if (m->msg == CURLMSG_DONE) result = m->data.result;
    long code = 0;
    curl_easy_getinfo(d.easy, CURLINFO_RESPONSE_CODE, &code);
    d.cleanup();                              // also closes the .part file

    if (result != CURLE_OK) _error = curl_easy_strerror(result);
    else if (code != 200) _error = "HTTP " + std::to_string(code);
    else if (_total && _done != _total) _error = "incomplete download";
    if (!_error.empty()) {
        std::remove(d.partReal.c_str());
        _state = State::Failed;
    } else {
        removeFile(d.path);                   // replace an older download
        _state = renamePath(d.path + ".part", d.path) ? State::Done : State::Failed;
        if (_state == State::Failed) _error = "rename failed";
    }
    log("[download] %s: %u bytes %s\n", d.path.c_str(), static_cast<unsigned>(_done),
        _error.empty() ? "OK" : _error.c_str());
    return _state;
}

void Download::cancel() {
    if (_state != State::Running) return;
    _impl->cleanup();
    std::remove(_impl->partReal.c_str());
    _error = "cancelled";
    _state = State::Failed;
}

}  // namespace hal
