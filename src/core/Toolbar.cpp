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

// `text` in lines of at most `chars` characters, broken at spaces where it can be.
std::vector<std::string> wrapText(const std::string& text, size_t chars) {
    std::vector<std::string> lines(1);
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find(' ', start);
        if (end == std::string::npos) end = text.size();
        std::string word = text.substr(start, end - start);
        while (word.size() > chars) {  // longer than a line: broken anywhere
            if (!lines.back().empty()) lines.emplace_back();
            lines.back() = word.substr(0, chars);
            word = word.substr(chars);
            lines.emplace_back();
        }
        std::string& line = lines.back();
        if (line.empty()) line = word;
        else if (line.size() + 1 + word.size() <= chars) line += " " + word;
        else if (!word.empty()) lines.push_back(word);
        start = end + 1;
    }
    if (lines.size() > 1 && lines.back().empty()) lines.pop_back();
    return lines;
}

}  // namespace

Toolbar::Toolbar(int width, int height, ToolbarGroups groups) : rect_{0, 0, width, height} {
    const int margin = 8, innerGap = 6, minGroupGap = 16;
    const int vpad = std::max(4, height / 10);
    const int y0 = vpad, y1 = height - vpad - 3;  // 3px bottom border
    const int gaps = std::max<int>(1, static_cast<int>(groups.size()) - 1);

    auto isTitle = [](const ToolbarButton& b) { return b.action == Action::None && b.label.empty(); };
    bool hasTitle = false;
    for (const auto& g : groups)
        for (const ToolbarButton& b : g) hasTitle = hasTitle || isTitle(b);
    auto usedWidth = [&](int scale) {
        int used = 2 * margin;
        for (const auto& g : groups) {
            for (const ToolbarButton& b : g)
                if (!isTitle(b)) used += buttonWidth(b.label, scale);
            used += innerGap * static_cast<int>(g.size() - 1);
        }
        return used;
    };
    // Largest glyph scale at which everything fits on one row, with room in the title
    // for 8 characters at least.
    auto titleRoom = [&](int scale) { return hasTitle ? 8 * 8 * std::max(1, scale - 1) : 0; };
    for (scale_ = 4; scale_ > 1 && usedWidth(scale_) + minGroupGap * gaps + titleRoom(scale_) > width; --scale_) {
    }
    // The groups are spread across the width, unless there's a title to take the room.
    const int spare = std::max(0, width - usedWidth(scale_) - minGroupGap * gaps);
    const int groupGap = groups.size() < 2 ? 0 : hasTitle ? minGroupGap : minGroupGap + spare / gaps;

    int x = margin;
    for (const auto& g : groups) {
        for (const ToolbarButton& b : g) {
            const IRect r{x, y0, x + (isTitle(b) ? spare : buttonWidth(b.label, scale_)), y1};
            if (isTitle(b)) titleSlot_ = r;
            else if (b.action == Action::None) textSlot_ = r;
            else buttons_.push_back({b.action, b.label, r});
            x = r.x1 + innerGap;
        }
        x += groupGap - innerGap;
    }
}

void Toolbar::draw(Canvas& c, const ToolbarState& s) const {
    c.fill(rect_, 255);
    c.fill({rect_.x0, rect_.y1 - 3, rect_.x1, rect_.y1}, 0);

    if (s.titleOnly) {
        // The title across the whole bar: as large as fits, on two lines if need be,
        // and shortened only if even that isn't enough.
        const IRect area{rect_.x0 + 16, rect_.y0 + 6, rect_.x1 - 16, rect_.y1 - 9};
        int scale = scale_;
        std::vector<std::string> lines;
        for (;; --scale) {
            const size_t chars = static_cast<size_t>(std::max(1, area.width() / (8 * scale)));
            const size_t fits = static_cast<size_t>(std::max(1, area.height() / (10 * scale)));
            lines = wrapText(s.title, chars);
            if (lines.size() <= fits || scale == 1) {
                if (lines.size() > fits) {
                    lines.resize(fits);
                    std::string& last = lines.back();
                    last = last.substr(0, chars > 3 ? std::min(last.size(), chars - 3) : 0) + "...";
                }
                break;
            }
        }
        const int lineH = 10 * scale, top = area.y0 + (area.height() - static_cast<int>(lines.size()) * lineH) / 2;
        for (size_t i = 0; i < lines.size(); ++i)
            c.drawText(area.x0 + (area.width() - Canvas::textWidth(lines[i], scale)) / 2,
                       top + static_cast<int>(i) * lineH + scale, lines[i], scale, 0, area);
        return;
    }

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
    if (!titleSlot_.empty() && !s.title.empty()) {
        // A size smaller than the buttons, shortened with "..." to fit.
        const int scale = std::max(1, scale_ - 1);
        const size_t room = static_cast<size_t>(titleSlot_.width() / (8 * scale));
        std::string t = s.title;
        if (t.size() > room) t = room > 3 ? t.substr(0, room - 3) + "..." : t.substr(0, room);
        const int tx = titleSlot_.x0 + (titleSlot_.width() - Canvas::textWidth(t, scale)) / 2;
        const int ty = titleSlot_.y0 + (titleSlot_.height() - 8 * scale) / 2;
        c.drawText(tx, ty, t, scale, 0, titleSlot_);
    }
}

Action Toolbar::hit(int x, int y) const {
    if (!rect_.contains(x, y)) return Action::None;
    // Buttons respond across the full toolbar height: easier to hit with a finger.
    for (const Button& b : buttons_)
        if (x >= b.r.x0 && x < b.r.x1) return b.action;
    if (x >= titleSlot_.x0 && x < titleSlot_.x1) return Action::Title;
    return Action::None;
}

IRect Toolbar::buttonRect(Action a) const {
    for (const Button& b : buttons_)
        if (b.action == a) return b.r;
    return {};
}

}  // namespace scrible
