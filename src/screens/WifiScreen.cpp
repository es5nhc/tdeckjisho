#include "screens/WifiScreen.hpp"
#include "core/I18n.hpp"

#include <algorithm>

#include "core/WifiStore.hpp"
#include "hal/Keys.hpp"
#include "screens/InputScreen.hpp"
#include "ui/Draw.hpp"
#include "ui/Theme.hpp"

using i18n::fmt;
using i18n::Str;
using i18n::tr;

namespace {
constexpr int kListY = 40;

const char* stateText(hal::WifiState s) {
    switch (s) {
        case hal::WifiState::Connected:  return tr(Str::WifiConnected);
        case hal::WifiState::Connecting: return tr(Str::WifiConnecting);
        case hal::WifiState::Failed:     return tr(Str::WifiFailed);
        default:                         return tr(Str::WifiNone);
    }
}
}  // namespace

void WifiScreen::onEnter(App&) {
    if (_nets.empty() && !_scanning) startScan();
}

void WifiScreen::startScan() {
    _scanning = hal::wifiScanStart();
}

void WifiScreen::rebuildList() {
    // Strongest signal first; the same SSID may appear several times (mesh
    // networks, several access points) – keep only the strongest.
    std::sort(_nets.begin(), _nets.end(),
              [](const hal::WifiNetwork& a, const hal::WifiNetwork& b) { return a.rssi > b.rssi; });
    std::vector<hal::WifiNetwork> unique;
    for (auto& n : _nets) {
        if (n.ssid.empty()) continue;   // hidden networks
        if (std::none_of(unique.begin(), unique.end(), [&](auto& u) { return u.ssid == n.ssid; }))
            unique.push_back(n);
    }
    _nets = std::move(unique);

    _list.items.clear();
    std::string dummy;
    for (const auto& n : _nets) {
        ListItem it;
        it.primary = n.ssid;
        it.secondary = std::to_string(n.rssi) + " dBm" +
                       (n.secured ? std::string(" · WPA") : " · " + std::string(tr(Str::WifiOpen))) +
                       (findWifi(n.ssid, dummy) ? " · " + std::string(tr(Str::WifiSaved)) : std::string());
        it.accent = (hal::wifiState() == hal::WifiState::Connected && hal::wifiSsid() == n.ssid);
        _list.items.push_back(std::move(it));
    }
    _list.clampSelection();
}

void WifiScreen::connect(App& app, const std::string& ssid, const std::string& password) {
    _pendingSsid = ssid;
    _pendingPass = password;
    hal::wifiConnect(ssid, password);
    app.toast(fmt(Str::WifiConnectingTo, {{"ssid", ssid}}));
}

void WifiScreen::onKey(App& app, int k) {
    if (_list.onKey(k, _list.rowsIn(theme::kContentH - kListY))) { app.redraw(); return; }
    switch (k) {
        case key::Enter:
        case key::Select: {
            if (_nets.empty()) break;
            const hal::WifiNetwork net = _nets[_list.selected];   // copy: the list may change
            std::string saved;
            if (!net.secured) {
                connect(app, net.ssid, "");
            } else if (findWifi(net.ssid, saved)) {
                connect(app, net.ssid, saved);
            } else {
                // Ask for the password. The callback runs later (after the
                // user presses Enter), so it captures `this` – safe because
                // the InputScreen sits ON TOP of us: we cannot be destroyed
                // while it exists.
                app.push(std::make_unique<InputScreen>(
                    tr(Str::WifiPasswordTitle), fmt(Str::WifiPasswordPrompt, {{"ssid", net.ssid}}), "", true,
                    [this, ssid = net.ssid](App& a, const std::string& pw) { connect(a, ssid, pw); }));
            }
            break;
        }
        case 's': startScan(); break;
        case 'x': hal::wifiDisconnect(); app.toast(tr(Str::WifiDisconnected)); break;
        case 'f':
            if (!_nets.empty()) {
                forgetWifi(_nets[_list.selected].ssid);
                app.toast(fmt(Str::WifiForgot, {{"ssid", _nets[_list.selected].ssid}}));
                rebuildList();
            }
            break;
        case key::Backspace: app.pop(); return;
        default: return;
    }
    app.redraw();
}

void WifiScreen::update(App& app) {
    if (_scanning) {
        const int n = hal::wifiScanPoll(_nets);
        if (n >= 0) { _scanning = false; rebuildList(); app.redraw(); }
    }
    const hal::WifiState s = hal::wifiState();
    if (s != _lastState) {
        _lastState = s;
        // Only now do we know the password was right: remember the network.
        if (s == hal::WifiState::Connected && !_pendingSsid.empty()) {
            rememberWifi({_pendingSsid, _pendingPass});
            _pendingSsid.clear();
            _pendingPass.clear();
            app.toast(tr(Str::WifiConnectedSaved));
        }
        rebuildList();
        app.redraw();
    }
}

void WifiScreen::draw(App&, lgfx::LovyanGFX& g, const Rect& r) {
    const hal::WifiState s = hal::wifiState();
    std::string status = stateText(s);
    if (s == hal::WifiState::Connected) status += ": " + hal::wifiSsid() + "  " + hal::wifiIp();
    draw::text(g, theme::fontSmall(), draw::fit(theme::fontSmall(), status, r.w - 12), r.x + 6, r.y + 4,
               s == hal::WifiState::Connected ? theme::kOk : theme::kText);
    draw::text(g, theme::fontSmall(), tr(_scanning ? Str::WifiScanning : Str::WifiNetworks), r.x + 6, r.y + 22, theme::kDim);

    const Rect list{r.x, r.y + kListY, r.w, r.h - kListY};
    if (_nets.empty()) draw::message(g, list.x, list.y, list.w, list.h, _scanning ? "" : tr(Str::WifiNoNetworks), theme::kDim);
    else _list.draw(g, list);
}
