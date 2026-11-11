#include "Keyboard.h"

namespace scrible {
namespace {

constexpr int kKeyH = 120, kGap = 8, kSide = 20, kEdge = 16;  // kEdge: above and below the keys
constexpr int kCharScale = 6, kWordScale = 4;

}  // namespace

Keyboard::Keyboard(int width, int height, int top) : width_(width), height_(height), top_(top) {
    using K = Key;
    struct Spec {
        Key key;
        std::string label, shifted, symbol;
        double units;  // tenths of the keyboard's width
    };
    // A row of character keys, as they are lowercase, uppercase and among the symbols.
    auto chars = [](const char* lower, const char* upper, const char* symbols) {
        std::vector<Spec> row;
        for (size_t i = 0; lower[i]; ++i) row.push_back({K::Text, {lower[i]}, {upper[i]}, {symbols[i]}, 1});
        return row;
    };
    std::vector<std::vector<Spec>> rows = {
        chars("1234567890", "1234567890", "1234567890"),
        chars("qwertyuiop", "QWERTYUIOP", "!@#$%&*()/"),
        chars("asdfghjkl'", "ASDFGHJKL'", "-+=_:;\"?<>"),
        {{K::Shift, "SHIFT", "SHIFT", "~", 1.5}},
        {{K::Symbols, "?123", "?123", "ABC", 1.5},
         {K::Text, ",", ",", ",", 1},
         {K::Text, "SPACE", "SPACE", "SPACE", 3.5},
         {K::Text, ".", ".", ".", 1},
         {K::Enter, "ENTER", "ENTER", "ENTER", 1.5},
         {K::Done, "DONE", "DONE", "DONE", 1.5}},
    };
    for (const Spec& s : chars("zxcvbnm", "ZXCVBNM", "[]{}\\|`")) rows[3].push_back(s);
    rows[3].push_back({K::Delete, "DEL", "DEL", "DEL", 1.5});

    const double unit = (width - 2.0 * kSide) / 10;
    for (size_t r = 0; r < rows.size(); ++r) {
        double x = kSide;
        const int y = kEdge + static_cast<int>(r) * kKeyH;
        for (const Spec& s : rows[r]) {
            const IRect rect{static_cast<int>(x) + kGap / 2, y + kGap / 2, static_cast<int>(x + s.units * unit) - kGap / 2,
                             y + kKeyH - kGap / 2};
            keys_.push_back({s.key, s.label, s.shifted, s.symbol, s.units, rect});
            x += s.units * unit;
        }
    }
    keyboardH_ = 2 * kEdge + static_cast<int>(rows.size()) * kKeyH;
}

IRect Keyboard::rectAt(bool atTop) const {
    return atTop ? IRect{0, top_, width_, top_ + keyboardH_} : IRect{0, height_ - keyboardH_, width_, height_};
}

void Keyboard::show(bool atTop) {
    open_ = true;
    atTop_ = atTop;
    rect_ = rectAt(atTop);
}

void Keyboard::setShift(bool on) {
    changed_ = changed_ || shift_ != on;
    shift_ = on;
}

const std::string& Keyboard::shown(const KeyDef& k) const {
    if (symbols_) return k.symbol;
    return shift_ ? k.shifted : k.label;
}

Keyboard::Key Keyboard::action(const KeyDef& k) const {
    if (symbols_ && k.key == Key::Shift) return Key::Text;  // a symbol in its place
    return k.key;
}

Keyboard::Press Keyboard::tap(int x, int y) {
    changed_ = false;
    Press p;
    if (!covers(x, y)) return p;
    for (const KeyDef& k : keys_) {
        const IRect r = placed(k);
        if (!r.contains(x, y)) continue;
        p.key = action(k);
        p.r = r;
        switch (p.key) {
            case Key::Text:
                p.text = shown(k) == "SPACE" ? " " : shown(k);
                setShift(false);  // one capital at a time
                break;
            case Key::Shift: setShift(!shift_); break;
            case Key::Symbols:
                symbols_ = !symbols_;
                changed_ = true;
                break;
            default: break;
        }
        return p;
    }
    return p;
}

IRect Keyboard::keyRect(const std::string& label) const {
    if (!open_) return {};
    for (const KeyDef& k : keys_)
        if (shown(k) == label) return placed(k);
    return {};
}

void Keyboard::draw(Canvas& c, const IRect& clip) const {
    if (!open_ || rect_.intersected(clip).empty()) return;
    auto fill = [&](const IRect& r, uint8_t v) { c.fill(r.intersected(clip), v); };
    fill(rect_, 255);
    // A rule between it and the page.
    fill(atTop_ ? IRect{rect_.x0, rect_.y1 - 3, rect_.x1, rect_.y1} : IRect{rect_.x0, rect_.y0, rect_.x1, rect_.y0 + 3}, 0);
    for (const KeyDef& k : keys_) {
        const IRect r = placed(k);
        const bool active = (k.key == Key::Shift && shift_ && !symbols_) || (k.key == Key::Symbols && symbols_);
        fill(r, 0);
        if (!active) fill(r.grown(-3), 255);
        const std::string& label = shown(k);
        const int scale = label.size() == 1 ? kCharScale : kWordScale;
        const int tx = r.x0 + (r.width() - Canvas::textWidth(label, scale)) / 2;
        const int ty = r.y0 + (r.height() - 8 * scale) / 2;
        c.drawText(tx, ty, label, scale, active ? 255 : 0, r.intersected(clip));
    }
}

}  // namespace scrible
