#pragma once

// Partial erasing: cuts the part of a stroke that the eraser passes over.

#include <vector>

#include "Document.h"

namespace scrible {

// The eraser is a disc of `radius` swept from `from` to `to` (equal for a single
// position). Ink counts as erased where the stroke's centreline comes within
// radius + half the stroke width, so the round cap left at a cut ends at the
// eraser's edge. If the stroke is touched, returns true and fills `pieces` with
// what's left (possibly nothing); attributes and per-segment widths carry over.
bool eraseFromStroke(const Stroke& s, const StrokePoint& from, const StrokePoint& to, double radius,
                     std::vector<Stroke>& pieces);

}  // namespace scrible
