#pragma once

#include <string>
#include <vector>

#include "Canvas.h"

namespace xs {

enum class Action { None, Pen, Highlighter, Eraser, SizeFine, SizeMedium, SizeThick, Undo, Redo, PrevPage, NextPage, Save, Exit };

enum class ToolKind { Pen, Highlighter, Eraser };
enum class ToolSize { Fine, Medium, Thick };

struct ToolbarState {
    ToolKind tool = ToolKind::Pen;
    ToolSize size = ToolSize::Medium;
    bool canUndo = false;
    bool canRedo = false;
    bool modified = false;
    size_t page = 0;
    size_t pageCount = 1;
    std::string status;  // shown instead of the page counter while non-empty (keep it <= 7 chars)
};

// A single row of large, high-contrast buttons along the top of the screen.
// Strictly black and white (no grays), so it can be refreshed with the fast DU waveform.
class Toolbar {
public:
    Toolbar(int width, int height);

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
    IRect pageLabel_;
};

}  // namespace xs
