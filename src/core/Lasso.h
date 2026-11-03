#pragma once

// Lasso selection geometry.

#include <vector>

#include "Document.h"

namespace scrible {

bool pointInPolygon(const StrokePoint& p, const std::vector<StrokePoint>& polygon);

// Indices of the strokes that a lasso loop selects: those with most of their
// points inside it, so a loose loop around a word still catches every letter.
std::vector<size_t> strokesInLasso(const std::vector<Element>& elements, const std::vector<StrokePoint>& loop);

Stroke movedStroke(const Stroke& s, double dx, double dy);
// Scaled about `anchor`, line widths included, so resized writing keeps its look.
Stroke scaledStroke(const Stroke& s, const StrokePoint& anchor, double factor);

}  // namespace scrible
