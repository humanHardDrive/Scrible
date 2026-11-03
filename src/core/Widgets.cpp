#include "Widgets.h"

namespace scrible::ui {

void drawBox(Canvas& c, const IRect& r, bool on, const IRect& clip) {
    c.fill(r.intersected(clip), 0);
    if (!on) c.fill(r.grown(-kBorder).intersected(clip), 255);
}

void drawLabel(Canvas& c, const IRect& r, int x, const std::string& text, uint8_t value, const IRect& clip) {
    if (x < 0) x = r.x0 + (r.width() - textWidth(text)) / 2;
    c.drawText(x, r.y0 + (r.height() - 8 * kScale) / 2, text, kScale, value, r.intersected(clip));
}

void drawCheckBox(Canvas& c, const IRect& r, bool checked, const IRect& clip) {
    const int cy = (r.y0 + r.y1) / 2;
    const IRect box{r.x1 - kTextPad - kBox, cy - kBox / 2, r.x1 - kTextPad, cy + kBox / 2};
    drawBox(c, box, false, clip);
    if (checked) c.fill(box.grown(-7).intersected(clip), 0);
}

}  // namespace scrible::ui
