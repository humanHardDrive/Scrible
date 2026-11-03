#include "Eraser.h"

#include <cmath>

namespace scrible {
namespace {

constexpr double kMinPieceLength = 0.01;  // points; shorter leftovers are rounding noise

StrokePoint lerp(const StrokePoint& a, const StrokePoint& b, double t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}

// The part of segment a-b, as t in [0, 1], within `reach` of the eraser path e0-e1.
// The squared distance to a segment is convex along a line, so that part is a
// single interval: find the closest t, then bisect towards each end.
bool erasedInterval(const StrokePoint& a, const StrokePoint& b, const StrokePoint& e0, const StrokePoint& e1,
                    double reach, double& t0, double& t1) {
    const double r2 = reach * reach;
    auto f = [&](double t) {
        const StrokePoint p = lerp(a, b, t);
        return distSqToSegment(p.x, p.y, e0.x, e0.y, e1.x, e1.y);
    };

    double lo = 0, hi = 1;
    for (int i = 0; i < 60; ++i) {
        const double m1 = lo + (hi - lo) / 3, m2 = hi - (hi - lo) / 3;
        if (f(m1) < f(m2)) hi = m2;
        else lo = m1;
    }
    const double tm = (lo + hi) / 2;
    if (f(tm) >= r2) return false;

    auto boundary = [&](double outside, double inside) {
        for (int i = 0; i < 50; ++i) {
            const double mid = (outside + inside) / 2;
            (f(mid) < r2 ? inside : outside) = mid;
        }
        return (outside + inside) / 2;
    };
    t0 = f(0) < r2 ? 0 : boundary(0, tm);
    t1 = f(1) < r2 ? 1 : boundary(1, tm);
    return true;
}

double length(const Stroke& s) {
    double len = 0;
    for (size_t i = 0; i + 1 < s.points.size(); ++i)
        len += std::hypot(s.points[i + 1].x - s.points[i].x, s.points[i + 1].y - s.points[i].y);
    return len;
}

}  // namespace

bool eraseFromStroke(const Stroke& s, const StrokePoint& from, const StrokePoint& to, double radius,
                     std::vector<Stroke>& pieces) {
    RectF path;
    path.add(from.x, from.y);
    path.add(to.x, to.y);
    path.grow(radius);
    RectF reachable = path;
    reachable.grow(s.maxWidth() / 2);
    if (!s.bounds().intersects(reachable)) return false;

    const bool pressure = !s.segmentWidths.empty();
    Stroke cur = s;  // keeps tool, color, width and extra attributes
    cur.points.clear();
    cur.segmentWidths.clear();
    std::vector<Stroke> out;
    auto flush = [&] {
        if (cur.points.size() >= 2 && length(cur) >= kMinPieceLength) {
            cur.invalidateBounds();
            out.push_back(cur);
        }
        cur.points.clear();
        cur.segmentWidths.clear();
    };
    auto keep = [&](const StrokePoint& a, const StrokePoint& b, double w) {
        if (cur.points.empty()) cur.points.push_back(a);
        cur.points.push_back(b);
        if (pressure) cur.segmentWidths.push_back(w);
    };

    bool touched = false;
    for (size_t i = 0; i + 1 < s.points.size(); ++i) {
        const StrokePoint& a = s.points[i];
        const StrokePoint& b = s.points[i + 1];
        const double w = s.segmentWidth(i);
        const double reach = radius + w / 2;

        RectF seg;
        seg.add(a.x, a.y);
        seg.add(b.x, b.y);
        seg.grow(w / 2);
        double t0 = 0, t1 = 0;
        if (!seg.intersects(path) || !erasedInterval(a, b, from, to, reach, t0, t1)) {
            keep(a, b, w);
            continue;
        }
        touched = true;
        if (t0 > 0) keep(a, lerp(a, b, t0), w);
        flush();
        if (t1 < 1) keep(lerp(a, b, t1), b, w);
    }
    if (!touched) return false;
    flush();
    pieces = std::move(out);
    return true;
}

}  // namespace scrible
