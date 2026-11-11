#pragma once

#include <cstdint>

#include "Canvas.h"

namespace scrible {

// The app's screen, turned: the device held a quarter turn clockwise (`turns` 1),
// upside down (2), or a quarter turn the other way (3). The app draws on a canvas
// the turned way up ("logical"), and the front end turns its pixels onto the
// panel ("physical") and the pen and fingers back. Turning in software leaves the
// Kindle's own screen setup alone, so there's nothing to put back on leaving.
struct ScreenRotation {
    int turns = 0;              // 0..3
    int width = 0, height = 0;  // the physical screen

    int logicalWidth() const { return turns % 2 ? height : width; }
    int logicalHeight() const { return turns % 2 ? width : height; }

    // A point on the panel (pen and touch), as the app sees it.
    void toLogical(double px, double py, double& lx, double& ly) const;
    // The panel's pixels that show a rectangle of the canvas.
    IRect toPhysical(const IRect& logical) const;
    // Copies what the panel's rectangle `physical` shows of canvas `c` into `out`,
    // rows packed (physical.width() * physical.height() bytes).
    void copyOut(const Canvas& c, const IRect& physical, uint8_t* out) const;
};

}  // namespace scrible
