#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Editor.h"

namespace scrible {

struct DisplayOptions {
    bool penMode = true;   // MTK "pen mode": low-latency DU updates for inking (Scribe 1st/2nd gen)
    bool fastMode = true;  // stop the driver from silently promoting partial updates to REAGL
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

    void present(const Canvas& c, const std::vector<Damage>& damage);

private:
    struct State;
    std::unique_ptr<State> s_;  // keeps fbink.h (and its `restrict` macro) out of this header
};

}  // namespace scrible
