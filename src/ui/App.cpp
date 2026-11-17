#include "ui/App.hpp"
#include "core/I18n.hpp"

#include <algorithm>   // std::clamp, std::min, std::max
#include <ctime>

#include "core/Paths.hpp"
#include "dict/DictBuilder.hpp"
#include "core/WifiStore.hpp"
#include "hal/Hal.hpp"
#include "hal/Keys.hpp"
#include "screens/Actions.hpp"
#include "ui/Draw.hpp"
#include "ui/Theme.hpp"

void App::begin() {
    // ---- the off-screen canvas -------------------------------------------
    _canvas.setColorDepth(16);     // RGB565, same as the panel
    _canvas.setPsram(true);        // allocate the 150 KB in PSRAM (ignored on PC)
    if (!_canvas.createSprite(theme::kScreenW, theme::kScreenH))
        hal::log("[app] could not allocate canvas!\n");

    // ---- persistent state on the SD card -----------------------------------
    hal::makeDirs(paths::kConfig);
    _settings.load(paths::kSettings);
    _positions.load(paths::kPositions);
    _quiz.load(paths::kQuiz);

    // The language: chosen in the Language screen (Settings → `g`), stored as lang=et|en|ja.
    i18n::setLanguage(i18n::fromCode(_settings.get("lang", "en"), i18n::Lang::En));
    applyBacklight();   // hal::begin() switched it on at full; now the user's level

    // ---- dictionary --------------------------------------------------------
    // A dictionary update interrupted by a power cut is finished (or undone)
    // first – see dict/DictBuilder.hpp.
    build::DictBuilder::recoverSwap();
    if (!_dict.open(paths::kDict)) hal::log("[app] dictionary: %s\n", _dict.error().c_str());

    // ---- Wi-Fi: connect to the most recently used network, if any ----------
    importWifiFromSd();                 // a wifi.txt on the card moves into NVS, once
    const auto creds = loadWifiCreds();
    if (!creds.empty()) hal::wifiConnect(creds[0].ssid, creds[0].password);

    _now = _lastInput = hal::millis();
    _stack.push_back(actions::home(*this));
    _stack.back()->onEnter(*this);
    _dirty = true;
}

// ---------------------------------------------------------------------------
//  Navigation
// ---------------------------------------------------------------------------
void App::push(std::unique_ptr<Screen> s) { _pending.push_back({Op::Push, std::move(s)}); }
void App::pop() { _pending.push_back({Op::Pop, nullptr}); }
void App::replace(std::unique_ptr<Screen> s) { _pending.push_back({Op::Replace, std::move(s)}); }

void App::applyPending() {
    if (_pending.empty()) return;
    // Take the queue out first: onEnter()/onExit() below may queue NEW
    // operations, and modifying a vector while iterating over it is a bug.
    std::vector<Op> ops = std::move(_pending);
    _pending.clear();

    for (Op& op : ops) {
        if ((op.kind == Op::Pop || op.kind == Op::Replace) && !_stack.empty()) {
            if (_stack.size() == 1 && op.kind == Op::Pop) continue;   // never pop Home
            _stack.back()->onExit(*this);
            _stack.pop_back();            // unique_ptr deletes the screen HERE
        }
        if (op.kind == Op::Push || op.kind == Op::Replace) _stack.push_back(std::move(op.screen));
    }
    _stack.back()->onEnter(*this);
    _dirty = true;
    applyPending();                       // in case onEnter queued something
}

bool App::wakeUp() {
    _lastInput = _now;
    if (_asleep) {
        _asleep = false;
        _dimmed = false;
        hal::displayOn();
        applyBacklight();                  // displayOn() switches it on at full
        _dirty = true;
        return true;
    }
    if (_dimmed) {                         // input while dimmed: back to full brightness
        _dimmed = false;
        applyBacklight();
    }
    return false;
}

uint8_t App::fullBacklight() const {
    // Stored as a percentage so settings.txt reads naturally. Clamped: a hand-edited
    // "brightness=0" must not give a black screen that looks like a dead device.
    const int pct = std::clamp(_settings.getInt("brightness", 100), 5, 100);
    return static_cast<uint8_t>(pct * 255 / 100);
}

// With auto-reconnect on, the ESP32 keeps scanning for a network that is gone (you left
// home) forever, and the radio is one of the biggest power users on the board. So: if the
// state has not reached Connected within `wifi_off_s` seconds of starting to connect
// (at boot, after a lost connection, or after picking a network), switch the radio off.
// Settings → Wi-Fi turns it on again. 0 = never give up.
void App::checkWifiGiveUp() {
    const hal::WifiState ws = hal::wifiState();
    if (ws != _wifiState) {
        if (ws == hal::WifiState::Connecting) _wifiTryingSince = _now;   // the clock starts
        _wifiState = ws;
    }
    if (ws != hal::WifiState::Connecting && ws != hal::WifiState::Failed) return;
    // The Wi-Fi screen is where the user is fixing exactly this: never pull the radio
    // from under a scan they are watching. The minute starts again when they leave.
    if (_stack.back()->holdsWifi()) { _wifiTryingSince = _now; return; }

    const int limitS = _settings.getInt("wifi_off_s", 60);
    if (limitS <= 0 || _now - _wifiTryingSince < static_cast<uint32_t>(limitS) * 1000u) return;
    hal::log("[wifi] no network for %d s, switching the radio off\n", limitS);
    hal::wifiDisconnect();
    _wifiState = hal::WifiState::Off;
    toast(i18n::tr(i18n::Str::WifiGaveUp), 5000);
}

void App::toast(const std::string& msg, uint32_t ms) {
    _toast = msg;
    _toastUntil = _now + ms;
    _dirty = true;
}

// ---------------------------------------------------------------------------
//  The main loop step
// ---------------------------------------------------------------------------
void App::tick() {
    _now = hal::millis();

    // 1. Input
    for (int k = hal::pollKey(); k != key::None; k = hal::pollKey()) {
        if (wakeUp()) continue;            // the waking key is swallowed
        _stack.back()->onKey(*this, k);
        applyPending();
    }

    // 1b. Taps, handled like a key: they wake the screen (and are swallowed), then reach the top screen
    for (int tx = 0, ty = 0; hal::pollTap(tx, ty);) {
        if (wakeUp()) continue;
        _stack.back()->onTap(*this, tx, ty);
        applyPending();
    }

    // 2. Background work of the top screen (searches, downloads, scans...)
    _stack.back()->update(*this);
    applyPending();

    // 3. Power saving and ANTI-GHOSTING after inactivity:
    //    * at 3/4 of the timeout: dim the backlight (a hint that sleep is near)
    //    * at the timeout: black frame + backlight off + panel asleep
    //      (hal::displayOff). A static picture left on an LCD for hours can
    //      leave a faint after-image; a black, sleeping panel can't.
    //    Deep sleep of the CPU would save more but needs a wake-up source
    //    (keyboard interrupt or trackball GPIO; not done yet, see docs/04-hardware.md).
    const uint32_t sleepMs = static_cast<uint32_t>(_settings.getInt("sleep_s", 120)) * 1000u;
    const uint32_t idle = _now - _lastInput;
    if (!_asleep && !_dimmed && idle > sleepMs / 4 * 3) {
        _dimmed = true;
        // Half of the user's level, at most 40 (about 3 of the chip's 16 steps). Never 0:
        // that is "off", and the user would think the device died.
        const int dim = std::min(40, fullBacklight() / 2);
        hal::setBacklight(static_cast<uint8_t>(std::max(1, dim)));
    }
    if (!_asleep && idle > sleepMs) {
        _asleep = true;
        hal::displayOff();
    }

    // Wi-Fi is checked even while the screen is off: a radio searching for a network that
    // isn't there drains the battery whether anyone looks or not.
    checkWifiGiveUp();
    if (_asleep) return;

    // 4. Status bar changes (clock minute, Wi-Fi, battery) – check once a second.
    if (_now - _lastStatusCheck > 1000) {
        _lastStatusCheck = _now;
        std::string sig = statusSignature();
        if (sig != _lastStatus) { _lastStatus = std::move(sig); _dirty = true; }
    }
    if (!_toast.empty() && _now > _toastUntil) { _toast.clear(); _dirty = true; }

    // 5. Draw
    if (_dirty) {
        _dirty = false;
        render();
    }
}

void App::render() {
    // LovyanGFX 1.3.0 pushes a PSRAM sprite to the panel by DMA and returns
    // BEFORE the transfer is over (the last pushSprite() below may still be
    // reading _canvas). Painting the next frame into it right away would tear
    // the picture on the device, so wait for the previous transfer first.
    // Free when it is already done, and a no-op in the SDL simulator.
    hal::display().waitDMA();

    lgfx::LovyanGFX& g = _canvas;          // a reference to the base class
    g.fillScreen(theme::kBg);

    const Rect content{0, theme::kContentY, theme::kScreenW, theme::kContentH};
    // Clip drawing to the content area so a screen cannot paint over the bars.
    g.setClipRect(content.x, content.y, content.w, content.h);
    _stack.back()->draw(*this, g, content);
    g.clearClipRect();

    drawTopBar(g);
    drawBottomBar(g);

    if (!_toast.empty()) {
        const lgfx::IFont* f = theme::fontSmall();
        const std::string t = draw::fit(f, _toast, theme::kScreenW - 40);
        const int w = draw::textWidth(f, t) + 16;
        const int x = (theme::kScreenW - w) / 2;
        const int y = theme::kScreenH - theme::kHintH - 28;
        g.fillRoundRect(x, y, w, 22, 6, theme::kWhiteColour);
        g.drawRoundRect(x, y, w, 22, 6, theme::kBlackColour);
        draw::text(g, f, t, x + 8, y + 5, theme::kText);
    }

    // Copy the finished frame to the panel in one go.
    _canvas.pushSprite(&hal::display(), 0, 0);
}

std::string App::statusSignature() const {
    char buf[64];
    std::time_t t = std::time(nullptr);
    std::tm* lt = std::localtime(&t);
    snprintf(buf, sizeof buf, "%d|%d|%d|%d", lt->tm_min, static_cast<int>(hal::wifiState()),
             hal::batteryPercent() / 5, hal::timeValid());
    return buf;
}

void App::drawTopBar(lgfx::LovyanGFX& g) {
    const lgfx::IFont* f = theme::fontSmall();
    g.fillRect(0, 0, theme::kScreenW, theme::kStatusH, theme::kBgBar);

    // Right side, drawn from right to left: battery, clock, Wi-Fi.
    int x = theme::kScreenW - 4;
    const int y = 2;
    char buf[16];
    const int bat = hal::batteryPercent();
    if (bat >= 0) {
        snprintf(buf, sizeof buf, "%d%%", bat);
        draw::textRight(g, f, buf, x, y, bat < 15 ? theme::kAccent : theme::kWhiteColour);
        x -= draw::textWidth(f, buf) + 8;
    }
    if (hal::timeValid()) {
        std::time_t t = std::time(nullptr);
        std::strftime(buf, sizeof buf, "%H:%M", std::localtime(&t));
        draw::textRight(g, f, buf, x, y, theme::kWhiteColour);
        x -= draw::textWidth(f, buf) + 8;
    }
    const hal::WifiState ws = hal::wifiState();
    if (ws != hal::WifiState::Off) {
        const char* label = ws == hal::WifiState::Connected ? "WiFi" : ws == hal::WifiState::Connecting ? "WiFi…" : "WiFi×";
        const uint16_t col = ws == hal::WifiState::Connected ? theme::kWifiOK : theme::kDim;
        draw::textRight(g, f, label, x, y, col);
        x -= draw::textWidth(f, label) + 8;
    }

    // Left side: the title, cut to whatever space is left.
    const std::string title = draw::fit(f, _stack.back()->title(), x - 8);
    draw::text(g, f, title, 4, y, theme::kWhiteColour);
}

void App::drawBottomBar(lgfx::LovyanGFX& g) {
    const int y = theme::kScreenH - theme::kHintH;
    g.fillRect(0, y, theme::kScreenW, theme::kHintH, theme::kBgBar);
    const lgfx::IFont* f = theme::fontSmall();
    draw::text(g, f, draw::fit(f, _stack.back()->hints(), theme::kScreenW - 8), 4, y,
               theme::kWhiteColour);
}
