#pragma once

// Software rasterizer for pages: antialiased variable-width strokes and ruled
// backgrounds, drawn into an 8-bit grayscale canvas.

#include "Canvas.h"
#include "Document.h"

namespace xs {

// Maps page coordinates (points) to screen pixels.
struct PageTransform {
    double scale = 1, offsetX = 0, offsetY = 0;

    double toScreenX(double x) const { return offsetX + x * scale; }
    double toScreenY(double y) const { return offsetY + y * scale; }
    double toPageX(double x) const { return (x - offsetX) / scale; }
    double toPageY(double y) const { return (y - offsetY) / scale; }

    IRect toScreen(const RectF& r) const;  // pixels touched by r, rounded outwards
    RectF toPage(const IRect& r) const;
    IRect pageRect(const Page& p) const;
};

struct RenderStyle {
    // Off on e-ink: strokes get hard edges, so the fast black/white (DU) refresh
    // used while writing already shows the final pixels.
    bool antialias = true;
};

// Fits `page` inside `area`, centered.
PageTransform fitPage(const Page& page, const IRect& area);

// Background and every layer, limited to `clip`.
void drawPage(Canvas& c, const Page& page, const PageTransform& t, const IRect& clip, const RenderStyle& style = {});
void drawStroke(Canvas& c, const Stroke& s, const PageTransform& t, const IRect& clip, const RenderStyle& style = {});

// Draws only segment i of a stroke that is still being written, straight onto the
// canvas. Highlighters get a 50% black checkerboard instead of their real gray so
// they stay visible under fast black-and-white e-ink updates; the region is
// repainted properly once the pen rests. Without antialiasing, pen segments come
// out exactly as the final render. Returns the pixels touched.
IRect drawLiveSegment(Canvas& c, const Stroke& s, size_t i, const PageTransform& t, const IRect& clip,
                      const RenderStyle& style = {});

uint8_t luminance(uint32_t rgb);

}  // namespace xs
