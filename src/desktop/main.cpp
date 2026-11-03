// Desktop front end (SDL3) for developing without a Kindle. It runs the same
// App at the Scribe's resolution in a scaled-down window, and by default
// imitates the e-ink panel (see EinkPanel): hard-edged ink, and refresh
// artifacts such as DU leaving gray pixels unchanged show up as on the device.
//
//   scrible-desktop [FOLDER | notebook.xopp] [--size 1860x2480] [--no-eink-sim]
//
// A folder (default ~/Documents/Scrible) opens the notebook list; a
// .xopp/.xoj file opens just that notebook.
// Mouse: left = pen, right = eraser, middle = highlighter (like a pen's side
// button). A drawing tablet gets real pressure.
// Keys: Cmd/Ctrl+Z undo, Shift+Cmd/Ctrl+Z or Ctrl+Y redo, Cmd/Ctrl+S save,
//       P/H/E/L pen/highlighter/eraser/lasso, 1/2/3 sizes, Delete removes the selection,
//       Left/Right pages,
//       N new notebook (in the list), Esc back/quit.

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "App.h"
#include "EinkPanel.h"

namespace {

struct Args {
    std::string path;
    int width = 1860, height = 2480;  // Kindle Scribe (1st/2nd gen)
    bool einkSim = true;
};

bool parseArgs(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--size") && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%dx%d", &a.width, &a.height) != 2) return false;
        } else if (!std::strcmp(argv[i], "--no-eink-sim")) {
            a.einkSim = false;
        } else if (argv[i][0] == '-') {
            return false;
        } else {
            a.path = argv[i];
        }
    }
    return a.width > 0 && a.height > 0;
}

class Window {
public:
    bool open(int w, int h, bool einkSim) {
        if (einkSim) panel_ = std::make_unique<scrible::EinkPanel>(w, h);
        SDL_SetHint(SDL_HINT_PEN_MOUSE_EVENTS, "0");  // tablet input arrives as pen events only
        SDL_SetHint(SDL_HINT_PEN_TOUCH_EVENTS, "0");
        if (!SDL_Init(SDL_INIT_VIDEO)) return fail("SDL_Init");

        // Fit the portrait page on screen.
        float scale = 0.35f;
        if (const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay()))
            scale = std::min(0.9f * m->h / h, 0.9f * m->w / w);
        if (!SDL_CreateWindowAndRenderer("Scrible", static_cast<int>(w * scale), static_cast<int>(h * scale),
                                         SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY, &window_, &renderer_))
            return fail("SDL_CreateWindowAndRenderer");
        SDL_SetRenderLogicalPresentation(renderer_, w, h, SDL_LOGICAL_PRESENTATION_LETTERBOX);
        texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!texture_) return fail("SDL_CreateTexture");
        SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_LINEAR);
        return true;
    }

    ~Window() {
        if (texture_) SDL_DestroyTexture(texture_);
        if (renderer_) SDL_DestroyRenderer(renderer_);
        if (window_) SDL_DestroyWindow(window_);
        SDL_Quit();
    }

    void present(const scrible::Canvas& c, const std::vector<scrible::Damage>& damage) {
        for (const scrible::Damage& d : damage) {
            const scrible::IRect r = d.rect.intersected(c.rect());
            if (r.empty()) continue;
            if (panel_) panel_->refresh(c, d);
            const scrible::Canvas& shown = panel_ ? panel_->shown() : c;
            const SDL_Rect sr{r.x0, r.y0, r.width(), r.height()};
            void* pixels = nullptr;
            int pitch = 0;
            if (!SDL_LockTexture(texture_, &sr, &pixels, &pitch)) continue;
            for (int y = 0; y < r.height(); ++y) {
                const uint8_t* src = shown.row(r.y0 + y) + r.x0;
                auto* dst = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pixels) + static_cast<size_t>(y) * pitch);
                for (int x = 0; x < r.width(); ++x) {
                    const uint32_t g = src[x];
                    dst[x] = 0xff000000u | g << 16 | g << 8 | g;
                }
            }
            SDL_UnlockTexture(texture_);
        }
        SDL_SetRenderDrawColor(renderer_, 40, 40, 40, 255);
        SDL_RenderClear(renderer_);
        SDL_RenderTexture(renderer_, texture_, nullptr, nullptr);
        SDL_RenderPresent(renderer_);
    }

    // Window coordinates -> canvas pixels.
    void toCanvas(float wx, float wy, double& x, double& y) const {
        float cx = wx, cy = wy;
        SDL_RenderCoordinatesFromWindow(renderer_, wx, wy, &cx, &cy);
        x = cx;
        y = cy;
    }

private:
    bool fail(const char* what) {
        std::fprintf(stderr, "%s failed: %s\n", what, SDL_GetError());
        return false;
    }

    std::unique_ptr<scrible::EinkPanel> panel_;  // null with --no-eink-sim
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
};

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parseArgs(argc, argv, args)) {
        std::fprintf(stderr, "usage: %s [FOLDER | notebook.xopp] [--size WxH] [--no-eink-sim]\n", argv[0]);
        return 2;
    }
    if (args.path.empty()) {
        const char* home = std::getenv("HOME");
        args.path = std::string(home ? home : ".") + "/Documents/Scrible";
    }

    scrible::AppOptions aopts;
    aopts.editor.render.antialias = !args.einkSim;  // like the Kindle: hard-edged ink
    if (scrible::isNotebookFile(args.path)) aopts.file = args.path;
    else aopts.notebooksDir = args.path;
    const int sdl = SDL_GetVersion();
    aopts.platformInfo = {"Desktop build, SDL " + std::to_string(SDL_VERSIONNUM_MAJOR(sdl)) + "." +
                          std::to_string(SDL_VERSIONNUM_MINOR(sdl)) + "." + std::to_string(SDL_VERSIONNUM_MICRO(sdl))};
    scrible::App app(args.width, args.height, aopts);
    if (!app.startupError().empty()) {
        std::fprintf(stderr, "%s\n", app.startupError().c_str());
        return 1;
    }

    Window win;
    if (!win.open(args.width, args.height, args.einkSim)) return 1;
    win.present(app.canvas(), app.takeDamage());

    bool mouseDrawing = false;
    float penPressure = 0.5f;
    bool closing = false;
    while (!closing && !app.quitRequested()) {
        const int wait = app.msUntilIdleWork(SDL_GetTicks());
        SDL_Event ev;
        bool got = wait < 0 ? SDL_WaitEvent(&ev) : SDL_WaitEventTimeout(&ev, wait);
        while (got) {
            const uint64_t now = SDL_GetTicks();
            double x = 0, y = 0;
            switch (ev.type) {
                case SDL_EVENT_QUIT:
                    // Window closed: save and leave from wherever we are.
                    if (app.saveNow(now)) closing = true;
                    break;

                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    if (ev.button.button != SDL_BUTTON_LEFT && ev.button.button != SDL_BUTTON_RIGHT &&
                        ev.button.button != SDL_BUTTON_MIDDLE)
                        break;
                    win.toCanvas(ev.button.x, ev.button.y, x, y);
                    app.penDown({x, y, 0.5, ev.button.button == SDL_BUTTON_RIGHT, ev.button.button == SDL_BUTTON_MIDDLE},
                                now);
                    mouseDrawing = true;
                    break;
                case SDL_EVENT_MOUSE_MOTION:
                    if (!mouseDrawing) break;
                    win.toCanvas(ev.motion.x, ev.motion.y, x, y);
                    app.penMove({x, y, 0.5, false}, now);
                    break;
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    if (!mouseDrawing) break;
                    app.penUp(now);
                    mouseDrawing = false;
                    break;

                case SDL_EVENT_PEN_AXIS:
                    if (ev.paxis.axis == SDL_PEN_AXIS_PRESSURE) penPressure = ev.paxis.value;
                    break;
                case SDL_EVENT_PEN_DOWN:
                    win.toCanvas(ev.ptouch.x, ev.ptouch.y, x, y);
                    app.penDown({x, y, penPressure, ev.ptouch.eraser, (ev.ptouch.pen_state & SDL_PEN_INPUT_BUTTON_1) != 0},
                                now);
                    break;
                case SDL_EVENT_PEN_MOTION:
                    if (!(ev.pmotion.pen_state & SDL_PEN_INPUT_DOWN)) break;
                    win.toCanvas(ev.pmotion.x, ev.pmotion.y, x, y);
                    app.penMove({x, y, penPressure, (ev.pmotion.pen_state & SDL_PEN_INPUT_ERASER_TIP) != 0}, now);
                    break;
                case SDL_EVENT_PEN_UP: app.penUp(now); break;

                case SDL_EVENT_KEY_DOWN: {
                    const bool cmd = ev.key.mod & (SDL_KMOD_GUI | SDL_KMOD_CTRL);
                    const bool shift = ev.key.mod & SDL_KMOD_SHIFT;
                    scrible::Action a = scrible::Action::None;
                    switch (ev.key.key) {
                        case SDLK_Z: if (cmd) a = shift ? scrible::Action::Redo : scrible::Action::Undo; break;
                        case SDLK_Y: if (cmd) a = scrible::Action::Redo; break;
                        case SDLK_S: if (cmd) a = scrible::Action::Save; break;
                        case SDLK_P: a = scrible::Action::Pen; break;
                        case SDLK_H: a = scrible::Action::Highlighter; break;
                        case SDLK_E: a = scrible::Action::Eraser; break;
                        case SDLK_L: a = scrible::Action::Lasso; break;
                        case SDLK_R: a = scrible::Action::Shapes; break;
                        case SDLK_DELETE:
                        case SDLK_BACKSPACE: a = scrible::Action::Delete; break;
                        case SDLK_1: a = scrible::Action::SizeFine; break;
                        case SDLK_2: a = scrible::Action::SizeMedium; break;
                        case SDLK_3: a = scrible::Action::SizeThick; break;
                        case SDLK_LEFT: a = scrible::Action::PrevPage; break;
                        case SDLK_RIGHT: a = scrible::Action::NextPage; break;
                        case SDLK_N: if (!cmd) a = scrible::Action::NewNotebook; break;
                        case SDLK_RETURN: a = scrible::Action::Confirm; break;
                        case SDLK_ESCAPE: a = scrible::Action::Exit; break;
                        default: break;
                    }
                    app.perform(a, now);
                    break;
                }
                case SDL_EVENT_WINDOW_EXPOSED: app.redrawAll(scrible::Refresh::Content); break;
                default: break;
            }
            got = SDL_PollEvent(&ev);
        }
        app.onIdle(SDL_GetTicks());
        win.present(app.canvas(), app.takeDamage());
    }
    return 0;
}
