#include "Toolbar.h"

#include <algorithm>

namespace scrible {
namespace {

template <typename T>
bool contains(const std::vector<T>& v, const T& x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

int buttonWidth(const std::string& label, int scale) {
    return std::max(24 * scale, Canvas::textWidth(label, scale) + 8 * scale);
}

}  // namespace

Toolbar::Toolbar(int width, int height, ToolbarGroups groups) : rect_{0, 0, width, height} {
    const int margin = 8, innerGap = 6, minGroupGap = 16;
    const int vpad = std::max(4, height / 10);
    const int y0 = vpad, y1 = height - vpad - 3;  // 3px bottom border
    const int gaps = std::max<int>(1, static_cast<int>(groups.size()) - 1);

    auto usedWidth = [&](int scale) {
        int used = 2 * margin;
        for (const auto& g : groups) {
            for (const ToolbarButton& b : g) used += buttonWidth(b.label, scale);
            used += innerGap * static_cast<int>(g.size() - 1);
        }
        return used;
    };
    // Largest glyph scale at which everything fits on one row.
    for (scale_ = 4; scale_ > 1 && usedWidth(scale_) + minGroupGap * gaps > width; --scale_) {
    }
    const int groupGap = groups.size() > 1 ? std::max(minGroupGap, (width - usedWidth(scale_)) / gaps) : 0;

    int x = margin;
    for (const auto& g : groups) {
        for (const ToolbarButton& b : g) {
            const IRect r{x, y0, x + buttonWidth(b.label, scale_), y1};
            if (b.action == Action::None) textSlot_ = r;
            else buttons_.push_back({b.action, b.label, r});
            x = r.x1 + innerGap;
        }
        x += groupGap - innerGap;
    }
}

void Toolbar::draw(Canvas& c, const ToolbarState& s) const {
    c.fill(rect_, 255);
    c.fill({rect_.x0, rect_.y1 - 3, rect_.x1, rect_.y1}, 0);

    auto centered = [&](const IRect& r, const std::string& text, uint8_t value) {
        const int tx = r.x0 + (r.width() - Canvas::textWidth(text, scale_)) / 2;
        const int ty = r.y0 + (r.height() - 8 * scale_) / 2;
        c.drawText(tx, ty, text, scale_, value, r);
    };

    for (const Button& b : buttons_) {
        std::string label = b.label;
        for (const auto& [action, text] : s.labels)
            if (action == b.action) label = text;
        if (contains(s.active, b.action)) {
            c.fill(b.r, 0);
            centered(b.r, label, 255);
        } else {
            c.strokeRect(b.r, 3, 0);
            centered(b.r, label, 0);
        }
        if (contains(s.disabled, b.action)) {
            // "Grayed out" with a checkerboard rather than gray: DU can't draw gray.
            for (int y = b.r.y0; y < b.r.y1; ++y)
                for (int x = b.r.x0 + ((b.r.x0 + y) & 1); x < b.r.x1; x += 2) c.row(y)[x] = 255;
        }
    }
    if (!textSlot_.empty()) centered(textSlot_, s.text, 0);
}

Action Toolbar::hit(int x, int y) const {
    if (!rect_.contains(x, y)) return Action::None;
    // Buttons respond across the full toolbar height: easier to hit with a finger.
    for (const Button& b : buttons_)
        if (x >= b.r.x0 && x < b.r.x1) return b.action;
    return Action::None;
}

IRect Toolbar::buttonRect(Action a) const {
    for (const Button& b : buttons_)
        if (b.action == a) return b.r;
    return {};
}

}  // namespace scrible
