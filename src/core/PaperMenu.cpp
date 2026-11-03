#include "PaperMenu.h"

#include <algorithm>

#include "Widgets.h"

namespace scrible {
namespace {

using namespace ui;

constexpr int kRowGap = 2 * kGap;  // between the rows of choices
constexpr Action kKinds[] = {Action::PaperBlank, Action::PaperRuled, Action::PaperGrid};
constexpr Action kSpacings[] = {Action::PaperNarrow, Action::PaperMedium, Action::PaperWide};

const char* label(Action a, PaperKind kind) {
    const bool grid = kind == PaperKind::Grid;
    switch (a) {
        case Action::PaperBlank: return "BLANK";
        case Action::PaperRuled: return "RULED";
        case Action::PaperGrid: return "GRID";
        case Action::PaperNarrow: return grid ? "SMALL" : "NARROW";
        case Action::PaperMedium: return "MEDIUM";
        case Action::PaperWide: return grid ? "LARGE" : "WIDE";
        case Action::PaperMargin: return "MARGIN";
        case Action::PaperEnds: return "BLANK TOP & BOTTOM";
        case Action::DeletePage: return "DELETE PAGE";
        default: return "";
    }
}

}  // namespace

PaperMenu::PaperMenu(const IRect& anchor, const IRect& area) : anchor_(anchor), area_(area) {}

void PaperMenu::show(const PaperMenuState& s) {
    open_ = true;
    state_ = s;
    buttons_.clear();
    const std::optional<Paper>& paper = s.paper;

    // Rows of three: the kinds, then the spacings, then (ruled) the margin line across.
    const int w = textWidth("NARROW") + 2 * kTextPad;
    const int width = 2 * kPad + 3 * w + 2 * kGap;
    const int x0 = std::clamp(anchor_.x0, area_.x0 + 10, std::max(area_.x0 + 10, area_.x1 - 10 - width));
    const int y0 = area_.y0 + 12;
    int y = y0 + kPad;
    auto row = [&](const Action (&actions)[3]) {
        for (int i = 0; i < 3; ++i) {
            const int x = x0 + kPad + i * (w + kGap);
            buttons_.push_back({actions[i], {x, y, x + w, y + kButtonH}});
        }
        y += kButtonH + kRowGap;
    };
    auto wide = [&](Action a) {
        buttons_.push_back({a, {x0 + kPad, y, x0 + width - kPad, y + kButtonH}});
        y += kButtonH + kRowGap;
    };
    if (s.paperChoices) {
        row(kKinds);
        const bool ruled = paper && paper->kind == PaperKind::Ruled;
        if (ruled || (paper && paper->kind == PaperKind::Grid)) row(kSpacings);
        if (ruled) {
            wide(Action::PaperMargin);
            wide(Action::PaperEnds);
        }
    }
    if (s.canDelete) {
        if (s.paperChoices) y += kRowGap;  // set apart from the paper
        wide(Action::DeletePage);
    }
    rect_ = {x0, y0, x0 + width, y - kRowGap + kPad};
}

void PaperMenu::close() {
    open_ = false;
    state_ = {};
    buttons_.clear();
    rect_ = {};
}

Action PaperMenu::hit(int x, int y) const {
    for (const Button& b : buttons_)
        if (b.r.contains(x, y)) return b.action;
    return Action::None;
}

IRect PaperMenu::buttonRect(Action a) const {
    for (const Button& b : buttons_)
        if (b.action == a) return b.r;
    return {};
}

void PaperMenu::draw(Canvas& c, const IRect& clip) const {
    if (!open_ || rect_.intersected(clip).empty()) return;
    drawBox(c, rect_, false, clip);
    const std::optional<Paper>& paper = state_.paper;
    const PaperKind kind = paper ? paper->kind : PaperKind::Blank;
    for (const Button& b : buttons_) {
        bool on = false;
        if (paper) {
            for (int i = 0; i < 3; ++i) {
                if (b.action == kKinds[i]) on = static_cast<int>(paper->kind) == i;
                if (b.action == kSpacings[i]) on = paper->spacing == i;
            }
        }
        if (b.action == Action::DeletePage) on = state_.deleteArmed;
        drawBox(c, b.r, on, clip);
        if (b.action == Action::PaperMargin || b.action == Action::PaperEnds) {
            drawLabel(c, b.r, b.r.x0 + kTextPad, label(b.action, kind), 0, clip);
            drawCheckBox(c, b.r, paper && (b.action == Action::PaperMargin ? paper->margin : paper->ends), clip);
        } else if (b.action == Action::DeletePage) {
            drawLabel(c, b.r, -1, state_.deleteArmed ? "TAP AGAIN TO DELETE" : label(b.action, kind), on ? 255 : 0, clip);
        } else {
            drawLabel(c, b.r, -1, label(b.action, kind), on ? 255 : 0, clip);
        }
    }
}

}  // namespace scrible
