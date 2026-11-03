#pragma once

#include <array>
#include <vector>

#include "Canvas.h"
#include "Toolbar.h"

namespace scrible {

enum class ToolSize { Fine, Medium, Thick };

struct ToolPanelState {
    Action tool = Action::Pen;  // Pen, Highlighter, Eraser or Lasso
    bool shapes = false;
    bool eraseWhole = false;
    // The current tool's size, and its preset widths in screen pixels (shown as samples).
    ToolSize size = ToolSize::Medium;
    std::array<double, 3> widthsPx{};
};

// The drawing tools, kept out of the way along the left edge of the page: a small
// tab with an arrow, which opens a column of PEN, SHAPE, LASSO, MARK and ERASE. Picking the pen, highlighter or eraser shows its options beside the
// column (sizes; for the eraser also partial or whole strokes). Strictly black and
// white, like the toolbar, so it can be shown with the fast DU waveform.
class ToolPanel {
public:
    explicit ToolPanel(const IRect& area);  // the page area; the tab sits at its top-left

    bool open() const { return open_; }
    Action options() const { return options_; }  // the tool whose options are showing, or None
    void setOpen(bool open);                     // closing also puts the options away
    void showOptions(Action tool);               // Pen, Highlighter or Eraser; opens the column too

    // What it covers right now: the tab, or the column and the options beside it.
    std::vector<IRect> rects() const;
    bool covers(int x, int y) const;
    Action hit(int x, int y) const;  // None between buttons
    IRect buttonRect(Action a) const;  // Tools is the tab; empty when that button isn't showing
    const IRect& tabRect() const { return tab_; }

    void draw(Canvas& c, const ToolPanelState& s, const IRect& clip) const;

private:
    struct Button {
        Action action;
        IRect r;
    };
    void layout();

    IRect area_, tab_, column_, optionsRect_;
    bool open_ = false;
    Action options_ = Action::None;
    std::vector<Button> buttons_;  // the column's, then the options'
};

}  // namespace scrible
