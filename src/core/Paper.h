#pragma once

// The paper a page is printed with, and how .xopp records it: the background's
// style ("plain", "ruled", "lined" = ruled with a margin line, "graph", ...) and
// Xournal++'s config string, e.g. "r1=18,m1=72" (r1: line or square spacing,
// m1: the margin line's x, negative for the right edge; both in points). Scrible adds
// "scrible-full=1" for ruled lines all the way down (Xournal++ ignores it, and keeps
// its blank header and footer).

#include <optional>

#include "Document.h"

namespace scrible {

// Xournal++'s defaults, used when a background's config doesn't say otherwise.
constexpr double kDefaultRuledSpacing = 24.0;   // "ruled", "lined"
constexpr double kDefaultGraphSpacing = 14.17;  // "graph", "dotted": 5 mm squares
constexpr double kDefaultMarginX = 72.0;        // "lined": 1 inch from the left

// A number from the background's config, or `fallback` if it isn't there.
double backgroundSetting(const Background& b, const char* key, double fallback);

// The papers on offer.
enum class PaperKind { Blank, Ruled, Grid };
struct Paper {
    PaperKind kind = PaperKind::Ruled;
    int spacing = 1;     // preset: 0 narrow/small, 1 medium, 2 wide/large; -1 = something else
    bool margin = true;  // ruled: with the vertical margin line
    bool ends = true;    // ruled: blank at the top and the bottom (Xournal++'s header and footer)
};
constexpr double kRuledSpacings[] = {18.0, 24.0, 30.0};    // 6.4, 8.5 (Xournal's), 10.6 mm
constexpr double kGridSpacings[] = {14.17, 21.26, 28.35};  // 5 (Xournal's), 7.5, 10 mm

// The page's paper in those terms; nothing for other backgrounds (dotted, staves,
// PDF, ...).
std::optional<Paper> paperOf(const Background& b);
// Turns `b` into that paper, keeping its color. A spacing of -1 keeps the current one.
void setPaper(Background& b, const Paper& p);

}  // namespace scrible
