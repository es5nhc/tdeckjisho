#pragma once
// ============================================================================
//  Screen.hpp – the base class every screen derives from
// ============================================================================
//
//  The app is a STACK of screens, like a phone app or a web browser's
//  history: Home -> Dictionary -> Entry. The top screen receives keys and is
//  drawn; Backspace usually pops it and reveals the one below.
//
//  A Screen is an abstract class ("interface"): `= 0` marks functions every
//  subclass MUST implement; the ones with `{}` bodies are optional hooks.
//
//    Python:  class Screen(ABC): @abstractmethod def on_key(self, app, key): ...
//    Rust:    trait Screen { fn on_key(&mut self, app: &mut App, key: i32); ... }
//
//  The App is passed into every call instead of being a global variable.
//  This is "dependency injection" – it makes it obvious what a screen can
//  touch, and lets tests hand in a different App.
// ============================================================================

#include <string>

#include <LovyanGFX.hpp>

class App;   // forward declaration: App.hpp includes Screen.hpp, not vice versa

struct Rect {
    int x, y, w, h;
};

class Screen {
public:
    // A virtual destructor is REQUIRED in a base class that will be deleted
    // through a base pointer (std::unique_ptr<Screen>). Without it, deleting
    // a SearchScreen via Screen* would skip SearchScreen's own destructor.
    virtual ~Screen() = default;

    virtual std::string title() const = 0;          // shown in the status bar
    virtual std::string hints() const { return ""; } // shown in the bottom bar

    // Called whenever this screen becomes the top of the stack: when first
    // pushed AND when the screen above it is popped.
    virtual void onEnter(App&) {}
    // Called just before this screen is removed from the stack for good.
    virtual void onExit(App&) {}

    virtual void onKey(App& app, int key) = 0;
    // A tap (finger or mouse click) at a SCREEN position (x 0..319, y 0..239; the content area
    // starts at y = theme::kContentY). Most screens ignore it.
    virtual void onTap(App&, int /*x*/, int /*y*/) {}
    // Called every loop iteration (~100x per second). Must not block for long.
    virtual void update(App&) {}
    // Draw into the off-screen canvas `g`, inside `area`.
    virtual void draw(App& app, lgfx::LovyanGFX& g, const Rect& area) = 0;

    // True while this screen needs the Wi-Fi radio left alone even without a connection
    // (the Wi-Fi screen, while the user scans and picks a network). See App::checkWifiGiveUp.
    virtual bool holdsWifi() const { return false; }
};
