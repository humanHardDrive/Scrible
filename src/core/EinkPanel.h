#pragma once

// A model of how an e-ink controller turns refresh requests into what the panel
// actually shows, as far as it affects correctness:
//  - partial updates only drive pixels whose value changed since they were last
//    submitted (the controller compares against its own copy, not the panel);
//  - DU only drives pixels to pure black or white; any other target leaves the
//    pixel showing whatever it showed before (FBInk's documentation of WFM_DU).
// The desktop build uses it to show e-ink artifacts, and tests use it to catch them.

#include "Canvas.h"
#include "Editor.h"

namespace scrible {

class EinkPanel {
public:
    EinkPanel(int width, int height);

    void refresh(const Canvas& c, const Damage& d);
    const Canvas& shown() const { return shown_; }

private:
    Canvas submitted_;  // the controller's idea of the screen
    Canvas shown_;      // what the panel shows
};

}  // namespace scrible
