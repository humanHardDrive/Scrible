#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Editor.h"
#include "Rotation.h"

namespace scrible {

struct DisplayOptions {
    bool penMode = true;   // MTK "pen mode": low-latency DU updates for inking (Scribe 1st/2nd gen)
    bool fastMode = true;  // stop the driver from silently promoting partial updates to REAGL
    bool a2 = true;        // zoom/pan previews after the first use A2 (~120 ms) rather than DU (~260 ms)
};

// Touch panel orientation quirks FBInk knows about for this device.
struct TouchQuirks {
    bool swapAxes = false, mirrorX = false, mirrorY = false;
};

// The e-ink framebuffer, driven through FBInk. Maps the editor's refresh kinds to
// waveforms: Ink/UI -> DU, Content -> GC16 (partial), Full -> flashing GC16.
class FbinkDisplay {
public:
    FbinkDisplay();
    ~FbinkDisplay();

    bool open(const DisplayOptions& opts);
    void close();

    int width() const;
    int height() const;
    int dpi() const;
    TouchQuirks touchQuirks() const;
    std::string describe() const;
    std::string aboutLine() const;  // "Kindle Scribe (Barolo), FBInk v1.25.0"

    // The canvas may be turned (see ScreenRotation): its damage is turned onto the panel.
    void present(const Canvas& c, const std::vector<Damage>& damage, const ScreenRotation& rotation = {});

private:
    struct State;
    std::unique_ptr<State> s_;  // keeps fbink.h (and its `restrict` macro) out of this header
};

}  // namespace scrible
