#include "Toolbar.h"

namespace xs {
namespace {

struct Spec {
    Action action;
    const char* label;  // for Action::None: the widest text the page counter must fit
};

const std::vector<std::vector<Spec>>& groups() {
    static const std::vector<std::vector<Spec>> g = {
        {{Action::Pen, "PEN"}, {Action::Highlighter, "MARK"}, {Action::Eraser, "ERASE"}},
        {{Action::SizeFine, "S"}, {Action::SizeMedium, "M"}, {Action::SizeThick, "L"}},
        {{Action::Undo, "UNDO"}, {Action::Redo, "REDO"}},
        {{Action::PrevPage, "<"}, {Action::None, "000/000"}, {Action::NextPage, ">"}},
        {{Action::Save, "SAVE*"}, {Action::Exit, "EXIT"}},
    };
    return g;
}

bool isActive(Action a, const ToolbarState& s) {
    switch (a) {
        case Action::Pen: return s.tool == ToolKind::Pen;
        case Action::Highlighter: return s.tool == ToolKind::Highlighter;
        case Action::Eraser: return s.tool == ToolKind::Eraser;
        case Action::SizeFine: return s.size == ToolSize::Fine;
        case Action::SizeMedium: return s.size == ToolSize::Medium;
        case Action::SizeThick: return s.size == ToolSize::Thick;
        default: return false;
    }
}

}  // namespace

Toolbar::Toolbar(int width, int height) : rect_{0, 0, width, height} {
    const int margin = 8, innerGap = 6, minGroupGap = 16;
    const int vpad = std::max(4, height / 10);
    const int y0 = vpad, y1 = height - vpad - 3;  // 3px bottom border

    // Largest glyph scale at which everything fits on one row.
    for (scale_ = 4; scale_ > 1; --scale_) {
        int total = 2 * margin + minGroupGap * static_cast<int>(groups().size() - 1);
        for (const auto& g : groups()) {
            for (const Spec& s : g) total += std::max(24 * scale_, Canvas::textWidth(s.label, scale_) + 8 * scale_);
            total += innerGap * static_cast<int>(g.size() - 1);
        }
        if (total <= width) break;
    }

    int used = 2 * margin;
    for (const auto& g : groups()) {
        for (const Spec& s : g) used += std::max(24 * scale_, Canvas::textWidth(s.label, scale_) + 8 * scale_);
        used += innerGap * static_cast<int>(g.size() - 1);
    }
    const int groupGap = std::max(minGroupGap, (width - used) / static_cast<int>(groups().size() - 1));

    int x = margin;
    for (const auto& g : groups()) {
        for (const Spec& s : g) {
            const int w = std::max(24 * scale_, Canvas::textWidth(s.label, scale_) + 8 * scale_);
            const IRect r{x, y0, x + w, y1};
            if (s.action == Action::None) pageLabel_ = r;
            else buttons_.push_back({s.action, s.label, r});
            x += w + innerGap;
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
        const bool enabled = !(b.action == Action::Undo && !s.canUndo) && !(b.action == Action::Redo && !s.canRedo);
        const std::string label = b.action == Action::Save ? (s.modified ? "SAVE*" : "SAVE") : b.label;
        if (isActive(b.action, s)) {
            c.fill(b.r, 0);
            centered(b.r, label, 255);
        } else {
            c.strokeRect(b.r, 3, 0);
            centered(b.r, label, 0);
        }
        if (!enabled) {
            // "Grayed out" with a checkerboard rather than gray: the toolbar is refreshed
            // with DU, which can't draw gray.
            for (int y = b.r.y0; y < b.r.y1; ++y)
                for (int x = b.r.x0 + ((b.r.x0 + y) & 1); x < b.r.x1; x += 2) c.row(y)[x] = 255;
        }
    }

    const std::string page = s.status.empty()
                                 ? std::to_string(s.page + 1) + "/" + std::to_string(s.pageCount)
                                 : s.status;
    centered(pageLabel_, page, 0);
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

}  // namespace xs
