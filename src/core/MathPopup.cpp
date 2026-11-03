#include "MathPopup.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace scrible {
namespace {

constexpr int kPad = 30, kBorder = 4;
constexpr int kTitleScale = 5, kCellScale = 6, kKeyScale = 5, kTextScale = 3;
constexpr int kCellW = 64, kCellH = 88, kCellGap = 6, kRowGap = 10;
constexpr int kKeyRows = 2, kDoneH = 88;
constexpr const char kKeys[] = "0123456789.,-";

const char* opName(Action op) {
    switch (op) {
        case Action::MathSum: return "SUM";
        case Action::MathAverage: return "AVERAGE";
        case Action::MathMin: return "MIN";
        case Action::MathMax: return "MAX";
        case Action::MathCount: return "COUNT";
        default: return "";
    }
}

// Up to six decimals, trailing zeros trimmed: 49.5, 1250, -0.125.
std::string format(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6f", v);
    char* end = buf + std::strlen(buf);
    while (end[-1] == '0') --end;
    if (end[-1] == '.') --end;
    *end = '\0';
    return std::strcmp(buf, "-0") == 0 ? "0" : buf;
}

void frame(Canvas& c, const IRect& r, int border, bool on, const IRect& clip) {
    c.fill(r.intersected(clip), 0);
    if (!on) c.fill(r.grown(-border).intersected(clip), 255);
}

void text(Canvas& c, int x, int y, const std::string& s, int scale, uint8_t v, const IRect& clip) {
    c.drawText(x, y, s, scale, v, clip);
}

void centered(Canvas& c, const IRect& r, const std::string& s, int scale, uint8_t v, const IRect& clip) {
    text(c, r.x0 + (r.width() - Canvas::textWidth(s, scale)) / 2, r.y0 + (r.height() - 8 * scale) / 2, s, scale, v,
         r.intersected(clip));
}

}  // namespace

MathPopup::MathPopup(const IRect& area) : area_(area) {}

void MathPopup::show(Action op, std::vector<ReadNumber> numbers) {
    open_ = true;
    op_ = op;
    numbers_ = std::move(numbers);
    pickedNumber_ = pickedChar_ = -1;
    layout();
}

void MathPopup::close() {
    open_ = false;
    numbers_.clear();
    rect_ = done_ = {};
    keys_.clear();
}

// Always the same size while open (room for the keys included), so changes inside
// it are black and white over black and white: DU.
void MathPopup::layout() {
    const int w = std::min(area_.width() - 80, 1400);
    const int fixed = 2 * kPad + 8 * kTitleScale + 30 + 8 * kTextScale + 50 + 8 * kTextScale + 20 +
                      kKeyRows * (kCellH + kCellGap) + 20 + kDoneH;
    const int maxRows = std::max(1, (area_.height() - 80 - fixed) / (kCellH + kRowGap));
    rows_ = std::min(numbers_.size(), static_cast<size_t>(maxRows));
    const int h = fixed + static_cast<int>(std::max<size_t>(rows_, 1)) * (kCellH + kRowGap);
    const int x0 = area_.x0 + (area_.width() - w) / 2;
    const int y0 = area_.y0 + std::max(40, (area_.height() - h) / 2);
    rect_ = {x0, y0, x0 + w, y0 + h};
    done_ = {x0 + kPad, rect_.y1 - kPad - kDoneH, rect_.x1 - kPad, rect_.y1 - kPad};

    keys_.clear();
    if (pickedNumber_ < 0) return;
    const int perRow = std::max(1, (w - 2 * kPad + kCellGap) / (kCellW + kCellGap));
    const int keysTop = done_.y0 - 20 - kKeyRows * (kCellH + kCellGap);
    for (int i = 0; kKeys[i]; ++i) {
        const int x = x0 + kPad + (i % perRow) * (kCellW + kCellGap), y = keysTop + (i / perRow) * (kCellH + kCellGap);
        keys_.push_back({kKeys[i], {x, y, x + kCellW, y + kCellH}});
    }
}

IRect MathPopup::cellRect(size_t number, size_t ch) const {
    if (!open_ || number >= rows_ || ch >= numbers_[number].chars.size()) return {};
    const int x = rect_.x0 + kPad + static_cast<int>(ch) * (kCellW + kCellGap);
    const int y = rect_.y0 + kPad + 8 * kTitleScale + 30 + static_cast<int>(number) * (kCellH + kRowGap);
    if (x + kCellW > rect_.x1 - kPad) return {};  // too long to show
    return {x, y, x + kCellW, y + kCellH};
}

IRect MathPopup::keyRect(char c) const {
    for (const Key& k : keys_)
        if (k.c == c) return k.r;
    return {};
}

std::string MathPopup::result() const {
    std::vector<double> v;
    for (const ReadNumber& n : numbers_) {
        double x;
        if (n.parse(x)) v.push_back(x);
    }
    const std::string name = opName(op_);
    if (op_ == Action::MathCount) return name + " = " + std::to_string(v.size());
    if (v.empty()) return name + ": NO NUMBERS";
    double r = 0;
    switch (op_) {
        case Action::MathSum:
        case Action::MathAverage:
            for (double x : v) r += x;
            if (op_ == Action::MathAverage) r /= static_cast<double>(v.size());
            break;
        case Action::MathMin: r = *std::min_element(v.begin(), v.end()); break;
        case Action::MathMax: r = *std::max_element(v.begin(), v.end()); break;
        default: break;
    }
    return name + " OF " + std::to_string(v.size()) + " = " + format(r);
}

MathPopup::Tap MathPopup::tap(int x, int y) {
    Tap t;
    if (done_.contains(x, y)) {
        t.close = true;
        return t;
    }
    for (const Key& k : keys_) {
        if (!k.r.contains(x, y)) continue;
        ReadChar& c = numbers_[static_cast<size_t>(pickedNumber_)].chars[static_cast<size_t>(pickedChar_)];
        c.c = k.c;
        if (k.c >= '0' && k.c <= '9' && !c.strokes.empty()) {
            t.taught = true;
            t.features = c.features;
            t.digit = k.c - '0';
        }
        pickedNumber_ = pickedChar_ = -1;
        layout();
        t.changed = true;
        return t;
    }
    for (size_t n = 0; n < rows_; ++n) {
        for (size_t ch = 0; ch < numbers_[n].chars.size(); ++ch) {
            if (!cellRect(n, ch).contains(x, y)) continue;
            const bool same = pickedNumber_ == static_cast<int>(n) && pickedChar_ == static_cast<int>(ch);
            pickedNumber_ = same ? -1 : static_cast<int>(n);  // tapping it again lets go
            pickedChar_ = same ? -1 : static_cast<int>(ch);
            layout();
            t.changed = true;
            return t;
        }
    }
    return t;
}

void MathPopup::draw(Canvas& c, const IRect& clip) const {
    if (!open_ || rect_.intersected(clip).empty()) return;
    frame(c, rect_, kBorder, false, clip);
    const IRect inner = rect_.grown(-kPad);
    text(c, inner.x0, inner.y0, result(), kTitleScale, 0, inner.intersected(clip));

    for (size_t n = 0; n < rows_; ++n) {
        const ReadNumber& num = numbers_[n];
        IRect last;
        for (size_t ch = 0; ch < num.chars.size(); ++ch) {
            const IRect r = cellRect(n, ch);
            if (r.empty()) break;
            const bool picked = pickedNumber_ == static_cast<int>(n) && pickedChar_ == static_cast<int>(ch);
            frame(c, r, 2, picked, clip);
            centered(c, r, std::string(1, num.chars[ch].c), kCellScale, picked ? 255 : 0, clip);
            last = r;
        }
        double v;
        if (!num.parse(v) && !last.empty())  // left out of the answer
            text(c, last.x1 + 24, last.y0 + (kCellH - 8 * kTextScale) / 2, "NOT A NUMBER", kTextScale, 0,
                 inner.intersected(clip));
    }
    int y = inner.y0 + 8 * kTitleScale + 30 + static_cast<int>(std::max<size_t>(rows_, 1)) * (kCellH + kRowGap);
    if (numbers_.size() > rows_)
        text(c, inner.x0, y, "AND " + std::to_string(numbers_.size() - rows_) + " MORE", kTextScale, 0,
             inner.intersected(clip));
    y += 8 * kTextScale + 50 - 30;
    text(c, inner.x0, y, pickedNumber_ < 0 ? "TAP A CHARACTER THAT WAS MISREAD TO FIX IT." : "WHAT SHOULD IT BE?",
         kTextScale, 0, inner.intersected(clip));
    for (const Key& k : keys_) {
        frame(c, k.r, 2, false, clip);
        centered(c, k.r, std::string(1, k.c), kKeyScale, 0, clip);
    }
    frame(c, done_, 3, false, clip);
    centered(c, done_, "DONE", 4, 0, clip);
}

}  // namespace scrible
