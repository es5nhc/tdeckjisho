#pragma once
// ============================================================================
//  Hal.hpp – the Hardware Abstraction Layer (HAL) interface
// ============================================================================
//
//  Everything the application needs from "the machine" is declared here, and
//  ONLY declared. There are two implementations:
//
//      src/hal/tdeck/*.cpp   – real hardware (Arduino-ESP32: SD, WiFi, I2C...)
//      src/hal/sdl/*.cpp     – PC simulator (SDL2 window, std::filesystem, curl)
//
//  platformio.ini compiles exactly one of the two folders. The linker then
//  connects calls like hal::millis() to whichever implementation was built.
//
//  This is "link-time polymorphism". Compare with your IClock interface in the
//  main project, which uses `virtual` functions ("run-time polymorphism").
//  Both are valid:
//    * virtual  – you can choose the implementation while the program runs,
//                 costs one pointer indirection per call, needs an object.
//    * link-time – the choice is made when building; zero overhead; but there
//                 can only be ONE implementation in a given binary.
//  For "which board am I running on" link-time is the natural fit: the answer
//  never changes while the program runs.
//
//  Rust analogy: a module with `#[cfg(target_os = "...")]` alternatives.
//  Python analogy: `if sys.platform == ...: from .tdeck import *`.
// ============================================================================

#include <cstddef>   // size_t
#include <cstdint>   // uint32_t and friends: integers with a KNOWN size
#include <memory>    // std::unique_ptr (hal::Download)
#include <string>
#include <vector>

// A FORWARD DECLARATION: "a class called lgfx::LGFX_Device exists". That is
// enough to declare a function returning a reference to it, and it means
// files that include Hal.hpp do not have to pull in all of LovyanGFX. This
// keeps the dictionary/text code compilable in the plain unit tests
// (tests/), which do not link a graphics library at all.
// (LovyanGFX puts everything in `lgfx::v1`, made transparent by `inline`.)
namespace lgfx { inline namespace v1 { class LGFX_Device; } }

namespace hal {

// ---------------------------------------------------------------------------
//  Board lifecycle, time, logging
// ---------------------------------------------------------------------------

// Power up peripherals, start the display, keyboard, SD card. Call once.
void begin();

// Milliseconds since boot. Wraps around after ~49 days (uint32_t overflow);
// always compare durations as `now - then > limit`, which works across the
// wrap thanks to unsigned arithmetic. (`now > then + limit` does NOT.)
uint32_t millis();

void sleepMs(uint32_t ms);

// printf-style logging to the serial monitor (device) or terminal (PC).
// The `__attribute__((format...))` lets GCC check the format string against
// the arguments, exactly like it does for printf itself.
void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// The physical display. The UI never draws on it directly – it draws into an
// off-screen sprite and copies that here (see ui/App.cpp for why).
lgfx::LGFX_Device& display();

void setBacklight(uint8_t level);   // 0 = off, 255 = full (used for dimming)

// Screen off / on, with ANTI-GHOSTING. An LCD that keeps showing the same
// static picture (even with the backlight off, the panel still drives the
// pixels) can develop a faint after-image ("image retention"). displayOff()
// therefore paints the panel BLACK first, then turns off the backlight AND
// puts the panel controller to sleep. displayOn() wakes it; the caller then
// redraws a full frame.
void displayOff();
void displayOn();

// Battery charge 0..100, or -1 if unknown (PC, or no battery connected).
int batteryPercent();

// True once the wall clock is trustworthy (NTP synced on device; always on PC).
bool timeValid();

struct MemInfo {
    uint32_t freeInternal = 0;   // fast on-chip RAM (~300 KB on ESP32-S3)
    uint32_t freePsram    = 0;   // slower external RAM (8 MB on T-Deck)
    uint32_t totalPsram   = 0;
};
MemInfo memInfo();

// ---------------------------------------------------------------------------
//  Input – returns one key code from hal/Keys.hpp per call, key::None if idle.
// ---------------------------------------------------------------------------
int pollKey();

// A tap on the screen (a finger on the device, a mouse click in the PC simulator) as a screen
// position, x 0..319 and y 0..239. Returns true once per tap; false when there is none.
// On the T-Deck the GT911 touch controller is read over the keyboard's I2C bus (docs/04-hardware.md).
// Holding a finger down gives ONE tap; lift and touch again for the next.
bool pollTap(int& x, int& y);

// One line for the touch-test screen: is a touch chip there, and what was the last raw → screen point.
const char* touchStatus();

// ---------------------------------------------------------------------------
//  Storage (SD card on device, ./sdcard/ folder on the PC)
//
//  Paths are always absolute SD paths such as "/tdeckjisho/jisho/kana.idx". The PC
//  implementation prefixes them with its sdcard folder.
// ---------------------------------------------------------------------------
bool storageReady();

// I/O statistics for performance work. On an SD card the NUMBER of
// operations matters far more than the bytes: every seek+read costs
// roughly a millisecond of SPI commands and FAT-chain walking, whether it
// reads 48 bytes or 4 KB. Counting them makes that visible on the PC too.
//
// The PC build can also SIMULATE that cost: set JISHO_SD_LATENCY_US=1000 and
// every read() sleeps 1 ms, so the simulator feels like the real device.
struct IoStats {
    uint32_t reads = 0;       // read() calls that hit the "card"
    uint32_t seeks = 0;
    uint64_t bytes = 0;
    uint32_t writes = 0;      // WFile::write() calls (the on-device dictionary builder)
    uint64_t bytesWritten = 0;
};
IoStats ioStats();
void resetIoStats();

// A read-only, random-access file handle.
//
// Why a class and not just functions? Because a file must be CLOSED when we
// are done with it. A C++ destructor (~File) runs automatically when the
// object goes out of scope, so forgetting to close is impossible. This idiom
// is called RAII ("Resource Acquisition Is Initialisation"). Rust's `Drop`
// and Python's `with open(...) as f:` solve the same problem.
class File {
public:
    File() = default;
    ~File() { close(); }

    // Copying a file handle would give two owners of one OS resource and a
    // double close. So we forbid copies (`= delete`)...
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    // ...but allow MOVES: ownership is transferred and the source is emptied.
    // This is what Rust does by default for every non-Copy type.
    File(File&& other) noexcept : _h(other._h) { other._h = nullptr; }
    File& operator=(File&& other) noexcept {
        if (this != &other) { close(); _h = other._h; other._h = nullptr; }
        return *this;
    }

    bool open(const std::string& path);         // implemented per platform
    void close();                               // implemented per platform
    bool isOpen() const { return _h != nullptr; }
    size_t read(void* buf, size_t n);           // returns bytes actually read
    bool seek(uint32_t pos);                    // absolute position
    uint32_t size() const;

private:
    // "Opaque handle": a pointer to fs::File on the device, FILE* on the PC.
    // The header does not need to know which – only the .cpp files do.
    void* _h = nullptr;
};

// Reads a whole file. Fails (returns false) if it is larger than maxBytes.
bool readFile(const std::string& path, std::string& out, size_t maxBytes);

struct DirEntry {
    std::string name;     // file name only, no directory part
    bool isDir = false;
    uint32_t size = 0;
};

bool listDir(const std::string& path, std::vector<DirEntry>& out);
bool exists(const std::string& path);
// Writes a whole file safely: write "path.tmp" first, then rename over the
// original, so a power cut never leaves a half-written settings file.
bool writeFile(const std::string& path, const std::string& data);
bool makeDirs(const std::string& path);         // like `mkdir -p`

// A write-only file handle, for files too big to build in RAM first (the
// on-device dictionary builder writes ~130 MB). Same RAII rules as File.
//
// Every write() is one trip through the SD driver and the FAT layer, so
// write in BIG pieces (8–32 KB). dict/BuildIo.hpp has a BufferedWriter that
// turns many small writes into a few big ones.
class WFile {
public:
    WFile() = default;
    ~WFile() { close(); }
    WFile(const WFile&) = delete;
    WFile& operator=(const WFile&) = delete;
    WFile(WFile&& other) noexcept : _h(other._h) { other._h = nullptr; }
    WFile& operator=(WFile&& other) noexcept {
        if (this != &other) { close(); _h = other._h; other._h = nullptr; }
        return *this;
    }

    bool open(const std::string& path);         // create, or truncate an existing file
    void close();                               // flushes; safe to call twice
    bool isOpen() const { return _h != nullptr; }
    size_t write(const void* buf, size_t n);    // returns bytes actually written

private:
    void* _h = nullptr;
};

bool removeFile(const std::string& path);
// FAT cannot rename onto an existing name: remove the target first.
bool renamePath(const std::string& from, const std::string& to);
// `rm -r`: a folder with everything in it. Used to throw away a failed or an
// old dictionary build. Handle with care.
bool removeTree(const std::string& path);
// Free space on the card in bytes (0 if unknown). On the device the first call
// can take a second or two: FAT has to count the free clusters.
uint64_t freeBytes();

// ---------------------------------------------------------------------------
//  Network
// ---------------------------------------------------------------------------
enum class WifiState { Off, Connecting, Connected, Failed };

struct WifiNetwork {
    std::string ssid;
    int rssi = 0;          // signal strength in dBm: -50 great, -85 poor
    bool secured = true;
};

void wifiConnect(const std::string& ssid, const std::string& password);
void wifiDisconnect();
WifiState wifiState();
std::string wifiSsid();
std::string wifiIp();

// Scanning takes 2–4 seconds. Instead of blocking the UI we START a scan and
// then POLL it from the main loop. Returns -1 while still running, otherwise
// the number of networks written into `out`.
bool wifiScanStart();
int wifiScanPoll(std::vector<WifiNetwork>& out);

struct HttpResult {
    int status = 0;            // 200 = OK; <= 0 means a transport error
    std::string body;          // raw bytes as received (usually UTF-8)
    std::string contentType;   // e.g. "text/html; charset=utf-8"
    std::string finalUrl;      // after redirects – needed to resolve links
    std::string error;         // human-readable error text if status != 200
    bool truncated = false;    // body was cut at maxBytes
};

// BLOCKING HTTP(S) GET with redirects. The UI shows a "Loading…" screen
// before calling it (see screens/LoadingScreen.hpp).
// Only http/https (url::isWebUrl), checked again on every redirect; https
// certificates are always verified. Anything else fails with r.error set.
HttpResult httpGet(const std::string& url, size_t maxBytes);

// Secrets (Wi-Fi passwords) never go on the SD card: anyone who borrows the card
// reads it on a PC in ten seconds. Device: the ESP32's NVS ("non-volatile storage",
// a key-value store in internal flash). PC: a file outside the simulated SD card
// that only your user may read. `name` is a short identifier: [a-z0-9_], 1–15 chars.
bool secretRead(const std::string& name, std::string& out);   // false = none stored
bool secretWrite(const std::string& name, const std::string& value);

// A NON-BLOCKING download straight into a file: for things far bigger than
// RAM (JMdict_e.gz is ~10 MB). Same start-then-poll pattern as the Wi-Fi scan:
//
//     hal::Download dl;
//     dl.start(url, "/jmdict/JMdict_e.gz");
//     ...every frame:  if (dl.poll() != hal::Download::State::Running) done
//
// The body goes to "<path>.part", which is renamed to <path> only once every
// byte has arrived, so a cancelled or broken download never looks complete.
//
// Device: HTTPClient's raw stream, read a few KB per poll().
// PC:     libcurl's "multi" interface, which is libcurl's own non-blocking API.
// Set JISHO_NET_KBPS=500 on the PC to limit the speed to roughly real Wi-Fi.
//
// Implementation detail hidden behind a pointer ("pimpl" idiom): the device
// needs HTTPClient objects, the PC needs CURL handles. Neither type may
// appear in this header, so `Impl` is only DECLARED here and defined in each
// platform's Net.cpp. (Rust: a Box<dyn Trait> with a private concrete type.)
class Download {
public:
    enum class State { Idle, Running, Done, Failed };

    Download();
    ~Download();                     // defined in Net.cpp, where Impl is complete
    Download(const Download&) = delete;
    Download& operator=(const Download&) = delete;

    // Same policy as httpGet: http/https only, every redirect checked, certificates
    // verified. A server may claim any size: anything above kMaxBytes is refused
    // (JMdict_e.gz is ~10 MB, the unpacked XML ~60 MB), so it can't fill the card.
    static constexpr uint32_t kMaxBytes = 128u * 1024 * 1024;
    bool start(const std::string& url, const std::string& path);
    State poll();                    // does a little work; call every frame
    void cancel();                   // stops, deletes the .part file
    State state() const { return _state; }
    uint32_t done() const { return _done; }
    uint32_t total() const { return _total; }    // 0 = server did not say
    const std::string& error() const { return _error; }

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
    State _state = State::Idle;
    uint32_t _done = 0, _total = 0;
    std::string _error;
};

}  // namespace hal
