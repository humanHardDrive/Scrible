#include "Shapes.h"

#include <algorithm>
#include <cmath>

namespace scrible {
namespace {

using P = StrokePoint;

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180;

constexpr double kMinSize = 8;              // points; smaller strokes (most handwriting) stay as drawn
constexpr double kAxisSnap = 5 * kDeg;      // lines this close to horizontal/vertical are straightened
constexpr double kRectAxisSnap = 8 * kDeg;  // likewise for rectangles
constexpr double kRightAngleTol = 15 * kDeg;
constexpr double kStraightCorner = 155 * kDeg;  // a "corner" this open is really part of a side
constexpr size_t kSamples = 96;
constexpr size_t kEllipseSegments = 72;

P operator+(P a, P b) { return {a.x + b.x, a.y + b.y}; }
P operator-(P a, P b) { return {a.x - b.x, a.y - b.y}; }
P operator*(P a, double s) { return {a.x * s, a.y * s}; }
double dot(P a, P b) { return a.x * b.x + a.y * b.y; }
double norm(P a) { return std::hypot(a.x, a.y); }
double dist(P a, P b) { return norm(a - b); }
P mid(P a, P b) { return (a + b) * 0.5; }
P unit(P a) {
    const double n = norm(a);
    return n > 0 ? a * (1 / n) : P{1, 0};
}
P rotate(P a, double t) { return {a.x * std::cos(t) - a.y * std::sin(t), a.x * std::sin(t) + a.y * std::cos(t)}; }
double distToSegment(P p, P a, P b) { return std::sqrt(distSqToSegment(p.x, p.y, a.x, a.y, b.x, b.y)); }

// The axis direction (a multiple of 90 degrees) nearest `angle`, exactly:
// cos(pi/2) isn't quite 0 in floating point, and snapped lines should be exact.
P axisDirection(double angle) {
    switch ((std::lround(angle / (kPi / 2)) % 4 + 4) % 4) {
        case 0: return {1, 0};
        case 1: return {0, 1};
        case 2: return {-1, 0};
        default: return {0, -1};
    }
}

// Angle at b between b->a and b->c: pi for a straight line, small for a sharp corner.
double cornerAngle(P a, P b, P c) {
    const P u = a - b, v = c - b;
    const double nu = norm(u), nv = norm(v);
    if (nu == 0 || nv == 0) return kPi;
    return std::acos(std::clamp(dot(u, v) / (nu * nv), -1.0, 1.0));
}

double pathLength(const std::vector<P>& p) {
    double len = 0;
    for (size_t i = 0; i + 1 < p.size(); ++i) len += dist(p[i], p[i + 1]);
    return len;
}

double diagonal(const std::vector<P>& p) {
    RectF r;
    for (const P& q : p) r.add(q.x, q.y);
    return std::hypot(r.x1 - r.x0, r.y1 - r.y0);
}

std::vector<P> withoutRepeats(const std::vector<P>& in) {
    std::vector<P> out;
    for (const P& q : in)
        if (out.empty() || dist(out.back(), q) > 1e-6) out.push_back(q);
    return out;
}

// n points evenly spaced along the path, both ends included.
std::vector<P> resample(const std::vector<P>& p, size_t n) {
    const double total = pathLength(p);
    if (total <= 0) return std::vector<P>(n, p.front());
    const double step = total / static_cast<double>(n - 1);
    std::vector<P> out{p.front()};
    double done = 0;  // path length up to p[i]
    size_t i = 0;
    for (size_t k = 1; k + 1 < n; ++k) {
        const double target = static_cast<double>(k) * step;
        while (i + 1 < p.size() && done + dist(p[i], p[i + 1]) < target) {
            done += dist(p[i], p[i + 1]);
            ++i;
        }
        if (i + 1 >= p.size()) break;
        const double seg = dist(p[i], p[i + 1]);
        out.push_back(p[i] + (p[i + 1] - p[i]) * (seg > 0 ? (target - done) / seg : 0));
    }
    out.push_back(p.back());
    return out;
}

// Douglas-Peucker: adds, in order, the indices of the points between i and j that
// are needed to stay within `tol` of p[i..j].
void simplify(const std::vector<P>& p, size_t i, size_t j, double tol, std::vector<size_t>& keep) {
    double worst = 0;
    size_t at = i;
    for (size_t k = i + 1; k < j; ++k) {
        const double d = distToSegment(p[k], p[i], p[j]);
        if (d > worst) {
            worst = d;
            at = k;
        }
    }
    if (worst <= tol) return;
    simplify(p, i, at, tol, keep);
    keep.push_back(at);
    simplify(p, at, j, tol, keep);
}

std::vector<P> simplifyOpen(const std::vector<P>& p, double tol) {
    std::vector<size_t> keep{0};
    simplify(p, 0, p.size() - 1, tol, keep);
    keep.push_back(p.size() - 1);
    std::vector<P> out;
    for (size_t k : keep) out.push_back(p[k]);
    return out;
}

// The corners of a closed outline (`ring` doesn't repeat its first point).
std::vector<P> corners(const std::vector<P>& ring, double tol, double minSide) {
    size_t far = 0;
    for (size_t k = 1; k < ring.size(); ++k)
        if (dist(ring[k], ring[0]) > dist(ring[far], ring[0])) far = k;
    std::vector<P> path = ring;
    path.push_back(ring[0]);
    std::vector<size_t> keep{0};
    simplify(path, 0, far, tol, keep);
    keep.push_back(far);
    simplify(path, far, path.size() - 1, tol, keep);
    std::vector<P> v;
    for (size_t k : keep) v.push_back(path[k]);

    // The pen usually starts mid-side, and rounded corners come out as two nearby
    // vertices: drop vertices that don't turn, and merge ones that are very close.
    for (bool changed = true; changed && v.size() > 2;) {
        changed = false;
        for (size_t k = 0; k < v.size(); ++k) {
            const size_t next = (k + 1) % v.size();
            if (dist(v[k], v[next]) < minSide) {
                v[next] = mid(v[k], v[next]);
            } else if (cornerAngle(v[(k + v.size() - 1) % v.size()], v[k], v[next]) <= kStraightCorner) {
                continue;
            }
            v.erase(v.begin() + static_cast<std::ptrdiff_t>(k));
            changed = true;
            break;
        }
    }
    return v;
}

Shape line(P a, P b) {
    const P d = b - a;
    const double angle = std::atan2(d.y, d.x);
    const double axis = std::round(angle / (kPi / 2)) * (kPi / 2);
    if (std::fabs(angle - axis) <= kAxisSnap) {
        const P m = mid(a, b), half = axisDirection(axis) * (norm(d) / 2);
        a = m - half;
        b = m + half;
    }
    return {ShapeKind::Line, {a, b}};
}

Shape polygon(std::vector<P> v) {
    Shape s;
    if (v.size() == 3) {
        s.kind = ShapeKind::Triangle;
    } else if (v.size() == 4) {
        bool square = true;
        for (size_t k = 0; k < 4; ++k)
            square = square && std::fabs(cornerAngle(v[(k + 3) % 4], v[k], v[(k + 1) % 4]) - kPi / 2) <= kRightAngleTol;
        s.kind = square ? ShapeKind::Rectangle : ShapeKind::Quadrilateral;
        if (square) {
            // Orientation: the sides' directions modulo 90 degrees, averaged (as 4x angles on a circle).
            double cx = 0, cy = 0;
            for (size_t k = 0; k < 4; ++k) {
                const P e = v[(k + 1) % 4] - v[k];
                const double a = 4 * std::atan2(e.y, e.x);
                cx += std::cos(a) * norm(e);
                cy += std::sin(a) * norm(e);
            }
            double theta = std::atan2(cy, cx) / 4;
            if (std::fabs(theta) <= kRectAxisSnap) theta = 0;
            // In the rectangle's own frame, each side is the average of its two corners.
            std::vector<P> r(4);
            for (size_t k = 0; k < 4; ++k) r[k] = rotate(v[k], -theta);
            std::vector<double> xs, ys;
            for (const P& q : r) {
                xs.push_back(q.x);
                ys.push_back(q.y);
            }
            std::sort(xs.begin(), xs.end());
            std::sort(ys.begin(), ys.end());
            const double x0 = (xs[0] + xs[1]) / 2, x1 = (xs[2] + xs[3]) / 2;
            const double y0 = (ys[0] + ys[1]) / 2, y1 = (ys[2] + ys[3]) / 2;
            v = {rotate({x0, y0}, theta), rotate({x1, y0}, theta), rotate({x1, y1}, theta), rotate({x0, y1}, theta)};
        }
    } else {
        return {};
    }
    s.points = v;
    s.points.push_back(v.front());
    return s;
}

bool fitEllipse(const std::vector<P>& ring, double pathLen, Shape& out) {
    P c{0, 0};
    for (const P& q : ring) c = c + q;
    c = c * (1.0 / static_cast<double>(ring.size()));
    double sxx = 0, syy = 0, sxy = 0;
    for (const P& q : ring) {
        const P d = q - c;
        sxx += d.x * d.x;
        syy += d.y * d.y;
        sxy += d.x * d.y;
    }
    double theta = 0.5 * std::atan2(2 * sxy, sxx - syy);
    P u{std::cos(theta), std::sin(theta)}, v{-u.y, u.x};
    double u0 = 1e300, u1 = -1e300, v0 = 1e300, v1 = -1e300;
    for (const P& q : ring) {
        const double du = dot(q - c, u), dv = dot(q - c, v);
        u0 = std::min(u0, du);
        u1 = std::max(u1, du);
        v0 = std::min(v0, dv);
        v1 = std::max(v1, dv);
    }
    double a = (u1 - u0) / 2, b = (v1 - v0) / 2;
    if (a <= 0 || b < 0.15 * a) return false;
    const P center = c + u * ((u0 + u1) / 2) + v * ((v0 + v1) / 2);

    double residual = 0;
    for (const P& q : ring) {
        const double du = dot(q - center, u) / a, dv = dot(q - center, v) / b;
        residual += std::fabs(std::hypot(du, dv) - 1);
    }
    if (residual / static_cast<double>(ring.size()) > 0.12) return false;
    // Drawn once around (not a scribble that loops several times).
    const double h = (a - b) * (a - b) / ((a + b) * (a + b));
    const double perimeter = kPi * (a + b) * (1 + 3 * h / (10 + std::sqrt(4 - 3 * h)));
    if (pathLen < 0.75 * perimeter || pathLen > 1.4 * perimeter) return false;

    if (std::fabs(a - b) <= 0.12 * std::max(a, b)) {
        out.kind = ShapeKind::Circle;
        a = b = (a + b) / 2;
    } else {
        out.kind = ShapeKind::Ellipse;
        const double axis = std::round(theta / (kPi / 2)) * (kPi / 2);
        if (std::fabs(theta - axis) <= 10 * kDeg) {
            u = axisDirection(axis);
            v = {-u.y, u.x};
        }
    }
    out.points.clear();
    for (size_t k = 0; k <= kEllipseSegments; ++k) {
        const double t = 2 * kPi * static_cast<double>(k % kEllipseSegments) / kEllipseSegments;
        out.points.push_back(center + u * (a * std::cos(t)) + v * (b * std::sin(t)));
    }
    return true;
}

}  // namespace

Shape recognizeStroke(const std::vector<StrokePoint>& points) {
    std::vector<P> p = withoutRepeats(points);
    if (p.size() < 2) return {};
    const double size = diagonal(p);
    if (size < kMinSize) return {};

    // Closed if the last stretch comes back to the start (the pen often overshoots it).
    size_t end = p.size() - 1;
    for (size_t k = p.size() * 3 / 4; k < p.size(); ++k)
        if (dist(p[k], p[0]) < dist(p[end], p[0])) end = k;

    // An open stroke can only be a straight line. Its length is measured on a
    // smoothed copy, so the pen's small jitter doesn't count as wandering.
    if (dist(p[end], p[0]) > 0.25 * size) {
        const double gap = dist(p.front(), p.back());
        double worst = 0;
        for (const P& q : p) worst = std::max(worst, distToSegment(q, p.front(), p.back()));
        if (pathLength(resample(p, 24)) <= 1.1 * gap && worst <= std::max(1.5, 0.05 * gap))
            return line(p.front(), p.back());
        return {};
    }
    p.resize(end + 1);  // drop the overshoot
    if (p.size() < 3) return {};

    std::vector<P> ring = resample(p, kSamples);
    const double len = pathLength(ring);
    ring.pop_back();  // the end meets the start
    const std::vector<P> v = corners(ring, std::max(1.5, 0.05 * size), 0.15 * size);
    if (v.size() == 3 || v.size() == 4) return polygon(v);
    Shape e;
    if (fitEllipse(ring, len, e)) return e;
    return {};
}

bool extendChain(std::vector<StrokePoint>& chain, StrokePoint a, StrokePoint b, Shape& closed) {
    closed = {};
    if (chain.size() < 2) {
        chain = {a, b};
        return true;
    }
    double shortest = dist(a, b);
    for (size_t k = 0; k + 1 < chain.size(); ++k) shortest = std::min(shortest, dist(chain[k], chain[k + 1]));
    const double snap = std::max(3.0, 0.25 * shortest);

    if (dist(a, chain.back()) <= snap) {
        chain.back() = mid(chain.back(), a);
        chain.push_back(b);
    } else if (dist(b, chain.back()) <= snap) {
        chain.back() = mid(chain.back(), b);
        chain.push_back(a);
    } else if (dist(b, chain.front()) <= snap) {
        chain.front() = mid(chain.front(), b);
        chain.insert(chain.begin(), a);
    } else if (dist(a, chain.front()) <= snap) {
        chain.front() = mid(chain.front(), a);
        chain.insert(chain.begin(), b);
    } else {
        return false;
    }

    if (chain.size() >= 4 && dist(chain.front(), chain.back()) <= snap) {
        std::vector<P> v(chain.begin(), chain.end() - 1);
        v.front() = mid(chain.front(), chain.back());
        closed = polygon(v);  // stays None unless 3 or 4 sides
    }
    return true;
}

bool makeArrow(StrokePoint lineA, StrokePoint lineB, const std::vector<StrokePoint>& head, Shape& arrow) {
    const std::vector<P> p = withoutRepeats(head);
    if (p.size() < 3) return false;
    const double size = diagonal(p);
    const std::vector<P> v = simplifyOpen(resample(p, 48), std::max(1.0, 0.12 * size));
    if (v.size() != 3) return false;
    const P h1 = v[0], tip = v[1], h2 = v[2];
    const double arm1 = dist(h1, tip), arm2 = dist(h2, tip), opening = cornerAngle(h1, tip, h2);
    if (opening < 20 * kDeg || opening > 120 * kDeg) return false;
    if (std::max(arm1, arm2) > 3 * std::min(arm1, arm2)) return false;
    const double arm = (arm1 + arm2) / 2;
    if (arm < 2 || arm > 0.6 * dist(lineA, lineB)) return false;

    // At whichever end of the line the tip is, opening back along the line.
    const bool atA = dist(tip, lineA) < dist(tip, lineB);
    const P end = atA ? lineA : lineB, tail = atA ? lineB : lineA;
    if (dist(tip, end) > std::max(4.0, 0.6 * arm)) return false;
    const P back = unit(tail - end);
    if (std::acos(std::clamp(dot(back, unit(mid(h1, h2) - tip)), -1.0, 1.0)) > 40 * kDeg) return false;

    const double half = std::clamp(opening / 2, 15 * kDeg, 45 * kDeg);
    arrow.kind = ShapeKind::Arrow;
    arrow.points = {tail, end, end + rotate(back, half) * arm, end, end + rotate(back, -half) * arm};
    return true;
}

}  // namespace scrible
