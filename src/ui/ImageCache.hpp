#pragma once
// ============================================================================
//  ImageCache.hpp – decode pictures once, draw them many times
// ============================================================================
//
//  LovyanGFX can decode JPEG (TJpgDec), PNG (pngle), BMP and QOI. All of
//  them read their input through an lgfx::DataWrapper, a small "give me
//  bytes / seek" interface. HalDataWrapper adapts our hal::File to it (the
//  ADAPTER pattern again, like StringSink in hal/tdeck/Net.cpp), so the very
//  same decoding code runs on the device (SD card) and in the simulator
//  (./sdcard folder). LovyanGFX's own drawJpgFile() would tie us to the
//  Arduino SD library on the device and to stdio on the PC.
//
//  Why a cache? The app redraws the whole screen on every scroll step.
//  Decoding a 300×150 JPEG from the SD card takes ~50–150 ms on the ESP32;
//  copying an already decoded picture (an RGB565 sprite in PSRAM) takes
//  ~1 ms. So each picture is decoded ONCE, at its display size, and kept.
//  The cache has a byte budget; the least recently used picture goes first
//  (LRU) – the same idea as the block cache in dict/Dictionary.cpp.
//
//  SPI note: the T-Deck's display and SD card share one SPI bus. We decode
//  into a sprite in RAM, never straight onto the panel, so SD reads and
//  display writes never interleave (need_transaction stays false).
// ============================================================================

#include <cstdint>
// LovyanGFX's PC file wrapper (platforms/sdl) calls fopen() & co. but does not
// include <cstdio> itself; it only compiles if someone included it before.
// A classic "include order" bug in a library – the fix is to include it first.
#include <cstdio>
#include <memory>
#include <vector>

#include <LovyanGFX.hpp>

#include "hal/Hal.hpp"
#include "text/RichText.hpp"

class HalDataWrapper : public lgfx::DataWrapper {
public:
    explicit HalDataWrapper(hal::File& f) : _f(f) {}
    int read(uint8_t* buf, uint32_t len) override {
        const int n = static_cast<int>(_f.read(buf, len));
        _pos += n;
        return n;
    }
    void skip(int32_t offset) override { seek(_pos + offset); }
    bool seek(uint32_t offset) override { _pos = offset; return _f.seek(offset); }
    void close() override {}                    // the hal::File's owner closes it
    int32_t tell() override { return static_cast<int32_t>(_pos); }

private:
    hal::File& _f;
    uint32_t _pos = 0;
};

class ImageCache {
public:
    explicit ImageCache(size_t budgetBytes = 1024 * 1024) : _budget(budgetBytes) {}

    // Draw picture number `index` (scaled to dw×dh) with its top-left at x,y.
    // Decodes it on first use; draws a "broken picture" box if that fails.
    void draw(lgfx::LovyanGFX& g, const rt::Image& img, int index, int x, int y, int dw, int dh);
    void clear() { _entries.clear(); _bytes = 0; }

private:
    struct Entry {
        int index, w, h;
        std::unique_ptr<lgfx::LGFX_Sprite> sprite;   // nullptr = decoding failed
        uint32_t lastUse;
    };
    Entry* find(int index, int w, int h);
    std::unique_ptr<lgfx::LGFX_Sprite> decode(const rt::Image& img, int dw, int dh);
    void evictFor(size_t bytes);

    std::vector<Entry> _entries;
    size_t _bytes = 0, _budget;
    uint32_t _clock = 0;                 // "time" for LRU: counts draw() calls
};
