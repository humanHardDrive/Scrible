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

std::vector<std::string> textLines(const ForeignElement& f) {
    const size_t maxChars = f.textWrap > 0 && f.textSize > 0
                                ? std::max<size_t>(1, static_cast<size_t>(f.textWrap / f.textSize))
                                : static_cast<size_t>(-1);
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= f.text.size()) {
        size_t end = f.text.find('\n', start);
        if (end == std::string::npos) end = f.text.size();
        std::string line;
        size_t len = 0, pos = start;
        while (pos < end) {
            size_t wordEnd = f.text.find(' ', pos);
            if (wordEnd == std::string::npos || wordEnd > end) wordEnd = end;
            const std::string word = f.text.substr(pos, wordEnd - pos);
            const size_t wlen = codePoints(word);
            if (len && len + 1 + wlen > maxChars) {
                lines.push_back(std::move(line));
                line.clear();
                len = 0;
            }
            if (len) {
                line += ' ';
                ++len;
            }
            line += word;
            len += wlen;
            pos = wordEnd + 1;
        }
        lines.push_back(std::move(line));
        start = end + 1;
    }
    return lines;
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
