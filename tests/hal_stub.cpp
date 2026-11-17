// ============================================================================
//  tests/hal_stub.cpp – the few HAL functions the PC test programs need
// ============================================================================
//  The tests link the real hal/sdl/Storage.cpp (files) but not Board.cpp,
//  which needs SDL. The dictionary and its builder also call hal::log(),
//  hal::millis() and hal::timeValid(); these stand-ins are enough.
//  (The linker is happy as long as exactly one definition of each exists.)
// ============================================================================
#include <chrono>
#include <cstdarg>
#include <cstdio>

#include "hal/Hal.hpp"

void hal::log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
}

uint32_t hal::millis() {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now() - t0).count());
}

bool hal::timeValid() { return true; }

// Secrets live in memory here: tests must never touch the real NVS or ./.jisho-secrets.
#include <map>
#include <string>
namespace {
std::map<std::string, std::string>& secrets() {
    static std::map<std::string, std::string> s;   // a function-local static: built on first use
    return s;
}
}  // namespace
bool hal::secretRead(const std::string& name, std::string& out) {
    const auto it = secrets().find(name);
    out = it == secrets().end() ? "" : it->second;
    return !out.empty();
}
bool hal::secretWrite(const std::string& name, const std::string& value) {
    secrets()[name] = value;
    return true;
}
