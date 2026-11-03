#pragma once

// Buttons for the editor's panels (ToolPanel, PaperMenu): large, high-contrast and
// strictly black and white, so they can be shown with the fast DU waveform.

#include <string>

#include "Canvas.h"

namespace scrible::ui {

constexpr int kScale = 3;  // 24 px glyphs, like the selection's buttons
constexpr int kButtonH = 88, kPad = 10, kGap = 8, kBorder = 3, kTextPad = 20;
constexpr int kBox = 30;  // check boxes

inline int textWidth(const std::string& s) { return Canvas::textWidth(s, kScale); }

// Black border, white inside; all black when it's the current choice.
void drawBox(Canvas& c, const IRect& r, bool on, const IRect& clip);
// Text centered vertically in `r`, from `x`, or centered across it when x < 0.
void drawLabel(Canvas& c, const IRect& r, int x, const std::string& text, uint8_t value, const IRect& clip);
// A check box at the right end of button `r`, filled while checked.
void drawCheckBox(Canvas& c, const IRect& r, bool checked, const IRect& clip);

}  // namespace scrible::ui
