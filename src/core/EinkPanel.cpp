#include "EinkPanel.h"

namespace scrible {

EinkPanel::EinkPanel(int width, int height) : submitted_(width, height), shown_(width, height) {}

void EinkPanel::refresh(const Canvas& c, const Damage& d) {
    const IRect r = d.rect.intersected(c.rect()).intersected(shown_.rect());
    const bool du = d.mode == Refresh::Ink || d.mode == Refresh::UI || d.mode == Refresh::Preview;
    const bool flashing = d.mode == Refresh::Full;
    for (int y = r.y0; y < r.y1; ++y) {
        const uint8_t* src = c.row(y);
        uint8_t* sub = submitted_.row(y);
        uint8_t* shown = shown_.row(y);
        for (int x = r.x0; x < r.x1; ++x) {
            const uint8_t v = src[x];
            if (flashing || v != sub[x]) {
                if (!du || v == 0 || v == 255) shown[x] = v;
            }
            sub[x] = v;
        }
    }
}

}  // namespace scrible
