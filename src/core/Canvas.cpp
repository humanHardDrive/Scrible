#include "Canvas.h"

#include <cstring>

#include "Font8x8.h"

namespace scrible {

Canvas::Canvas(int width, int height, uint8_t fill)
    : w_(width), h_(height), px_(static_cast<size_t>(width) * height, fill) {}

void Canvas::fill(const IRect& r, uint8_t value) {
    const IRect c = r.intersected(rect());
    for (int y = c.y0; y < c.y1; ++y) std::memset(row(y) + c.x0, value, c.width());
}

void Canvas::strokeRect(const IRect& r, int t, uint8_t value) {
    fill({r.x0, r.y0, r.x1, r.y0 + t}, value);
    fill({r.x0, r.y1 - t, r.x1, r.y1}, value);
    fill({r.x0, r.y0, r.x0 + t, r.y1}, value);
    fill({r.x1 - t, r.y0, r.x1, r.y1}, value);
}

void Canvas::drawText(int x, int y, const std::string& text, int scale, uint8_t value, const IRect& clip) {
    const IRect c = clip.intersected(rect());
    for (unsigned char ch : text) {
        if ((ch & 0xc0) == 0x80) continue;  // UTF-8 continuation byte: one glyph per code point
        if (ch < 0x20 || ch > 0x7e) ch = '?';
        const uint8_t* glyph = kFont8x8[ch - 0x20];
        for (int gy = 0; gy < 8; ++gy) {
            for (int gx = 0; gx < 8; ++gx) {
                if (!((glyph[gy] >> gx) & 1)) continue;
                fill(IRect{x + gx * scale, y + gy * scale, x + (gx + 1) * scale, y + (gy + 1) * scale}.intersected(c),
                     value);
            }
        }
        x += 8 * scale;
    }
}

}  // namespace scrible
