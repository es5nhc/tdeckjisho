#include "ui/ImageCache.hpp"

#include <algorithm>

#include "ui/Theme.hpp"

ImageCache::Entry* ImageCache::find(int index, int w, int h) {
    for (auto& e : _entries)
        if (e.index == index && e.w == w && e.h == h) return &e;
    return nullptr;
}

void ImageCache::evictFor(size_t bytes) {
    while (!_entries.empty() && _bytes + bytes > _budget) {
        auto oldest = std::min_element(_entries.begin(), _entries.end(),
                                       [](const Entry& a, const Entry& b) { return a.lastUse < b.lastUse; });
        _bytes -= static_cast<size_t>(oldest->w) * oldest->h * 2;
        _entries.erase(oldest);
    }
}

std::unique_ptr<lgfx::LGFX_Sprite> ImageCache::decode(const rt::Image& img, int dw, int dh) {
    hal::File f;
    if (!f.open(img.path)) return nullptr;
    auto sprite = std::make_unique<lgfx::LGFX_Sprite>();
    sprite->setColorDepth(16);           // RGB565, like the canvas: a plain copy later
    sprite->setPsram(true);              // dw*dh*2 bytes, e.g. 300*150*2 = 88 KB
    if (!sprite->createSprite(dw, dh)) return nullptr;
    sprite->fillScreen(theme::kBg);      // transparent PNG pixels end up on the page colour

    HalDataWrapper data(f);
    const float sx = static_cast<float>(dw) / img.w;
    const float sy = static_cast<float>(dh) / img.h;
    const uint32_t t0 = hal::millis();
    bool ok = false;
    switch (img.format) {
        // drawXxx(source, x, y, maxW, maxH, offX, offY, scaleX, scaleY)
        case rt::ImageFormat::Jpeg: ok = sprite->drawJpg(&data, 0, 0, dw, dh, 0, 0, sx, sy); break;
        case rt::ImageFormat::Png:  ok = sprite->drawPng(&data, 0, 0, dw, dh, 0, 0, sx, sy); break;
        case rt::ImageFormat::Bmp:  ok = sprite->drawBmp(&data, 0, 0, dw, dh, 0, 0, sx, sy); break;
        case rt::ImageFormat::Qoi:  ok = sprite->drawQoi(&data, 0, 0, dw, dh, 0, 0, sx, sy); break;
        default: break;
    }
    hal::log("[image] %s %dx%d -> %dx%d: %s in %u ms\n", img.src.c_str(), img.w, img.h, dw, dh,
             ok ? "decoded" : "FAILED", static_cast<unsigned>(hal::millis() - t0));
    if (!ok) return nullptr;
    return sprite;
}

void ImageCache::draw(lgfx::LovyanGFX& g, const rt::Image& img, int index, int x, int y, int dw, int dh) {
    Entry* e = find(index, dw, dh);
    if (!e) {
        const size_t bytes = static_cast<size_t>(dw) * dh * 2;
        evictFor(bytes);
        _entries.push_back({index, dw, dh, decode(img, dw, dh), 0});
        _bytes += bytes;
        e = &_entries.back();
    }
    e->lastUse = ++_clock;
    if (e->sprite) {
        e->sprite->pushSprite(&g, x, y);   // copy the decoded pixels: ~1 ms
        return;
    }
    // Decoding failed (damaged file, out of memory): a crossed-out box.
    g.drawRect(x, y, dw, dh, theme::kRule);
    g.drawLine(x, y, x + dw - 1, y + dh - 1, theme::kRule);
    g.drawLine(x, y + dh - 1, x + dw - 1, y, theme::kRule);
}
