#include "Document.h"

namespace scrible {

double Stroke::maxWidth() const {
    double w = width;
    for (double s : segmentWidths) w = std::max(w, s);
    return w;
}

RectF Stroke::bounds() const {
    if (boundsCache_.valid) return boundsCache_;
    RectF r;
    for (const auto& p : points) r.add(p.x, p.y);
    r.grow(maxWidth() / 2);
    boundsCache_ = r;
    return r;
}

size_t codePoints(const std::string& s) {
    size_t n = 0;
    for (char c : s)
        if ((static_cast<unsigned char>(c) & 0xc0) != 0x80) ++n;
    return n;
}

RectF elementBounds(const Element& e) {
    if (const auto* s = std::get_if<Stroke>(&e)) return s->bounds();
    return std::get<ForeignElement>(e).bounds;
}

Page makeBlankPage(double width, double height, const std::string& style) {
    Page p;
    p.width = width;
    p.height = height;
    p.background.style = style;
    p.layers.emplace_back();
    return p;
}

}  // namespace scrible
