#pragma once
// ============================================================================
//  WifiStore.hpp – remembered Wi-Fi networks, kept as a SECRET
// ============================================================================
//
//  Stored with hal::secretWrite("wifi", ...) – the ESP32's internal NVS flash,
//  NOT the SD card (anyone who borrows the card could read a text file on it).
//  The text inside is one network per line, SSID and password separated by a TAB:
//
//      HomeNetwork<TAB>secret123
//      Cafe<TAB>
//
//  TAB because SSIDs may contain spaces, '=', '|' ... A network whose name or
//  password contains a TAB, a line break or another control byte cannot be
//  stored safely in this format, so it is refused (never "almost" stored).
//  The most recently used network is first; it is connected at boot.
//
//  Setting up without typing: put the same lines into /tdeckjisho/config/wifi.txt
//  on the SD card. At the next start TDeckJisho IMPORTS them into NVS and
//  overwrites the file, so the password does not stay on the card.
// ============================================================================

#include <string>
#include <vector>

struct WifiCred {
    std::string ssid;
    std::string password;
};

// At most this many networks; longer names/passwords than Wi-Fi allows are refused.
constexpr size_t kMaxWifiNetworks = 8;

std::vector<WifiCred> loadWifiCreds();
bool saveWifiCreds(const std::vector<WifiCred>& creds);
// Move/insert `cred` to the front of the list and save.
void rememberWifi(const WifiCred& cred);
void forgetWifi(const std::string& ssid);
// The saved password for `ssid`, if any.
bool findWifi(const std::string& ssid, std::string& passwordOut);
// Move networks from an SD-card wifi.txt into NVS and wipe the file (call at boot).
void importWifiFromSd();
