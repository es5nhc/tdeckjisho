#include "core/WifiStore.hpp"

#include <algorithm>

#include "core/Paths.hpp"
#include "core/Str.hpp"
#include "hal/Hal.hpp"

namespace {

const char* const kSecret = "wifi";   // the NVS key

// Can this network be stored in our TAB/line format without changing its meaning?
// 802.11 allows ANY bytes in an SSID (up to 32) – including TAB and '\n', which would
// split our line in the wrong place and invent a network. Refuse instead of guessing.
bool storable(const WifiCred& c) {
    // WPA: a passphrase of 8–63 characters, or the raw key as 64 hex digits.
    if (c.ssid.empty() || c.ssid.size() > 32 || c.password.size() > 64) return false;
    for (const std::string* s : {&c.ssid, &c.password})
        for (char ch : *s)
            if (static_cast<uint8_t>(ch) < 0x20) return false;
    return true;
}

// The same parser for NVS text and the SD import file.
std::vector<WifiCred> parse(std::string_view text) {
    std::vector<WifiCred> out;
    for (std::string_view line : str::split(text, '\n')) {
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line[0] == '#') continue;
        const size_t tab = line.find('\t');
        WifiCred c;
        c.ssid = std::string(tab == std::string_view::npos ? line : line.substr(0, tab));
        if (tab != std::string_view::npos) c.password = std::string(line.substr(tab + 1));
        if (storable(c) && out.size() < kMaxWifiNetworks) out.push_back(std::move(c));
    }
    return out;
}

}  // namespace

std::vector<WifiCred> loadWifiCreds() {
    std::string text;
    if (!hal::secretRead(kSecret, text)) return {};
    return parse(text);
}

bool saveWifiCreds(const std::vector<WifiCred>& creds) {
    std::string text;
    size_t n = 0;
    for (const auto& c : creds) {
        if (!storable(c) || n == kMaxWifiNetworks) continue;
        text += c.ssid + "\t" + c.password + "\n";
        ++n;
    }
    return hal::secretWrite(kSecret, text);
}

void rememberWifi(const WifiCred& cred) {
    if (!storable(cred)) {
        hal::log("[wifi] not remembered: name or password has control characters or is too long\n");
        return;
    }
    auto creds = loadWifiCreds();
    // "erase-remove idiom": remove_if moves the unwanted elements to the end
    // and returns where they start; erase() then chops them off.
    // Python: creds = [c for c in creds if c.ssid != cred.ssid]
    creds.erase(std::remove_if(creds.begin(), creds.end(),
                               [&](const WifiCred& c) { return c.ssid == cred.ssid; }),
                creds.end());
    creds.insert(creds.begin(), cred);
    saveWifiCreds(creds);
}

void forgetWifi(const std::string& ssid) {
    auto creds = loadWifiCreds();
    creds.erase(std::remove_if(creds.begin(), creds.end(),
                               [&](const WifiCred& c) { return c.ssid == ssid; }),
                creds.end());
    saveWifiCreds(creds);
}

bool findWifi(const std::string& ssid, std::string& passwordOut) {
    for (const auto& c : loadWifiCreds())
        if (c.ssid == ssid) { passwordOut = c.password; return true; }
    return false;
}

bool pickSavedWifi(const std::vector<WifiCred>& creds, const std::vector<hal::WifiNetwork>& inRange,
                   WifiCred& out) {
    const hal::WifiNetwork* best = nullptr;
    for (const auto& n : inRange) {
        if (best && n.rssi <= best->rssi) continue;   // dBm: higher (closer to 0) is stronger
        for (const auto& c : creds)
            if (c.ssid == n.ssid) { best = &n; out = c; break; }
    }
    return best != nullptr;
}

void importWifiFromSd() {
    std::string text;
    if (!hal::readFile(paths::kWifi, text, 16 * 1024)) return;     // no file: nothing to do
    const std::vector<WifiCred> found = parse(text);
    if (found.empty()) return;                                      // only comments: leave it
    auto creds = loadWifiCreds();
    for (auto it = found.rbegin(); it != found.rend(); ++it) {      // keep the file's order
        creds.erase(std::remove_if(creds.begin(), creds.end(),
                                   [&](const WifiCred& c) { return c.ssid == it->ssid; }),
                    creds.end());
        creds.insert(creds.begin(), *it);
    }
    if (!saveWifiCreds(creds)) return;                              // keep the file if NVS failed
    // Overwrite, so the password does not stay on the card. (The old bytes may survive
    // in the card's free space until reused – a FAT card is not a safe deleter. Don't
    // lend a card that ever held passwords without formatting it.)
    hal::writeFile(paths::kWifi, "# Imported into the device's internal storage on first start.\n"
                                 "# Add networks here again (SSID<TAB>password) to import more.\n");
    hal::log("[wifi] imported %u network(s) from the SD card\n", static_cast<unsigned>(found.size()));
}
