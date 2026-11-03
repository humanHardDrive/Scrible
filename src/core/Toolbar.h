#pragma once

#include <string>
#include <utility>
#include <vector>

#include "Canvas.h"

namespace scrible {

enum class Action {
    None,
    // editor: the tools panel (Tools opens it; sizes apply to the current tool), and
    // the selection's menu: EditMenu (Delete, Duplicate) and MathMenu (the Math ones,
    // on the numbers written in it); Cancel lets go of it
    Tools, Pen, Shapes, Lasso, Highlighter, Eraser, Pan, ZoomOut, ZoomIn, ZoomFit,
    SizeFine, SizeMedium, SizeThick, ErasePartial, EraseWhole,
    EditMenu, MathMenu, MathSum, MathAverage, MathMin, MathMax, MathCount,
    Undo, Redo, Save,
    // editor: the notebook's name, tapped to see it whole (and tapped again to go back)
    Title,
    // editor: the page menu (PageStyle opens it): the current page's paper, or deleting it
    PageStyle, PaperBlank, PaperRuled, PaperGrid, PaperNarrow, PaperMedium, PaperWide, PaperMargin,
    PaperEnds, DeletePage,
    // both screens: pages of the notebook, or of the notebook list
    PrevPage, NextPage,
    // editor: close the notebook; library: leave the app
    Exit,
    // editor: save and leave the app directly
    Quit,
    // library
    NewNotebook, Sort, Rename, Duplicate, Delete, About,
    // dialogs and the text prompt
    Confirm, Cancel,
};

struct ToolbarButton {
    Action action;      // Action::None marks a text slot: the page counter and status, or the title
    std::string label;  // for the text slot: the widest text it has to fit; empty for the title,
                        // which takes the room left over
};
using ToolbarGroups = std::vector<std::vector<ToolbarButton>>;

struct ToolbarState {
    std::vector<Action> active;                          // drawn inverted
    std::vector<Action> disabled;                        // drawn checkered
    std::vector<std::pair<Action, std::string>> labels;  // label overrides
    std::string text;                                    // contents of the text slot
    std::string title;                                   // shortened to fit, if need be
    bool titleOnly = false;                              // the whole bar shows the title, whole
};

// A single row of large, high-contrast buttons along the top of the screen, laid
// out in groups spread across the width. Strictly black and white (no grays), so
// it can be refreshed with the fast DU waveform.
class Toolbar {
public:
    Toolbar(int width, int height, ToolbarGroups groups);

    const IRect& rect() const { return rect_; }
    void draw(Canvas& c, const ToolbarState& s) const;
    Action hit(int x, int y) const;
    IRect buttonRect(Action a) const;
    const IRect& titleRect() const { return titleSlot_; }

private:
    struct Button {
        Action action;
        std::string label;
        IRect r;
    };
    IRect rect_;
    int scale_ = 4;
    std::vector<Button> buttons_;
    IRect textSlot_, titleSlot_;
};

}  // namespace scrible
