#pragma once

// Text on the page, in the typeface built into the app: DejaVu Sans, which is what
// Xournal++'s default "Sans" is on most Linux systems, so typed notes take the same
// room in both. Sizes are font sizes (the em) in page points; Xournal++ places a text
// element by the top left of its first line.

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "Canvas.h"

namespace scrible {

// One line of a text: bytes [begin, end) of it, and how wide it is in points.
struct TextLine {
    size_t begin = 0, end = 0;
    double width = 0;
};

class Font {
public:
    static const Font& sans();

    // In ems.
    double ascent() const { return ascent_; }
    double lineHeight() const { return lineHeight_; }
    // Bytes [begin, end) of `s`, kerned.
    double advance(const std::string& s, size_t begin, size_t end) const;

    // The lines of `text` at `size` points: one per '\n', each word-wrapped at
    // `wrap` points (0: not wrapped).
    std::vector<TextLine> layout(const std::string& text, double size, double wrap = 0) const;
    // The byte offset in `text` nearest to (x, y), in points from its top left.
    size_t offsetAt(const std::string& text, double size, double wrap, double x, double y) const;

    // Bytes [begin, end) of `s` as one line, its top left at (x, y) pixels, `px` pixels
    // to the em. Without antialiasing, pixels are either `value` or left alone.
    void draw(Canvas& c, double x, double y, const std::string& s, size_t begin, size_t end, double px,
              uint8_t value, const IRect& clip, bool antialias = true) const;

    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;
    ~Font();

private:
    Font(const unsigned char* data, size_t size);
    int glyph(uint32_t codePoint) const;
    struct Glyph {
        int x0 = 0, y0 = 0, w = 0, h = 0;  // from the pen position on the baseline
        std::vector<uint8_t> coverage;
    };
    const Glyph& rendered(int glyph, double px) const;

    struct Info;
    Info* info_;
    double ascent_ = 0, lineHeight_ = 0;
    mutable std::unordered_map<uint32_t, int> glyphs_;
    mutable std::unordered_map<uint64_t, Glyph> cache_;
};

// The next code point of UTF-8 `s` from byte `i`, which moves past it. A broken
// sequence counts as U+FFFD, one byte long.
uint32_t nextCodePoint(const std::string& s, size_t& i);
// Where the code point before byte `i` of `s` starts.
size_t prevCodePointStart(const std::string& s, size_t i);

}  // namespace scrible
