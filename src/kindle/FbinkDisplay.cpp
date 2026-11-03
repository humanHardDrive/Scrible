#include "FbinkDisplay.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "fbink.h"

namespace scrible {

struct FbinkDisplay::State {
    int fd = -1;
    FBInkConfig cfg{};
    FBInkState fb{};
    DisplayOptions opts;
    bool penModeOn = false;
    bool penModeWorks = true;
    bool fastModeSet = false;
    uint32_t previewMarker = 0;  // the last zoom/pan preview, until something waited for it
    bool inPreviews = false;     // the last update was a preview: the screen is all black and white
    int previewWaits = 0;        // ...and how long waiting for them took, for the log
    double previewWaitMs = 0;
    std::vector<uint8_t> buf;
};

FbinkDisplay::FbinkDisplay() : s_(new State) {}
FbinkDisplay::~FbinkDisplay() { close(); }

bool FbinkDisplay::open(const DisplayOptions& opts) {
    s_->opts = opts;
    s_->fd = fbink_open();
    if (s_->fd < 0) {
        std::fprintf(stderr, "scrible: can't open the framebuffer\n");
        return false;
    }
    if (fbink_init(s_->fd, &s_->cfg) < 0) {
        std::fprintf(stderr, "scrible: FBInk initialization failed\n");
        close();
        return false;
    }
    fbink_get_state(&s_->cfg, &s_->fb);

    // We only send pixels with explicit refreshes, never let FBInk refresh on its own.
    s_->cfg.no_refresh = true;
    s_->cfg.ignore_alpha = true;

    if (s_->fb.is_mtk && opts.fastMode) s_->fastModeSet = fbink_mtk_toggle_auto_reagl(s_->fd, false) >= 0;
    s_->penModeWorks = s_->fb.is_mtk && opts.penMode;
    return true;
}

void FbinkDisplay::close() {
    if (!s_ || s_->fd < 0) return;
    if (s_->penModeOn) fbink_mtk_toggle_pen_mode(s_->fd, false);
    if (s_->fastModeSet) fbink_mtk_toggle_auto_reagl(s_->fd, true);
    fbink_close(s_->fd);
    s_->fd = -1;
    s_->penModeOn = s_->fastModeSet = false;
}

int FbinkDisplay::width() const { return static_cast<int>(s_->fb.screen_width); }
int FbinkDisplay::height() const { return static_cast<int>(s_->fb.screen_height); }
int FbinkDisplay::dpi() const { return s_->fb.screen_dpi ? s_->fb.screen_dpi : 300; }

TouchQuirks FbinkDisplay::touchQuirks() const {
    return {s_->fb.touch_swap_axes, s_->fb.touch_mirror_x, s_->fb.touch_mirror_y};
}

std::string FbinkDisplay::describe() const {
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s (%s, %s), %ux%u @ %u dpi, %u bpp, rotation %u, MTK %s, FBInk %s",
                  s_->fb.device_name, s_->fb.device_codename, s_->fb.device_platform, s_->fb.screen_width,
                  s_->fb.screen_height, s_->fb.screen_dpi, s_->fb.bpp, s_->fb.current_rota,
                  s_->fb.is_mtk ? "yes" : "no", fbink_version());
    return buf;
}

std::string FbinkDisplay::aboutLine() const {
    return std::string("Kindle ") + s_->fb.device_name + " (" + s_->fb.device_codename + "), FBInk " + fbink_version();
}

void FbinkDisplay::present(const Canvas& c, const std::vector<Damage>& damage) {
    if (s_->fd < 0) return;
    for (const Damage& d : damage) {
        const IRect r = d.rect.intersected(c.rect()).intersected({0, 0, width(), height()});
        if (r.empty()) continue;

        // A whole-page preview takes the panel longer than the gesture takes to make
        // the next one. So each waits until the one before it is on screen (whatever
        // moved meanwhile goes into this one), and anything but ink waits for the last
        // one, which mustn't land on top of the proper redraw that follows.
        if (s_->previewMarker && d.mode != Refresh::Ink) {
            const auto start = std::chrono::steady_clock::now();
            fbink_wait_for_complete(s_->fd, s_->previewMarker);
            s_->previewMarker = 0;
            ++s_->previewWaits;
            s_->previewWaitMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (d.mode != Refresh::Preview) {
                std::fprintf(stderr, "scrible: waited for %d previews (%s) to reach the screen, %.0f ms each on average\n",
                             s_->previewWaits, s_->opts.a2 ? "A2 after the first" : "DU",
                             s_->previewWaitMs / s_->previewWaits);
                s_->previewWaits = 0;
                s_->previewWaitMs = 0;
            }
        }

        // Previews go without pen mode: it's meant for small strokes.
        const bool fast = d.mode == Refresh::Ink || d.mode == Refresh::UI;
        // Pen mode only tolerates DU updates: switch it off before anything else,
        // and back on for the next stroke.
        if (fast && !s_->penModeOn && s_->penModeWorks) {
            if (fbink_mtk_toggle_pen_mode(s_->fd, true) >= 0) {
                s_->penModeOn = true;
            } else {
                std::fprintf(stderr, "scrible: pen mode unavailable on this device, using plain DU\n");
                s_->penModeWorks = false;
            }
        } else if (!fast && s_->penModeOn) {
            fbink_mtk_toggle_pen_mode(s_->fd, false);
            s_->penModeOn = false;
        }

        // Copy the region out (FBInk wants tightly packed rows), then refresh it.
        const int w = r.width(), h = r.height();
        s_->buf.resize(static_cast<size_t>(w) * h);
        for (int y = 0; y < h; ++y) std::copy_n(c.row(r.y0 + y) + r.x0, w, &s_->buf[static_cast<size_t>(y) * w]);
        if (fbink_print_raw_data(s_->fd, s_->buf.data(), w, h, s_->buf.size(), static_cast<short>(r.x0),
                                 static_cast<short>(r.y0), &s_->cfg) < 0) {
            std::fprintf(stderr, "scrible: fbink_print_raw_data failed\n");
            continue;
        }

        FBInkConfig rc = s_->cfg;
        rc.no_refresh = false;
        switch (d.mode) {
            case Refresh::Ink:
            case Refresh::UI: rc.wfm_mode = WFM_DU; break;
            // A2 only goes from black and white to black and white, which the screen is
            // once the first preview (DU) is on it.
            case Refresh::Preview: rc.wfm_mode = s_->inPreviews && s_->opts.a2 ? WFM_A2 : WFM_DU; break;
            // GC16 rather than GL16: GL16 assumes pixels start out white, which
            // isn't true when ink disappears (erase, undo).
            case Refresh::Content: rc.wfm_mode = WFM_GC16; break;
            case Refresh::Full:
                rc.wfm_mode = WFM_GC16;
                rc.is_flashing = true;
                break;
        }
        fbink_refresh(s_->fd, static_cast<uint32_t>(r.y0), static_cast<uint32_t>(r.x0), static_cast<uint32_t>(w),
                      static_cast<uint32_t>(h), &rc);
        if (d.mode == Refresh::Preview) s_->previewMarker = fbink_get_last_marker();
        s_->inPreviews = d.mode == Refresh::Preview;
    }
}

}  // namespace scrible
