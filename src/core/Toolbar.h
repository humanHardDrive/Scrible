#pragma once

#include <string>
#include <utility>
#include <vector>

#include "Canvas.h"

namespace scrible {

enum class Action {
    None,
    // editor: the tools panel (Tools opens it; sizes apply to the current tool), and
    // Delete/Duplicate/Cancel act on a lasso selection
    Tools, Pen, Shapes, Lasso, Highlighter, Eraser, Pan, ZoomOut, ZoomIn, ZoomFit,
    SizeFine, SizeMedium, SizeThick, ErasePartial, EraseWhole,
    Undo, Redo, Save,
    // editor: the page menu (PageStyle opens it): the current page's paper, or deleting it
    PageStyle, PaperBlank, PaperRuled, PaperGrid, PaperNarrow, PaperMedium, PaperWide, PaperMargin, DeletePage,
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
    Action action;      // Action::None marks the text slot (page counter, status)
    std::string label;  // for the text slot: the widest text it has to fit
};
using ToolbarGroups = std::vector<std::vector<ToolbarButton>>;

struct ToolbarState {
    std::vector<Action> active;                          // drawn inverted
    std::vector<Action> disabled;                        // drawn checkered
    std::vector<std::pair<Action, std::string>> labels;  // label overrides
    std::string text;                                    // contents of the text slot
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

private:
    struct Button {
        Action action;
        std::string label;
        IRect r;
    };
    IRect rect_;
    int scale_ = 4;
    std::vector<Button> buttons_;
    IRect textSlot_;
};

}  // namespace scrible
