#pragma once

// In-memory model of a Xournal++ document. It mirrors the .xopp structure
// (pages -> layers -> elements) closely enough that anything we can't edit yet
// survives a load/save round trip untouched.

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "Geometry.h"

namespace scrible {

enum class StrokeTool { Pen, Highlighter, Eraser };  // Eraser: whiteout strokes from original Xournal

struct StrokePoint {
    double x, y;
};

struct Stroke {
    StrokeTool tool = StrokeTool::Pen;
    uint32_t color = 0x000000;  // 0xRRGGBB; like Xournal++, alpha is implied by the tool
    double width = 1.41;          // nominal width, points
    std::vector<StrokePoint> points;
    // Pressure-adjusted width of each segment (points[i] -> points[i+1]).
    // Empty for constant-width strokes, otherwise exactly points.size() - 1 entries.
    std::vector<double> segmentWidths;
    // Attributes we don't interpret (capStyle, fill, style, audio...), written back as-is.
    std::vector<std::pair<std::string, std::string>> extraAttrs;

    double segmentWidth(size_t i) const { return segmentWidths.empty() ? width : segmentWidths[i]; }
    double maxWidth() const;
    // Includes half the widest segment. Cached: call invalidateBounds() after changing
    // points or widths (committed strokes never change, so the editor doesn't need to).
    RectF bounds() const;
    void invalidateBounds() { boundsCache_.valid = false; }

private:
    mutable RectF boundsCache_;
};

// An element we can't edit yet (text, image, LaTeX, link...). Kept as raw XML so
// saving doesn't drop it; `bounds` is a best-effort box for drawing a placeholder.
struct ForeignElement {
    std::string tag;
    std::string xml;
    RectF bounds;
    // For <text>: what's needed to draw it (see Font).
    std::string text;
    double textSize = 0;
    double textWrap = 0;  // wrap width in points, 0 = no wrapping
    double textX = 0, textY = 0;
    uint32_t color = 0x000000;
    // A Scrible math answer: a <text> that also says which function made it, from
    // which numbers (scrible-math="sum" scrible-numbers="12;7.5;-30"). Xournal++ shows
    // the text and ignores the rest.
    std::string mathOp;                    // "sum", "average", "min", "max", "count"; empty otherwise
    std::vector<std::string> mathNumbers;  // as read, and corrected
    // Not saved: the shape of each number's characters as read (see DigitReader), so
    // correcting one can teach the reader.
    std::vector<std::vector<std::array<uint8_t, 16>>> mathShapes;
};

size_t codePoints(const std::string& s);

using Element = std::variant<Stroke, ForeignElement>;

RectF elementBounds(const Element& e);

struct Layer {
    std::string name;  // empty = unnamed
    std::vector<Element> elements;
};

struct Background {
    std::string type = "solid";  // "solid" is drawn; "pdf"/"pixmap" are kept but drawn blank for now
    uint32_t color = 0xffffff;     // 0xRRGGBB
    std::string style = "plain";  // plain, lined, ruled, graph, dotted, ...
    std::string config;           // Xournal++'s settings for the style, e.g. "r1=18,m1=72" (see Paper.h)
    std::string xml;              // original <background> element if loaded from a file; written back unchanged
};

struct Page {
    double width = 0, height = 0;  // points
    Background background;
    std::vector<Layer> layers;
};

struct Document {
    int fileVersion = 4;
    std::vector<Page> pages;
};

Page makeBlankPage(double width, double height, const std::string& style);

}  // namespace scrible
