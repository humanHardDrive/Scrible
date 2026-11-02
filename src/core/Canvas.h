#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Geometry.h"

namespace xs {

// 8-bit grayscale image (0 = black, 255 = white), the same pixel format as the
// Kindle's framebuffer, so presenting a region is a plain copy.
class Canvas {
public:
    Canvas(int width, int height, uint8_t fill = 255);

    int width() const { return w_; }
    int height() const { return h_; }
    IRect rect() const { return {0, 0, w_, h_}; }

    uint8_t* row(int y) { return px_.data() + static_cast<size_t>(y) * w_; }
    const uint8_t* row(int y) const { return px_.data() + static_cast<size_t>(y) * w_; }
    uint8_t at(int x, int y) const { return row(y)[x]; }

    void fill(const IRect& r, uint8_t value);
    void strokeRect(const IRect& r, int thickness, uint8_t value);  // outline drawn inside r

    // Bitmap text using the built-in 8x8 font, each glyph scaled by `scale`.
    // Only printable ASCII is drawn; any other UTF-8 code point renders as '?'.
    void drawText(int x, int y, const std::string& text, int scale, uint8_t value, const IRect& clip);
    static int textWidth(const std::string& asciiText, int scale) {
        return static_cast<int>(asciiText.size()) * 8 * scale;
    }

private:
    int w_, h_;
    std::vector<uint8_t> px_;
};

}  // namespace xs
