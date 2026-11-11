#include "ToolPanel.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>

#include "Font.h"
#include "Widgets.h"

namespace scrible {
namespace {

using namespace ui;

constexpr int kTop = 24;    // between the toolbar and the tab
constexpr int kAside = 12;  // between the column and the options
constexpr int kArrow = 16;        // the tab's arrow (twice as tall), and the space either side of it
constexpr int kArrowPad = 28;

constexpr Action kColumn[] = {Action::Pen,    Action::Shapes, Action::Lasso, Action::Highlighter,
                             Action::Eraser, Action::Text,   Action::Pan};
constexpr Action kSizes[] = {Action::SizeFine, Action::SizeMedium, Action::SizeThick};
constexpr Action kLines[] = {Action::LineSolid, Action::LineDots, Action::LineDashes};

LineStyle lineOf(Action a) {
    return a == Action::LineDots ? LineStyle::Dots : a == Action::LineDashes ? LineStyle::Dashes : LineStyle::Solid;
}

// A sample of a line across r, t pixels thick: solid, dotted or dashed.
void drawSampleLine(Canvas& c, const IRect& r, int t, LineStyle line, uint8_t ink, const IRect& clip) {
    const int y = r.y0 + (r.height() - t) / 2;
    const int x0 = r.x0 + kTextPad, x1 = r.x1 - kTextPad;
    const int on = line == LineStyle::Dots ? t : line == LineStyle::Dashes ? std::max(3 * t, 18) : x1 - x0;
    const int off = line == LineStyle::Dots ? std::max(t, 8) : std::max(t, 10);
    for (int x = x0; x < x1; x += on + off) c.fill(IRect{x, y, std::min(x + on, x1), y + t}.intersected(clip), ink);
}

const char* label(Action a) {
    switch (a) {
        case Action::Pen: return "PEN";
        case Action::Shapes: return "SHAPE";
        case Action::Lasso: return "SELECT";
        case Action::Highlighter: return "MARK";
        case Action::Eraser: return "ERASE";
        case Action::Text: return "TEXT";
        case Action::Pan: return "PAN";
        case Action::ZoomFit: return "FIT";
        case Action::ErasePartial: return "PARTIAL";
        case Action::EraseWhole: return "WHOLE";
        default: return "";
    }
}

}  // namespace

ToolPanel::ToolPanel(const IRect& area) : area_(area) {
    tab_ = {area_.x0, area_.y0 + kTop, area_.x0 + 2 * kArrowPad + kArrow, area_.y0 + kTop + kButtonH};
}

void ToolPanel::setOpen(bool open) {
    open_ = open;
    if (!open) options_ = Action::None;
    layout();
}

void ToolPanel::showOptions(Action tool) {
    open_ = true;
    options_ = tool;
    layout();
}

void ToolPanel::layout() {
    buttons_.clear();
    column_ = optionsRect_ = {};
    if (!open_) return;

    // The column grows out of the tab, down from the same corner.
    const int columnW = textWidth("SHAPE") + 2 * kGap + kBox + 2 * kTextPad + 2 * kPad;
    // The tools, then (set apart) zoom out / in side by side, and FIT.
    const int n = static_cast<int>(std::size(kColumn)) + 2;
    column_ = {tab_.x0, tab_.y0, tab_.x0 + columnW, tab_.y0 + 2 * kPad + n * kButtonH + (n - 1) * kGap + 2 * kGap};
    const int x0 = column_.x0 + kPad, x1 = column_.x1 - kPad, xm = (x0 + x1) / 2;
    int y = column_.y0 + kPad;
    for (Action a : kColumn) {
        buttons_.push_back({a, {x0, y, x1, y + kButtonH}});
        y += kButtonH + kGap;
    }
    y += 2 * kGap;
    buttons_.push_back({Action::ZoomOut, {x0, y, xm - kGap / 2, y + kButtonH}});
    buttons_.push_back({Action::ZoomIn, {xm + kGap / 2, y, x1, y + kButtonH}});
    y += kButtonH + kGap;
    buttons_.push_back({Action::ZoomFit, {x0, y, x1, y + kButtonH}});
    if (options_ == Action::None) return;

    std::vector<Action> opts(std::begin(kSizes), std::end(kSizes));
    if (options_ == Action::Eraser) opts.insert(opts.end(), {Action::ErasePartial, Action::EraseWhole});
    if (options_ == Action::Pen) opts.insert(opts.end(), std::begin(kLines), std::end(kLines));
    const int m = static_cast<int>(opts.size());
    const int modesGap = m > 3 ? 2 * kGap : 0;  // the eraser's modes and the pen's lines, set apart from the sizes
    const int w = textWidth("PARTIAL") + 2 * kTextPad + 2 * kPad;
    const int h = 2 * kPad + m * kButtonH + (m - 1) * kGap + modesGap;
    // Level with the tool's button, as far as the page allows.
    const int y0 = std::max(area_.y0, std::min(buttonRect(options_).y0 - kPad, area_.y1 - h));
    optionsRect_ = {column_.x1 + kAside, y0, column_.x1 + kAside + w, y0 + h};
    y = y0 + kPad;
    for (int i = 0; i < m; ++i) {
        if (i == 3) y += modesGap;
        buttons_.push_back({opts[i], {optionsRect_.x0 + kPad, y, optionsRect_.x1 - kPad, y + kButtonH}});
        y += kButtonH + kGap;
    }
}

std::vector<IRect> ToolPanel::rects() const {
    if (!open_) return {tab_};
    std::vector<IRect> r = {column_};
    if (!optionsRect_.empty()) r.push_back(optionsRect_);
    return r;
}

bool ToolPanel::covers(int x, int y) const {
    for (const IRect& r : rects())
        if (r.contains(x, y)) return true;
    return false;
}

Action ToolPanel::hit(int x, int y) const {
    if (!open_) return tab_.contains(x, y) ? Action::Tools : Action::None;
    for (const Button& b : buttons_)
        if (b.r.contains(x, y)) return b.action;
    return Action::None;
}

IRect ToolPanel::buttonRect(Action a) const {
    if (!open_) return a == Action::Tools ? tab_ : IRect{};
    for (const Button& b : buttons_)
        if (b.action == a) return b.r;
    return {};
}

void ToolPanel::draw(Canvas& c, const ToolPanelState& s, const IRect& clip) const {
    auto fill = [&](const IRect& r, uint8_t v) { c.fill(r.intersected(clip), v); };
    auto box = [&](const IRect& r, bool on) { drawBox(c, r, on, clip); };

    if (!open_) {
        box(tab_, false);
        // An arrow pointing away from the edge: the tools come out this way.
        const int ax = tab_.x0 + kArrowPad, cy = (tab_.y0 + tab_.y1) / 2;
        for (int i = 0; i < kArrow; ++i) fill({ax + i, cy - (kArrow - i), ax + i + 1, cy + (kArrow - i)}, 0);
        return;
    }

    box(column_, false);
    if (!optionsRect_.empty()) box(optionsRect_, false);
    for (const Button& b : buttons_) {
        const auto size = std::find(std::begin(kSizes), std::end(kSizes), b.action);
        bool on = b.action == s.tool;
        if (size != std::end(kSizes)) on = size - std::begin(kSizes) == static_cast<int>(s.size);
        if (b.action == Action::ErasePartial) on = !s.eraseWhole;
        if (b.action == Action::EraseWhole) on = s.eraseWhole;
        const bool lineButton = std::find(std::begin(kLines), std::end(kLines), b.action) != std::end(kLines);
        if (lineButton) on = lineOf(b.action) == s.line;
        box(b.r, on);
        const uint8_t ink = on ? 255 : 0;

        if (size != std::end(kSizes) && s.tool == Action::Text) {
            // A sample of the text, as big as on the page (within reason).
            const Font& font = Font::sans();
            const double px = std::clamp(s.widthsPx[static_cast<size_t>(size - std::begin(kSizes))], 16.0,
                                         (b.r.height() - 12) / font.lineHeight());
            const std::string sample = "Aa";
            const double w = font.advance(sample, 0, sample.size()) * px;
            font.draw(c, b.r.x0 + (b.r.width() - w) / 2, b.r.y0 + (b.r.height() - font.lineHeight() * px) / 2, sample, 0,
                      sample.size(), px, ink, b.r.intersected(clip), false);
        } else if (size != std::end(kSizes)) {
            // A sample of the line it draws, as thick as on the page (within reason), and
            // for the pen dotted or dashed like it.
            const double px = s.widthsPx[static_cast<size_t>(size - std::begin(kSizes))];
            const int t = std::clamp(static_cast<int>(std::lround(px)), 2, b.r.height() - 2 * kTextPad);
            drawSampleLine(c, b.r, t, s.tool == Action::Pen ? s.line : LineStyle::Solid, ink, clip);
        } else if (lineButton) {
            drawSampleLine(c, b.r, 6, lineOf(b.action), ink, clip);
        } else if (b.action == Action::ZoomOut || b.action == Action::ZoomIn) {
            // Drawn rather than lettered: the font's "-" is a hairline.
            const int cx = (b.r.x0 + b.r.x1) / 2, cy = (b.r.y0 + b.r.y1) / 2, arm = 16, half = 3;
            fill({cx - arm, cy - half, cx + arm, cy + half}, ink);
            if (b.action == Action::ZoomIn) fill({cx - half, cy - arm, cx + half, cy + arm}, ink);
        } else if (b.action == Action::ErasePartial || b.action == Action::EraseWhole || b.action == Action::ZoomFit) {
            drawLabel(c, b.r, -1, label(b.action), ink, clip);
        } else {
            drawLabel(c, b.r, b.r.x0 + kTextPad, label(b.action), ink, clip);
        }
        if (b.action == Action::Shapes) drawCheckBox(c, b.r, s.shapes, clip);  // filled while shapes are recognized
    }
}

}  // namespace scrible
