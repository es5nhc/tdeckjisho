// ============================================================================
//  hal/tdeck/Storage.cpp – the micro-SD card via Arduino's SD library
// ============================================================================
//
//  The SD card speaks SPI here (CS = GPIO 39), on the SAME bus as the
//  display. LovyanGFX already configured that bus in hal::begin(); the
//  Arduino SPI object (FSPI = SPI2 on the ESP32-S3) attaches to the same
//  hardware with the same pins.
//
//  Format the card as FAT32 (exFAT also works with Arduino-ESP32 2.x for
//  cards > 32 GB). Long file names and UTF-8 names are supported.
// ============================================================================

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

#include "hal/Hal.hpp"

namespace {

bool g_ready = false;
hal::IoStats g_stats;

// Our opaque handle is a heap-allocated fs::File (Arduino's file object).
fs::File* fh(void* h) { return static_cast<fs::File*>(h); }

}  // namespace

namespace hal {

void storageBegin() {
    SPI.begin(40, 38, 41, 39);   // SCK, MISO, MOSI, SS
    // Try fast first; some cards/wiring only work at lower clock speeds.
    for (uint32_t hz : {20000000u, 10000000u, 4000000u}) {
        // max_files = 10: how many files may be open AT THE SAME TIME. The
        // default is 5. The dictionary keeps 3 open, and the dictionary
        // builder needs 4 (source, entries.dat, two run files), plus a
        // settings write now and then. Too few, and open() simply fails.
        if (SD.begin(39, SPI, hz, "/sd", 10)) {
            g_ready = true;
            log("[sd] card OK at %u MHz, %llu MB\n", static_cast<unsigned>(hz / 1000000),
                SD.cardSize() / (1024ULL * 1024ULL));
            return;
        }
    }
    log("[sd] no card\n");
}

bool storageReady() { return g_ready; }

IoStats ioStats() { return g_stats; }
void resetIoStats() { g_stats = IoStats{}; }

bool File::open(const std::string& path) {
    close();
    if (!g_ready) return false;
    fs::File f = SD.open(path.c_str(), FILE_READ);
    if (!f || f.isDirectory()) return false;
    _h = new fs::File(f);    // fs::File is a small, copyable handle object
    return true;
}

void File::close() {
    if (!_h) return;
    fh(_h)->close();
    delete fh(_h);           // every `new` needs exactly one `delete`
    _h = nullptr;
}

size_t File::read(void* buf, size_t n) {
    if (!_h) return 0;
    ++g_stats.reads;
    const size_t got = fh(_h)->read(static_cast<uint8_t*>(buf), n);
    g_stats.bytes += got;
    return got;
}

bool File::seek(uint32_t pos) {
    if (!_h) return false;
    ++g_stats.seeks;
    return fh(_h)->seek(pos);
}

uint32_t File::size() const { return _h ? fh(_h)->size() : 0; }

bool listDir(const std::string& path, std::vector<DirEntry>& out) {
    out.clear();
    if (!g_ready) return false;
    fs::File dir = SD.open(path.c_str());
    if (!dir || !dir.isDirectory()) return false;
    while (fs::File e = dir.openNextFile()) {
        DirEntry d;
        // Arduino-ESP32 2.x returns just the name; 1.x returned the full
        // path. Taking everything after the last '/' works for both.
        std::string name = e.name();
        d.name = name.substr(name.rfind('/') + 1);
        d.isDir = e.isDirectory();
        d.size = d.isDir ? 0 : e.size();
        out.push_back(std::move(d));
    }
    return true;
}

bool exists(const std::string& path) { return g_ready && SD.exists(path.c_str()); }

bool readFile(const std::string& path, std::string& out, size_t maxBytes) {
    File f;
    if (!f.open(path)) return false;
    const uint32_t n = f.size();
    if (n > maxBytes) return false;
    // std::string's buffer for large files is allocated with malloc. With
    // PSRAM enabled, Arduino-ESP32 puts allocations bigger than 4 KB into
    // PSRAM automatically – so a 3 MB book does not touch internal RAM.
    out.assign(n, '\0');
    return f.read(&out[0], n) == n;
}

bool writeFile(const std::string& path, const std::string& data) {
    if (!g_ready) return false;
    const std::string tmp = path + ".tmp";
    fs::File f = SD.open(tmp.c_str(), FILE_WRITE);   // FILE_WRITE truncates
    if (!f) return false;
    const bool ok = f.write(reinterpret_cast<const uint8_t*>(data.data()), data.size()) == data.size();
    f.close();
    if (!ok) return false;
    SD.remove(path.c_str());   // FAT cannot rename over an existing file
    return SD.rename(tmp.c_str(), path.c_str());
}

bool makeDirs(const std::string& path) {
    if (!g_ready) return false;
    // mkdir -p: create every prefix "/a", "/a/b", "/a/b/c".
    for (size_t pos = 1; pos <= path.size(); ++pos) {
        if (pos == path.size() || path[pos] == '/') {
            const std::string prefix = path.substr(0, pos);
            if (!SD.exists(prefix.c_str()) && !SD.mkdir(prefix.c_str())) return false;
        }
    }
    return true;
}

// ---- writing (used by the on-device dictionary builder) ---------------------
bool WFile::open(const std::string& path) {
    close();
    if (!g_ready) return false;
    fs::File f = SD.open(path.c_str(), FILE_WRITE);   // FILE_WRITE = create/truncate
    if (!f) return false;
    _h = new fs::File(f);
    return true;
}

void WFile::close() {
    if (!_h) return;
    fh(_h)->close();         // writes the last sector and updates the directory entry
    delete fh(_h);
    _h = nullptr;
}

size_t WFile::write(const void* buf, size_t n) {
    if (!_h) return 0;
    ++g_stats.writes;
    const size_t put = fh(_h)->write(static_cast<const uint8_t*>(buf), n);
    g_stats.bytesWritten += put;
    return put;
}

bool removeFile(const std::string& path) { return g_ready && SD.remove(path.c_str()); }

bool renamePath(const std::string& from, const std::string& to) {
    return g_ready && !SD.exists(to.c_str()) && SD.rename(from.c_str(), to.c_str());
}

bool removeTree(const std::string& path) {
    if (!g_ready || path.empty() || path == "/") return false;
    fs::File dir = SD.open(path.c_str());
    if (!dir) return true;                       // already gone
    if (!dir.isDirectory()) { dir.close(); return SD.remove(path.c_str()); }
    // Collect the names first, then delete: removing entries while
    // openNextFile() walks the same directory is asking for trouble.
    std::vector<std::string> names;
    while (fs::File e = dir.openNextFile()) {
        std::string name = e.name();
        names.push_back(path + "/" + name.substr(name.rfind('/') + 1));
    }
    dir.close();
    bool ok = true;
    for (const auto& n : names) ok = removeTree(n) && ok;   // recursion handles sub-folders
    return SD.rmdir(path.c_str()) && ok;
}

uint64_t freeBytes() { return g_ready ? SD.totalBytes() - SD.usedBytes() : 0; }

}  // namespace hal
