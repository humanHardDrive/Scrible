#include "Lasso.h"

namespace scrible {

namespace {
constexpr double kInsideShare = 0.7;  // of a stroke's points, for it to count as selected
}

bool pointInPolygon(const StrokePoint& p, const std::vector<StrokePoint>& polygon) {
    bool inside = false;
    for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const StrokePoint& a = polygon[i];
        const StrokePoint& b = polygon[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) inside = !inside;
    }
    return inside;
}

std::vector<size_t> strokesInLasso(const std::vector<Element>& elements, const std::vector<StrokePoint>& loop) {
    std::vector<size_t> out;
    if (loop.size() < 3) return out;
    RectF area;
    for (const StrokePoint& p : loop) area.add(p.x, p.y);
    for (size_t i = 0; i < elements.size(); ++i) {
        const auto* s = std::get_if<Stroke>(&elements[i]);
        if (!s || s->points.empty() || !s->bounds().intersects(area)) continue;
        size_t inside = 0;
        for (const StrokePoint& p : s->points) inside += pointInPolygon(p, loop);
        if (inside >= kInsideShare * static_cast<double>(s->points.size())) out.push_back(i);
    }
    return out;
}

Stroke movedStroke(const Stroke& s, double dx, double dy) {
    Stroke out = s;
    for (StrokePoint& p : out.points) {
        p.x += dx;
        p.y += dy;
    }
    out.invalidateBounds();
    return out;
}

Stroke scaledStroke(const Stroke& s, const StrokePoint& anchor, double factor) {
    Stroke out = s;
    for (StrokePoint& p : out.points) {
        p.x = anchor.x + (p.x - anchor.x) * factor;
        p.y = anchor.y + (p.y - anchor.y) * factor;
    }
    out.width *= factor;
    for (double& w : out.segmentWidths) w *= factor;
    out.invalidateBounds();
    return out;
}

}  // namespace scrible
