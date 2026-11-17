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

namespace {

// A sprite in PSRAM, filled with the page colour (transparent PNG pixels end up on it).
std::unique_ptr<lgfx::LGFX_Sprite> makeSprite(int w, int h, int bits) {
    auto s = std::make_unique<lgfx::LGFX_Sprite>();
    s->setColorDepth(bits);
    s->setPsram(true);
    if (!s->createSprite(w, h)) return nullptr;
    s->fillScreen(theme::kBg);
    return s;
}

// Let LovyanGFX decode the file into `s`, scaled by `scale`.
bool decodeInto(lgfx::LGFX_Sprite& s, const rt::Image& img, hal::File& f, float scale) {
    HalDataWrapper data(f);
    const int w = s.width(), h = s.height();
    switch (img.format) {
        // drawXxx(source, x, y, maxW, maxH, offX, offY, scaleX, scaleY)
        case rt::ImageFormat::Jpeg: return s.drawJpg(&data, 0, 0, w, h, 0, 0, scale, scale);
        case rt::ImageFormat::Png:  return s.drawPng(&data, 0, 0, w, h, 0, 0, scale, scale);
        case rt::ImageFormat::Bmp:  return s.drawBmp(&data, 0, 0, w, h, 0, 0, scale, scale);
        case rt::ImageFormat::Qoi:  return s.drawQoi(&data, 0, 0, w, h, 0, 0, scale, scale);
        default: return false;
    }
}

// LovyanGFX's JPEG decoder (utility/lgfx_tjpgd.c, `#define JD_BAYER 1`, no way to
// switch it off) adds this 4×4 "Bayer" pattern to the brightness of every pixel:
// ORDERED DITHERING, meant to hide colour banding on a 16-bit panel. Rounded to
// RGB565 it flips pixels between neighbouring colour steps, which looks like a
// fine square grid. The pattern is fixed and starts at the picture's top-left
// corner, so we can subtract it again exactly.
constexpr int8_t kBayer[16] = {0, 4, 1, 5, -2, 2, -1, 3, 1, 5, 0, 4, -1, 3, -2, 2};

// Decoding at 1/div, TJpgDec averages div×div pixels, dither included. bayer[]
// gets, for each output position mod 4, the SUM of the offsets it averaged
// (so the real offset is bayer[i] / (div*div)). div 8 skips the dither: all 0.
void bayerTable(int div, int bayer[16]) {
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            int sum = 0;
            if (div < 8)
                for (int j = 0; j < div; ++j)
                    for (int i = 0; i < div; ++i) sum += kBayer[((y * div + j) & 3) * 4 + ((x * div + i) & 3)];
            bayer[y * 4 + x] = sum;
        }
}

// 0..255 -> nearest RGB565 step (swap565_t's own constructor just cuts off bits).
inline uint8_t round5(int v) { return static_cast<uint8_t>(std::clamp(v + 4, 0, 255)); }
inline uint8_t round6(int v) { return static_cast<uint8_t>(std::clamp(v + 2, 0, 255)); }

// Shrink the 24-bit `src` into the 16-bit `dst` with a BOX FILTER: every
// destination pixel is the average of the source pixels it covers – roughly what
// a PC image viewer does – minus the decoder's dither (see kBayer).
// Pixel types: a 24-bit sprite holds lgfx::bgr888_t (.r .g .b bytes), a 16-bit one
// lgfx::swap565_t (RGB565 with the bytes swapped, so it can go to the panel as is).
void boxShrink(const lgfx::LGFX_Sprite& src, lgfx::LGFX_Sprite& dst, int div) {
    int bayer[16];
    bayerTable(div, bayer);
    const int dd = div * div;
    const int sw = src.width(), sh = src.height(), dw = dst.width(), dh = dst.height();
    auto* in = static_cast<const lgfx::bgr888_t*>(src.getBuffer());
    auto* out = static_cast<lgfx::swap565_t*>(dst.getBuffer());
    for (int y = 0; y < dh; ++y) {
        const int y0 = y * sh / dh, y1 = std::max(y0 + 1, (y + 1) * sh / dh);
        for (int x = 0; x < dw; ++x) {
            const int x0 = x * sw / dw, x1 = std::max(x0 + 1, (x + 1) * sw / dw);
            int r = 0, g = 0, b = 0, dither = 0;
            for (int yy = y0; yy < y1; ++yy)
                for (int xx = x0; xx < x1; ++xx) {
                    const lgfx::bgr888_t p = in[yy * sw + xx];
                    r += p.r; g += p.g; b += p.b;
                    dither += bayer[(yy & 3) * 4 + (xx & 3)];
                }
            const int n = (y1 - y0) * (x1 - x0) * dd;   // r/n*dd = average colour
            out[y * dw + x] = lgfx::swap565_t(round5((r * dd - dither) / n),
                                              round6((g * dd - dither) / n),
                                              round5((b * dd - dither) / n));
        }
    }
}

// Largest full-size decode we allow before shrinking: 3 bytes a pixel, 2.25 MB of PSRAM.
constexpr uint32_t kMaxFullPixels = 768u * 1024u;

}  // namespace

// Why not just pass the scale to drawJpg()? Two problems:
//  * LovyanGFX shrinks every 8×8 JPEG block ON ITS OWN by picking the nearest
//    pixel, so all blocks drop the same rows and columns (PNG & co.: jagged edges).
//  * The JPEG decoder dithers (see kBayer), visible as a fine square grid.
// So we decode at full size – JPEG at 1/2, 1/4 or 1/8 if that is still big enough,
// which TJpgDec does exactly while decoding – into a temporary 24-bit sprite, and
// boxShrink() the WHOLE picture once, removing the dither on the way.
// A picture too large for that falls back to the direct way.
std::unique_ptr<lgfx::LGFX_Sprite> ImageCache::decode(const rt::Image& img, int dw, int dh) {
    hal::File f;
    if (!f.open(img.path)) return nullptr;
    const uint32_t t0 = hal::millis();

    // JPEG: the largest exact 1/div that still keeps at least dw×dh pixels.
    const bool jpeg = img.format == rt::ImageFormat::Jpeg;
    int div = 1;
    if (jpeg)
        while (div < 8 && img.w / (div * 2) >= dw && img.h / (div * 2) >= dh) div *= 2;
    const int fw = (img.w + div - 1) / div, fh = (img.h + div - 1) / div;

    auto sprite = makeSprite(dw, dh, 16);    // RGB565 like the canvas: dw*dh*2 bytes, e.g. 88 KB
    bool ok = false;
    if (sprite && static_cast<uint32_t>(fw) * fh <= kMaxFullPixels) {
        auto full = makeSprite(fw, fh, 24);  // temporary, freed on return
        ok = full && decodeInto(*full, img, f, 1.0f / div);
        if (ok) boxShrink(*full, *sprite, jpeg ? div : 8);   // 8 = "no dither to remove"
    } else if (sprite) {
        ok = decodeInto(*sprite, img, f, static_cast<float>(dw) / img.w);
    }
    hal::log("[image] %s %dx%d -> %dx%d (via %dx%d): %s in %u ms\n", img.src.c_str(), img.w,
             img.h, dw, dh, fw, fh, ok ? "decoded" : "FAILED",
             static_cast<unsigned>(hal::millis() - t0));
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
