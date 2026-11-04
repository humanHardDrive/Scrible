#include "Font.h"

#include <algorithm>
#include <cmath>

#include "stb_truetype.h"

namespace scrible {

extern const unsigned char kSansFont[];  // DejaVuSans.ttf, built in (cmake/embed.cmake)
extern const size_t kSansFontSize;

namespace {

constexpr size_t kMaxCachedGlyphs = 4000;  // a few sizes of a few hundred characters

}  // namespace

uint32_t nextCodePoint(const std::string& s, size_t& i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    const int extra = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
    if (extra < 0 || i + extra >= s.size()) {
        ++i;
        return 0xfffd;
    }
    uint32_t cp = extra == 0 ? c : c & (0x3f >> extra);
    for (int k = 1; k <= extra; ++k) {
        const unsigned char d = static_cast<unsigned char>(s[i + k]);
        if ((d & 0xc0) != 0x80) {
            ++i;
            return 0xfffd;
        }
        cp = (cp << 6) | (d & 0x3f);
    }
    i += 1 + extra;
    return cp;
}

size_t prevCodePointStart(const std::string& s, size_t i) {
    if (i == 0) return 0;
    --i;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xc0) == 0x80) --i;
    return i;
}

struct Font::Info {
    stbtt_fontinfo font;
    double unitsToEm = 0;
};

const Font& Font::sans() {
    static const Font f(kSansFont, kSansFontSize);
    return f;
}

Font::Font(const unsigned char* data, size_t size) : info_(new Info) {
    (void)size;
    stbtt_InitFont(&info_->font, data, stbtt_GetFontOffsetForIndex(data, 0));
    info_->unitsToEm = stbtt_ScaleForMappingEmToPixels(&info_->font, 1);
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&info_->font, &ascent, &descent, &gap);
    ascent_ = ascent * info_->unitsToEm;
    lineHeight_ = (ascent - descent + gap) * info_->unitsToEm;
}

Font::~Font() { delete info_; }

int Font::glyph(uint32_t codePoint) const {
    const auto it = glyphs_.find(codePoint);
    if (it != glyphs_.end()) return it->second;
    const int g = stbtt_FindGlyphIndex(&info_->font, static_cast<int>(codePoint));
    glyphs_.emplace(codePoint, g);
    return g;
}

double Font::advance(const std::string& s, size_t begin, size_t end) const {
    int units = 0, prev = -1;
    for (size_t i = begin; i < end;) {
        const int g = glyph(nextCodePoint(s, i));
        int adv = 0, bearing = 0;
        stbtt_GetGlyphHMetrics(&info_->font, g, &adv, &bearing);
        if (prev >= 0) units += stbtt_GetGlyphKernAdvance(&info_->font, prev, g);
        units += adv;
        prev = g;
    }
    return units * info_->unitsToEm;
}

std::vector<TextLine> Font::layout(const std::string& text, double size, double wrap) const {
    std::vector<TextLine> lines;
    size_t start = 0;
    for (;;) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        // Words go on the line while they fit; a word wider than the line has one to itself.
        size_t from = start;
        while (wrap > 0) {
            size_t fits = std::string::npos;
            for (size_t sp = text.find(' ', from); sp != std::string::npos && sp < end; sp = text.find(' ', sp + 1)) {
                if (advance(text, from, sp) * size > wrap) break;
                fits = sp;
            }
            if (fits == std::string::npos || advance(text, from, end) * size <= wrap) break;
            lines.push_back({from, fits, advance(text, from, fits) * size});
            from = fits + 1;
        }
        lines.push_back({from, end, advance(text, from, end) * size});
        if (end == text.size()) break;
        start = end + 1;
    }
    return lines;
}

size_t Font::offsetAt(const std::string& text, double size, double wrap, double x, double y) const {
    const std::vector<TextLine> lines = layout(text, size, wrap);
    const double step = lineHeight_ * size;
    const size_t row = static_cast<size_t>(std::clamp(std::floor(y / step), 0.0, double(lines.size() - 1)));
    const TextLine& l = lines[row];
    size_t best = l.begin;
    double bestDist = std::fabs(x);
    for (size_t i = l.begin; i < l.end;) {
        nextCodePoint(text, i);
        const double d = std::fabs(advance(text, l.begin, i) * size - x);
        if (d < bestDist) {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

const Font::Glyph& Font::rendered(int glyph, double px) const {
    const uint64_t key = static_cast<uint64_t>(glyph) | static_cast<uint64_t>(std::lround(px * 16)) << 32;
    const auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    if (cache_.size() >= kMaxCachedGlyphs) cache_.clear();
    Glyph g;
    const float scale = stbtt_ScaleForMappingEmToPixels(&info_->font, static_cast<float>(px));
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphBitmapBox(&info_->font, glyph, scale, scale, &x0, &y0, &x1, &y1);
    g.x0 = x0;
    g.y0 = y0;
    g.w = std::max(0, x1 - x0);
    g.h = std::max(0, y1 - y0);
    g.coverage.resize(static_cast<size_t>(g.w) * g.h);
    if (g.w > 0 && g.h > 0) stbtt_MakeGlyphBitmap(&info_->font, g.coverage.data(), g.w, g.h, g.w, scale, scale, glyph);
    return cache_.emplace(key, std::move(g)).first->second;
}

void Font::draw(Canvas& c, double x, double y, const std::string& s, size_t begin, size_t end, double px,
                uint8_t value, const IRect& clip, bool antialias) const {
    const IRect area = clip.intersected(c.rect());
    if (area.empty() || px < 1) return;
    const int baseline = static_cast<int>(std::lround(y + ascent_ * px));
    double pen = x;
    int prev = -1;
    for (size_t i = begin; i < end;) {
        const int gi = glyph(nextCodePoint(s, i));
        int adv = 0, bearing = 0;
        stbtt_GetGlyphHMetrics(&info_->font, gi, &adv, &bearing);
        if (prev >= 0) pen += stbtt_GetGlyphKernAdvance(&info_->font, prev, gi) * info_->unitsToEm * px;
        prev = gi;
        const Glyph& g = rendered(gi, px);
        const int gx = static_cast<int>(std::lround(pen)) + g.x0, gy = baseline + g.y0;
        pen += adv * info_->unitsToEm * px;
        const IRect r = IRect{gx, gy, gx + g.w, gy + g.h}.intersected(area);
        for (int yy = r.y0; yy < r.y1; ++yy) {
            uint8_t* row = c.row(yy);
            const uint8_t* cov = g.coverage.data() + static_cast<size_t>(yy - gy) * g.w - gx;
            for (int xx = r.x0; xx < r.x1; ++xx) {
                const int a = cov[xx];
                if (!antialias) {
                    if (a >= 128) row[xx] = value;
                } else if (a) {
                    row[xx] = static_cast<uint8_t>(row[xx] + ((value - row[xx]) * a + 127) / 255);
                }
            }
        }
    }
}

}  // namespace scrible
