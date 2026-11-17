#pragma once
// ============================================================================
//  WifiScreen – scan, pick a network, enter the password, connect
// ============================================================================
//
//  Everything here is NON-blocking: the scan and the connection both run in
//  the background (inside the ESP32 Wi-Fi driver). update() polls their state
//  every loop and redraws when something changed.
// ============================================================================

#include "core/I18n.hpp"
#include <string>
#include <vector>

#include "hal/Hal.hpp"
#include "ui/App.hpp"
#include "ui/ListView.hpp"

class WifiScreen : public Screen {
public:
    std::string title() const override { return "Wi-Fi"; }
    std::string hints() const override { return i18n::tr(i18n::Str::WifiHints); }
    void onEnter(App& app) override;
    void onKey(App& app, int key) override;
    void update(App& app) override;
    void draw(App& app, lgfx::LovyanGFX& g, const Rect& r) override;

private:
    void startScan();
    void rebuildList();
    void connect(App& app, const std::string& ssid, const std::string& password);

    std::vector<hal::WifiNetwork> _nets;
    ListView _list;
    bool _scanning = false;
    hal::WifiState _lastState = hal::WifiState::Off;
    std::string _pendingSsid, _pendingPass;   // saved once the connection works
};
