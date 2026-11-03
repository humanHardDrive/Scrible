#pragma once

// Shape recognition: turns a hand-drawn stroke into a clean straight line,
// triangle, quadrilateral, rectangle, circle or ellipse, and pairs of strokes into
// arrows or polygons. Written from scratch after the behaviour of the original
// Xournal's shape recognizer (as its manual describes it), not from its code.

#include <vector>

#include "Document.h"

namespace scrible {

enum class ShapeKind { None, Line, Triangle, Quadrilateral, Rectangle, Circle, Ellipse, Arrow };

struct Shape {
    ShapeKind kind = ShapeKind::None;
    // The clean shape as a polyline; closed shapes end where they start.
    std::vector<StrokePoint> points;
};

// One stroke on its own. Lines within a few degrees of horizontal or vertical are
// straightened; four-sided shapes with right angles become exact rectangles.
Shape recognizeStroke(const std::vector<StrokePoint>& points);

// Lines drawn one after another: adds the line a-b to the chain of connected
// vertices `chain` if it meets either end (true), and turns a chain that closes up
// with three or four sides into a polygon (`closed`, else kind None).
bool extendChain(std::vector<StrokePoint>& chain, StrokePoint a, StrokePoint b, Shape& closed);

// A line followed by a "V" drawn at one of its ends becomes an arrow.
bool makeArrow(StrokePoint lineA, StrokePoint lineB, const std::vector<StrokePoint>& head, Shape& arrow);

}  // namespace scrible
