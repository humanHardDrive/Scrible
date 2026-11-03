#pragma once

#include <algorithm>
#include <cmath>

namespace scrible {

// Integer pixel rectangle, half-open: [x0, x1) x [y0, y1).
struct IRect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;

    bool empty() const { return x1 <= x0 || y1 <= y0; }
    int width() const { return x1 - x0; }
    int height() const { return y1 - y0; }
    bool contains(int x, int y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }

    IRect united(const IRect& o) const {
        if (empty()) return o;
        if (o.empty()) return *this;
        return {std::min(x0, o.x0), std::min(y0, o.y0), std::max(x1, o.x1), std::max(y1, o.y1)};
    }
    IRect intersected(const IRect& o) const {
        IRect r{std::max(x0, o.x0), std::max(y0, o.y0), std::min(x1, o.x1), std::min(y1, o.y1)};
        return r.empty() ? IRect{} : r;
    }
    IRect grown(int d) const { return empty() ? *this : IRect{x0 - d, y0 - d, x1 + d, y1 + d}; }
};

// Floating-point bounding box in page coordinates (points). Starts out empty.
struct RectF {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool valid = false;

    void add(double x, double y) {
        if (!valid) {
            x0 = x1 = x;
            y0 = y1 = y;
            valid = true;
            return;
        }
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
    }
    void grow(double d) {
        if (!valid) return;
        x0 -= d;
        y0 -= d;
        x1 += d;
        y1 += d;
    }
    bool intersects(const RectF& o) const {
        return valid && o.valid && x0 <= o.x1 && o.x0 <= x1 && y0 <= o.y1 && o.y0 <= y1;
    }
};

// Squared distance from point (px, py) to the segment (ax, ay)-(bx, by).
inline double distSqToSegment(double px, double py, double ax, double ay, double bx, double by) {
    const double dx = bx - ax, dy = by - ay;
    const double len2 = dx * dx + dy * dy;
    double t = 0;
    if (len2 > 0) t = std::clamp(((px - ax) * dx + (py - ay) * dy) / len2, 0.0, 1.0);
    const double cx = ax + t * dx - px, cy = ay + t * dy - py;
    return cx * cx + cy * cy;
}

}  // namespace scrible
