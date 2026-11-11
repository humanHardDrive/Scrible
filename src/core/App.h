#pragma once

// Top level of the app: the notebook list and the editor, and switching between
// them. Front ends feed it input and present its damage, nothing else.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Editor.h"
#include "Library.h"
#include "Rotation.h"
#include "Settings.h"

namespace scrible {

struct AppOptions {
    EditorOptions editor;
    // With a notebooks folder the app starts on the list of notebooks in it, and
    // closing a notebook returns there. Otherwise `file` is opened directly (and
    // created if missing), and closing it quits.
    std::string notebooksDir;
    std::string file;
    std::vector<std::string> platformInfo;  // for the About dialog: the device, its libraries
};

class App {
public:
    // The screen as it is, unturned; the app may draw on it turned (rotation()).
    App(int width, int height, AppOptions opts);

    // Check first: non-empty if the app couldn't start (e.g. the notebook given
    // on the command line can't be read).
    const std::string& startupError() const { return startupError_; }

    Canvas& canvas() { return editor_ ? editor_->canvas() : library_->canvas(); }
    Editor* editor() { return editor_.get(); }
    Library* library() { return library_.get(); }
    bool quitRequested() const { return quit_; }
    // How the screen is turned, which ROTATE in a notebook's tools changes (and the
    // settings remember). canvas() is that way up, and so are the coordinates of the
    // input: the front end turns its pixels and input with this.
    const ScreenRotation& rotation() const { return rotation_; }

    void redrawAll(Refresh mode = Refresh::Full);

    // Input. `now` is a monotonic clock in milliseconds.
    void penDown(const PenSample& s, uint64_t now);
    void penMove(const PenSample& s, uint64_t now);
    void penUp(uint64_t now);
    void touchDown(int x, int y, uint64_t now);  // a tap
    // Fingers, told apart by `id`: on a notebook's page, two pinch to zoom (see Editor).
    void touchDown(int id, int x, int y, uint64_t now);
    void touchMove(int id, int x, int y, uint64_t now);
    void touchUp(int id, uint64_t now);
    void setPenNear(bool near);  // the pen hovers over the screen, or touches it
    // Mouse wheel and trackpad.
    void zoomBy(double factor, double x, double y, uint64_t now);
    void panBy(double dx, double dy, uint64_t now);
    void perform(Action a, uint64_t now);  // keyboard shortcuts
    // Typing on the page with the TEXT tool, from a real keyboard (see Editor::type).
    bool typing() const { return editor_ && editor_->typing(); }
    void type(const std::string& s, uint64_t now);

    void onIdle(uint64_t now);
    int msUntilIdleWork(uint64_t now) const;  // -1 when nothing is scheduled
    std::vector<Damage> takeDamage();

    // Saves the open notebook, if it has changes (before the device sleeps, or on
    // shutdown). False if saving failed.
    bool saveNow(uint64_t now);

private:
    void handle(const Library::Intent& intent, uint64_t now);
    void afterEditorInput(uint64_t now);
    bool openNotebook(const std::string& path, std::string& error);
    void showLibrary(const std::string& message = {});
    void makeLibrary();
    void rotate(uint64_t now);

    ScreenRotation rotation_;
    int width_, height_;  // the canvas, turned
    AppOptions opts_;
    std::unique_ptr<Settings> settings_;  // with a notebooks folder only
    std::unique_ptr<Library> library_;
    std::unique_ptr<Editor> editor_;
    std::string startupError_;
    bool quit_ = false;
    int fingers_ = 0;  // down on the screen
    bool penNear_ = false;
    // Opening a notebook is deferred until the tapped row's highlight is on screen:
    // loading a large notebook takes a moment.
    std::function<void()> deferred_;
    int deferredTicks_ = 0;
};

}  // namespace scrible
