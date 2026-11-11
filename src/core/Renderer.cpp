#include "Renderer.h"

#include <cmath>
#include <vector>

#include "Font.h"
#include "Paper.h"

namespace scrible {
namespace {

constexpr double kMinRadius = 0.6;  // px; keeps hairlines visible

// Ruling geometry of Xournal++, in points (spacings and the margin are in Paper.h).
constexpr double kRulingTopMargin = 80.0;     // no lines in the header...
constexpr double kRulingBottomMargin = 60.0;  // ...or the footer
constexpr double kRulingThickness = 0.5;
// The desktop ruling colors (light blue, pink) come out mid-gray on e-ink, which is too
// loud behind handwriting, so rulings are drawn in light grays instead.
constexpr uint8_t kRulingGray = 200;
constexpr uint8_t kMarginGray = 170;
constexpr uint8_t kPlaceholderGray = 150;

// On paper a 50% yellow highlighter is clearly visible; mapped to gray it would be
// almost white, so cap it at a gray that still shows up on e-ink.
uint8_t highlighterGray(uint32_t rgb) { return static_cast<uint8_t>(std::min((255 + luminance(rgb)) / 2, 200)); }

uint8_t strokeGray(const Stroke& s) {
    switch (s.tool) {
        case StrokeTool::Highlighter: return highlighterGray(s.color);
        case StrokeTool::Eraser: return 255;
        case StrokeTool::Pen: break;
    }
    return luminance(s.color);
}

inline uint8_t mix(uint8_t dst, uint8_t value, uint8_t cov) {
    return static_cast<uint8_t>((dst * (255 - cov) + value * cov + 127) / 255);
}

IRect capsuleBounds(double ax, double ay, double bx, double by, double r) {
    return {static_cast<int>(std::floor(std::min(ax, bx) - r - 1)),
            static_cast<int>(std::floor(std::min(ay, by) - r - 1)),
            static_cast<int>(std::ceil(std::max(ax, bx) + r + 1)),
            static_cast<int>(std::ceil(std::max(ay, by) + r + 1))};
}

// Calls plot(x, y, coverage) for every pixel of a round-capped segment of radius r.
template <typename Plot>
void rasterizeCapsule(double ax, double ay, double bx, double by, double r, const IRect& clip, Plot&& plot) {
    const IRect bb = capsuleBounds(ax, ay, bx, by, r).intersected(clip);
    const double outer = (r + 0.5) * (r + 0.5);
    const double inner = (r - 0.5) * (r - 0.5);
    for (int y = bb.y0; y < bb.y1; ++y) {
        const double py = y + 0.5;
        for (int x = bb.x0; x < bb.x1; ++x) {
            const double d2 = distSqToSegment(x + 0.5, py, ax, ay, bx, by);
            if (d2 >= outer) continue;
            const long cov = d2 <= inner ? 255 : std::lround((r + 0.5 - std::sqrt(d2)) * 255);
            if (cov > 0) plot(x, y, static_cast<uint8_t>(std::min(cov, 255L)));
        }
    }
}

double segmentRadius(const Stroke& s, size_t i, const PageTransform& t) {
    return std::max(s.segmentWidth(i) * t.scale / 2, kMinRadius);
}

// Without antialiasing a pixel is in or out at 50% coverage, the same rule the
// live path uses, so live and final renders match pixel for pixel.
inline uint8_t styled(uint8_t cov, const RenderStyle& style) {
    return style.antialias ? cov : cov >= 128 ? 255 : 0;
}

void drawStrokeIn(Canvas& c, const Stroke& s, const RectF& bounds, const PageTransform& t, const IRect& clip,
                  const RenderStyle& style) {
    const IRect box = t.toScreen(bounds).intersected(clip);
    if (box.empty()) return;

    // Coverage mask: each pixel keeps the max over all segments, so the overlapping
    // caps at segment joints don't darken translucent (highlighter) strokes.
    static thread_local std::vector<uint8_t> mask;
    const int bw = box.width();
    mask.assign(static_cast<size_t>(bw) * box.height(), 0);
    for (size_t i = 0; i + 1 < s.points.size(); ++i) {
        if (s.segmentWidth(i) <= 0) continue;  // Xournal++ treats these as invisible
        const auto& a = s.points[i];
        const auto& b = s.points[i + 1];
        rasterizeCapsule(t.toScreenX(a.x), t.toScreenY(a.y), t.toScreenX(b.x), t.toScreenY(b.y),
                         segmentRadius(s, i, t), box, [&](int x, int y, uint8_t cov) {
                             uint8_t& m = mask[static_cast<size_t>(y - box.y0) * bw + (x - box.x0)];
                             if (cov > m) m = cov;
                         });
    }

    const uint8_t value = strokeGray(s);
    const bool darken = s.tool == StrokeTool::Highlighter;
    for (int y = box.y0; y < box.y1; ++y) {
        uint8_t* row = c.row(y);
        const uint8_t* m = &mask[static_cast<size_t>(y - box.y0) * bw];
        for (int x = box.x0; x < box.x1; ++x) {
            const uint8_t cov = styled(m[x - box.x0], style);
            if (!cov) continue;
            const uint8_t target = darken ? std::min(row[x], value) : value;
            row[x] = mix(row[x], target, cov);
        }
    }
}

void drawBackground(Canvas& c, const Page& p, const PageTransform& t, const IRect& clip) {
    const IRect pr = t.pageRect(p).intersected(clip);
    if (pr.empty()) return;
    const bool solid = p.background.type == "solid";
    c.fill(pr, solid ? luminance(p.background.color) : 255);
    if (!solid) return;  // PDF and image backgrounds aren't rendered yet

    const std::string& style = p.background.style;
    const int thick = std::max(1, static_cast<int>(std::lround(kRulingThickness * t.scale)));
    auto hline = [&](double yPt, uint8_t g) {
        const int y = static_cast<int>(std::lround(t.toScreenY(yPt) - thick / 2.0));
        c.fill(IRect{pr.x0, y, pr.x1, y + thick}.intersected(pr), g);
    };
    auto vline = [&](double xPt, uint8_t g) {
        const int x = static_cast<int>(std::lround(t.toScreenX(xPt) - thick / 2.0));
        c.fill(IRect{x, pr.y0, x + thick, pr.y1}.intersected(pr), g);
    };

    // The spacing a file asks for, within reason (a tiny one would take forever to draw).
    auto spacing = [&](double fallback) {
        const double s = backgroundSetting(p.background, "r1", fallback);
        return s >= 2 ? s : fallback;
    };

    if (style == "lined" || style == "ruled") {
        const double s = spacing(kDefaultRuledSpacing);
        if (backgroundSetting(p.background, "scrible-full", 0) != 0) {
            // Lines all the way down, from one spacing below the top.
            for (int i = 1; i * s <= p.height - s / 2; ++i) hline(i * s, kRulingGray);
        } else {
            for (int i = 0; kRulingTopMargin + i * s <= p.height - kRulingBottomMargin; ++i)
                hline(kRulingTopMargin + i * s, kRulingGray);
        }
        if (style == "lined") {
            const double m = backgroundSetting(p.background, "m1", kDefaultMarginX);
            vline(m < 0 ? p.width + m : m, kMarginGray);  // negative: from the right edge
        }
    } else if (style == "graph") {
        // Lines on multiples of the square size, none on the page's edges.
        const double s = spacing(kDefaultGraphSpacing), edge = kRulingThickness / 2;
        for (int i = 1; i * s <= p.width - edge; ++i) vline(i * s, kRulingGray);
        for (int i = 1; i * s <= p.height - edge; ++i) hline(i * s, kRulingGray);
    } else if (style == "dotted") {
        const int d = thick * 2;
        const double s = spacing(kDefaultGraphSpacing);
        for (double y = s; y < p.height - 1; y += s) {
            for (double x = s; x < p.width - 1; x += s) {
                const int sx = static_cast<int>(std::lround(t.toScreenX(x))) - d / 2;
                const int sy = static_cast<int>(std::lround(t.toScreenY(y))) - d / 2;
                c.fill(IRect{sx, sy, sx + d, sy + d}.intersected(pr), kMarginGray);
            }
        }
    }
    // Other styles (isometric, staves, ...) are drawn plain for now.
}

void drawForeign(Canvas& c, const ForeignElement& f, const PageTransform& t, const IRect& clip, const RenderStyle& style) {
    if (!f.bounds.valid) return;
    const IRect r = t.toScreen(f.bounds);
    if (r.intersected(clip).empty()) return;

    if (f.tag == "text") {
        // Whatever its font, in Sans. Clipped to the element's bounds so partial repaints stay consistent.
        const Font& font = Font::sans();
        const double px = f.textSize * t.scale;
        const std::vector<TextLine> lines = font.layout(f.text, f.textSize, f.textWrap);
        for (size_t i = 0; i < lines.size(); ++i)
            font.draw(c, t.toScreenX(f.textX), t.toScreenY(f.textY) + i * font.lineHeight() * px, f.text,
                      lines[i].begin, lines[i].end, px, luminance(f.color), clip.intersected(r), style.antialias);
        return;
    }

    // Images and LaTeX: a labelled placeholder box.
    const int th = 2;
    for (const IRect& edge : {IRect{r.x0, r.y0, r.x1, r.y0 + th}, IRect{r.x0, r.y1 - th, r.x1, r.y1},
                              IRect{r.x0, r.y0, r.x0 + th, r.y1}, IRect{r.x1 - th, r.y0, r.x1, r.y1}})
        c.fill(edge.intersected(clip), kPlaceholderGray);
    const std::string label = f.tag == "teximage" ? "LATEX" : f.tag == "image" ? "IMAGE" : f.tag;
    c.drawText(r.x0 + 8, r.y0 + 8, label, 2, kPlaceholderGray, clip.intersected(r));
}

}  // namespace

uint8_t luminance(uint32_t rgb) {
    const unsigned r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
    return static_cast<uint8_t>((299 * r + 587 * g + 114 * b + 500) / 1000);
}

IRect PageTransform::toScreen(const RectF& r) const {
    if (!r.valid) return {};
    return {static_cast<int>(std::floor(toScreenX(r.x0))) - 1, static_cast<int>(std::floor(toScreenY(r.y0))) - 1,
            static_cast<int>(std::ceil(toScreenX(r.x1))) + 1, static_cast<int>(std::ceil(toScreenY(r.y1))) + 1};
}

RectF PageTransform::toPage(const IRect& r) const {
    RectF out;
    out.add(toPageX(r.x0), toPageY(r.y0));
    out.add(toPageX(r.x1), toPageY(r.y1));
    return out;
}

IRect PageTransform::pageRect(const Page& p) const {
    return {static_cast<int>(std::lround(offsetX)), static_cast<int>(std::lround(offsetY)),
            static_cast<int>(std::lround(offsetX + p.width * scale)),
            static_cast<int>(std::lround(offsetY + p.height * scale))};
}

PageTransform fitPage(const Page& page, const IRect& area) {
    PageTransform t;
    t.scale = std::min(area.width() / page.width, area.height() / page.height);
    t.offsetX = area.x0 + (area.width() - page.width * t.scale) / 2;
    t.offsetY = area.y0 + (area.height() - page.height * t.scale) / 2;
    return t;
}

void drawPage(Canvas& c, const Page& page, const PageTransform& t, const IRect& clip, const RenderStyle& style) {
    const IRect cl = clip.intersected(c.rect()).intersected(t.pageRect(page));
    if (cl.empty()) return;
    drawBackground(c, page, t, cl);
    const RectF visible = t.toPage(cl);
    for (const Layer& layer : page.layers) {
        for (const Element& e : layer.elements) {
            if (const auto* s = std::get_if<Stroke>(&e)) {
                const RectF b = s->bounds();
                if (b.intersects(visible)) drawStrokeIn(c, *s, b, t, cl, style);
            } else {
                drawForeign(c, std::get<ForeignElement>(e), t, cl, style);
            }
        }
    }
}

void drawStroke(Canvas& c, const Stroke& s, const PageTransform& t, const IRect& clip, const RenderStyle& style) {
    drawStrokeIn(c, s, s.bounds(), t, clip.intersected(c.rect()), style);
}

IRect drawLiveSegment(Canvas& c, const Stroke& s, size_t i, const PageTransform& t, const IRect& clip,
                      const RenderStyle& style) {
    if (i + 1 >= s.points.size()) return {};
    const auto& a = s.points[i];
    const auto& b = s.points[i + 1];
    const double ax = t.toScreenX(a.x), ay = t.toScreenY(a.y), bx = t.toScreenX(b.x), by = t.toScreenY(b.y);
    const double r = segmentRadius(s, i, t);
    const IRect cl = clip.intersected(c.rect());

    if (s.tool == StrokeTool::Highlighter) {
        rasterizeCapsule(ax, ay, bx, by, r, cl, [&](int x, int y, uint8_t cov) {
            if (cov >= 128 && ((x + y) & 1) == 0) c.row(y)[x] = 0;
        });
    } else {
        const uint8_t value = strokeGray(s);
        rasterizeCapsule(ax, ay, bx, by, r, cl, [&](int x, int y, uint8_t cov) {
            cov = styled(cov, style);
            if (!cov) return;
            uint8_t& px = c.row(y)[x];
            px = mix(px, value, cov);
        });
    }
    return capsuleBounds(ax, ay, bx, by, r).intersected(cl);
}

}  // namespace scrible
