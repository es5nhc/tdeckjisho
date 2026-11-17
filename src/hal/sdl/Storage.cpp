// ============================================================================
//  hal/sdl/Storage.cpp – the "SD card" on the PC is a normal folder
// ============================================================================
//
//  By default ./sdcard (relative to where you start the program; `pio run -e
//  native -t exec` starts it in the project folder). Override with the
//  environment variable JISHO_SD=/some/other/folder.
//
//  This file uses only the C++ standard library, no SDL – so the unit tests
//  in tests/ can link it too and run the dictionary against real files.
// ============================================================================

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <thread>

#include "hal/Hal.hpp"

namespace fs = std::filesystem;   // a namespace alias, like `import x as y`

namespace {

std::string root() {
    const char* env = std::getenv("JISHO_SD");
    return env ? env : "sdcard";
}

// "/jisho/kana.idx" -> "sdcard/jisho/kana.idx"
std::string real(const std::string& sdPath) { return root() + sdPath; }

// Our opaque handle (`void* _h` in hal::File) is a C stdio FILE*.
FILE* fp(void* h) { return static_cast<FILE*>(h); }

hal::IoStats g_stats;

// Simulated SD-card cost of one read() call:
//   JISHO_SD_LATENCY_US per operation (command overhead, FAT walking...)
//   + the transfer itself at ~2.5 MB/s (SPI at 20 MHz) = 0.4 µs per byte.
// The env variable is read once: a function-local static is initialised the
// first time the function runs only.
void simulateLatency(size_t bytes) {
    static const long us = [] {
        const char* e = std::getenv("JISHO_SD_LATENCY_US");
        return e ? std::atol(e) : 0L;
    }();   // <- an immediately-invoked lambda: compute once, store result
    if (us > 0)
        std::this_thread::sleep_for(std::chrono::microseconds(us + static_cast<long>(bytes * 2 / 5)));
}

}  // namespace

namespace hal {

bool storageReady() { return fs::is_directory(root()); }

IoStats ioStats() { return g_stats; }
void resetIoStats() { g_stats = IoStats{}; }

bool File::open(const std::string& path) {
    close();
    _h = std::fopen(real(path).c_str(), "rb");   // "rb": read, binary (no \r\n games)
    return _h != nullptr;
}

void File::close() {
    if (_h) std::fclose(fp(_h));
    _h = nullptr;
}

size_t File::read(void* buf, size_t n) {
    if (!_h) return 0;
    simulateLatency(n);
    ++g_stats.reads;
    const size_t got = std::fread(buf, 1, n, fp(_h));
    g_stats.bytes += got;
    return got;
}

bool File::seek(uint32_t pos) {
    if (!_h) return false;
    ++g_stats.seeks;
    return std::fseek(fp(_h), static_cast<long>(pos), SEEK_SET) == 0;
}

uint32_t File::size() const {
    if (!_h) return 0;
    const long cur = std::ftell(fp(_h));
    std::fseek(fp(_h), 0, SEEK_END);
    const long end = std::ftell(fp(_h));
    std::fseek(fp(_h), cur, SEEK_SET);
    return static_cast<uint32_t>(end);
}

bool listDir(const std::string& path, std::vector<DirEntry>& out) {
    out.clear();
    std::error_code ec;   // the non-throwing overloads report errors here
    for (const auto& e : fs::directory_iterator(real(path), ec)) {
        DirEntry d;
        d.name = e.path().filename().string();
        d.isDir = e.is_directory(ec);
        d.size = d.isDir ? 0 : static_cast<uint32_t>(e.file_size(ec));
        out.push_back(std::move(d));
    }
    return !ec;
}

bool exists(const std::string& path) { return fs::exists(real(path)); }

bool readFile(const std::string& path, std::string& out, size_t maxBytes) {
    File f;
    if (!f.open(path)) return false;
    const uint32_t n = f.size();
    if (n > maxBytes) return false;
    out.assign(n, '\0');
    return f.read(&out[0], n) == n;
}

bool writeFile(const std::string& path, const std::string& data) {
    const std::string tmp = real(path) + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    std::error_code ec;
    if (ok) fs::rename(tmp, real(path), ec);   // atomic replace on POSIX
    return ok && !ec;
}

bool makeDirs(const std::string& path) {
    std::error_code ec;
    fs::create_directories(real(path), ec);
    return !ec;
}

// ---- writing (used by the on-device dictionary builder) ---------------------
bool WFile::open(const std::string& path) {
    close();
    _h = std::fopen(real(path).c_str(), "wb");
    return _h != nullptr;
}

void WFile::close() {
    if (_h) std::fclose(fp(_h));
    _h = nullptr;
}

size_t WFile::write(const void* buf, size_t n) {
    if (!_h) return 0;
    simulateLatency(n);                 // JISHO_SD_LATENCY_US applies to writes too
    ++g_stats.writes;
    const size_t put = std::fwrite(buf, 1, n, fp(_h));
    g_stats.bytesWritten += put;
    return put;
}

bool removeFile(const std::string& path) {
    std::error_code ec;
    return fs::remove(real(path), ec) && !ec;
}

bool renamePath(const std::string& from, const std::string& to) {
    // POSIX rename() would silently replace an existing FILE. The device's
    // FAT cannot, so refuse here too: the PC must not hide a device bug.
    if (fs::exists(real(to))) return false;
    std::error_code ec;
    fs::rename(real(from), real(to), ec);
    return !ec;
}

bool removeTree(const std::string& path) {
    if (path.empty() || path == "/") return false;   // never `rm -rf` the whole card
    std::error_code ec;
    fs::remove_all(real(path), ec);
    return !ec;
}

uint64_t freeBytes() {
    std::error_code ec;
    const fs::space_info s = fs::space(root(), ec);
    return ec ? 0 : s.available;
}

}  // namespace hal
