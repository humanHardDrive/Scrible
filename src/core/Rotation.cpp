#include "Rotation.h"

#include <algorithm>

namespace scrible {

// Turned a quarter turn clockwise, the panel's left edge is the top: the canvas's
// pixel (x, y) is the panel's (y, height - 1 - x). The other turns follow suit.
void ScreenRotation::toLogical(double px, double py, double& lx, double& ly) const {
    switch (turns & 3) {
        case 1: lx = height - 1 - py; ly = px; break;
        case 2: lx = width - 1 - px; ly = height - 1 - py; break;
        case 3: lx = py; ly = width - 1 - px; break;
        default: lx = px; ly = py; break;
    }
}

IRect ScreenRotation::toPhysical(const IRect& r) const {
    switch (turns & 3) {
        case 1: return {r.y0, height - r.x1, r.y1, height - r.x0};
        case 2: return {width - r.x1, height - r.y1, width - r.x0, height - r.y0};
        case 3: return {width - r.y1, r.x0, width - r.y0, r.x1};
        default: return r;
    }
}

void ScreenRotation::copyOut(const Canvas& c, const IRect& p, uint8_t* out) const {
    const int w = p.width(), h = p.height();
    const int t = turns & 3;
    if (t == 0) {
        for (int y = 0; y < h; ++y) std::copy_n(c.row(p.y0 + y) + p.x0, w, out + static_cast<size_t>(y) * w);
        return;
    }
    if (t == 2) {
        // A row of the panel is a row of the canvas, backwards.
        for (int y = 0; y < h; ++y) {
            const uint8_t* src = c.row(height - 1 - (p.y0 + y));
            uint8_t* dst = out + static_cast<size_t>(y) * w;
            for (int x = 0; x < w; ++x) dst[x] = src[width - 1 - (p.x0 + x)];
        }
        return;
    }
    // ...or a column of it. Copied in tiles, so the canvas rows being read stay in the
    // cache: a whole screen goes in one pass on the Kindle's small caches.
    constexpr int kTile = 64;
    for (int ty = 0; ty < h; ty += kTile) {
        for (int tx = 0; tx < w; tx += kTile) {
            for (int y = ty; y < std::min(ty + kTile, h); ++y) {
                const int py = p.y0 + y;
                uint8_t* dst = out + static_cast<size_t>(y) * w;
                if (t == 1) {
                    const int lx = height - 1 - py;  // canvas x; canvas y = panel x
                    for (int x = tx; x < std::min(tx + kTile, w); ++x) dst[x] = c.row(p.x0 + x)[lx];
                } else {
                    for (int x = tx; x < std::min(tx + kTile, w); ++x) dst[x] = c.row(width - 1 - (p.x0 + x))[py];  // canvas (py, width - 1 - px)
                }
            }
        }
    }
}

}  // namespace scrible
