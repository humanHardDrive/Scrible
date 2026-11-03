#pragma once

#include <optional>
#include <vector>

#include "Canvas.h"
#include "Paper.h"
#include "Toolbar.h"

namespace scrible {

struct PaperMenuState {
    std::optional<Paper> paper;  // nothing: a background it can't show, such as dotted
    bool paperChoices = true;    // false for PDF and image pages, which keep theirs
    bool canDelete = true;       // not the notebook's only page
    bool deleteArmed = false;    // DELETE PAGE was tapped once
};

// The menu that drops down below the toolbar's PAGE button, for the current page:
// BLANK / RULED / GRID, then the spacing (ruled and grid), then the margin line
// (ruled), and the blank top and bottom (ruled), then DELETE PAGE. Black and white only, like the toolbar.
class PaperMenu {
public:
    // `anchor` is the PAGE button; the menu stays within `area`, the page area.
    PaperMenu(const IRect& anchor, const IRect& area);

    bool open() const { return open_; }
    void show(const PaperMenuState& s);  // opens or updates it
    void close();

    const IRect& rect() const { return rect_; }  // empty when closed
    bool covers(int x, int y) const { return rect_.contains(x, y); }
    Action hit(int x, int y) const;  // None between buttons
    IRect buttonRect(Action a) const;

    void draw(Canvas& c, const IRect& clip) const;

private:
    struct Button {
        Action action;
        IRect r;
    };

    IRect anchor_, area_, rect_;
    bool open_ = false;
    PaperMenuState state_;
    std::vector<Button> buttons_;
};

}  // namespace scrible
