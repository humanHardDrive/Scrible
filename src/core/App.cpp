#include "App.h"

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "XoppFile.h"

namespace scrible {
namespace {

// Settings keys for the size of each tool, indexed by ToolKind, and their values.
constexpr const char* kSizeKeys[] = {"pen-size", "highlighter-size", "eraser-size"};
constexpr const char* kSizeNames[] = {"fine", "medium", "thick"};

}  // namespace

App::App(int width, int height, AppOptions opts) : width_(width), height_(height), opts_(std::move(opts)) {
    std::string error;
    if (!opts_.notebooksDir.empty()) {
        if (!makeDirs(opts_.notebooksDir, &error)) {
            startupError_ = "Can't create " + error;
            quit_ = true;
            return;
        }
        settings_ = std::make_unique<Settings>(opts_.notebooksDir + "/.scrible");
        library_ = std::make_unique<Library>(width_, height_, opts_.notebooksDir, settings_.get(),
                                             opts_.editor.toolbarHeight);
        library_->setPlatformInfo(opts_.platformInfo);
    } else if (!openNotebook(opts_.file, error)) {
        startupError_ = error;
        quit_ = true;
    }
}

bool App::openNotebook(const std::string& path, std::string& error) {
    Document doc;
    if (access(path.c_str(), F_OK) == 0) {  // otherwise it's a new notebook, saved on first change
        std::vector<std::string> warnings;
        if (!loadXopp(path, doc, error, &warnings)) return false;
        for (const auto& w : warnings) std::fprintf(stderr, "scrible: %s: %s\n", path.c_str(), w.c_str());
    }
    EditorOptions eo = opts_.editor;
    eo.fromList = library_ != nullptr;
    if (settings_) {
        eo.digitExamplesPath = opts_.notebooksDir + "/.scrible-digits";
        eo.eraseWholeStrokes = settings_->get("eraser") == "stroke";
        eo.recognizeShapes = settings_->get("shapes") == "on";
        for (size_t k = 0; k < eo.sizes.size(); ++k)
            for (int i = 0; i < 3; ++i)
                if (settings_->get(kSizeKeys[k]) == kSizeNames[i]) eo.sizes[k] = static_cast<ToolSize>(i);
    }
    editor_ = std::make_unique<Editor>(width_, height_, std::move(doc), path, eo);
    editor_->setPenNear(penNear_);
    return true;
}

void App::showLibrary(const std::string& message) {
    library_->rescan();
    library_->setMessage(message);
    library_->redrawAll(Refresh::Full);
}

void App::handle(const Library::Intent& intent, uint64_t now) {
    switch (intent.kind) {
        case Library::Intent::Kind::None: break;
        case Library::Intent::Kind::Exit: quit_ = true; break;
        case Library::Intent::Kind::New: {
            std::string error;
            if (!openNotebook(intent.path, error)) showLibrary(error);
            else editor_->save(now, true);  // it was given a name, so it exists from now on
            break;
        }
        case Library::Intent::Kind::Open:
            deferred_ = [this, path = intent.path] {
                std::string error;
                if (!openNotebook(path, error)) showLibrary("Can't open it: " + error);
            };
            deferredTicks_ = 1;
            break;
    }
}

void App::afterEditorInput() {
    if (!editor_) return;
    if (settings_) {
        settings_->set("eraser", editor_->eraseWholeStrokes() ? "stroke" : "partial");
        settings_->set("shapes", editor_->recognizeShapes() ? "on" : "off");
        for (size_t k = 0; k < 3; ++k)
            settings_->set(kSizeKeys[k], kSizeNames[static_cast<int>(editor_->toolSize(static_cast<ToolKind>(k)))]);
    }
    if (!editor_->closeRequested()) return;
    const bool quit = editor_->quitRequested() || !library_;
    editor_.reset();  // it has saved already
    if (quit) quit_ = true;
    else showLibrary();
}

void App::redrawAll(Refresh mode) {
    if (editor_) editor_->redrawAll(mode);
    else library_->redrawAll(mode);
}

void App::penDown(const PenSample& s, uint64_t now) {
    if (editor_) {
        editor_->penDown(s, now);
        afterEditorInput();
    } else if (!deferred_) {
        handle(library_->tap(static_cast<int>(s.x), static_cast<int>(s.y)), now);
    }
}

void App::penMove(const PenSample& s, uint64_t now) {
    if (editor_) editor_->penMove(s, now);
}

void App::penUp(uint64_t now) {
    if (editor_) editor_->penUp(now);
}

void App::touchDown(int x, int y, uint64_t now) {
    if (editor_) {
        editor_->touchDown(x, y, now);
        afterEditorInput();
    } else if (!deferred_) {
        handle(library_->tap(x, y), now);
    }
}

void App::touchDown(int id, int x, int y, uint64_t now) {
    ++fingers_;
    if (editor_) {
        editor_->touchDown(id, x, y, now);
        afterEditorInput();
    } else if (!deferred_ && fingers_ == 1) {
        handle(library_->tap(x, y), now);
    }
}

void App::touchMove(int id, int x, int y, uint64_t now) {
    if (editor_) editor_->touchMove(id, x, y, now);
}

void App::touchUp(int id, uint64_t now) {
    fingers_ = std::max(0, fingers_ - 1);
    if (editor_) editor_->touchUp(id, now);
}

void App::setPenNear(bool near) {
    penNear_ = near;
    if (editor_) editor_->setPenNear(near);
}

void App::zoomBy(double factor, double x, double y, uint64_t now) {
    if (editor_) editor_->zoomBy(factor, x, y, now);
}

void App::panBy(double dx, double dy, uint64_t now) {
    if (editor_) editor_->panBy(dx, dy, now);
}

void App::perform(Action a, uint64_t now) {
    if (editor_) {
        editor_->perform(a, now);
        afterEditorInput();
    } else if (!deferred_) {
        handle(library_->perform(a), now);
    }
}

void App::onIdle(uint64_t now) {
    // The first idle pass after scheduling only lets the tap feedback get presented.
    if (deferred_ && deferredTicks_-- <= 0) {
        auto work = std::move(deferred_);
        deferred_ = nullptr;
        work();
    }
    if (editor_) {
        editor_->onIdle(now);
        afterEditorInput();  // BACK and EXIT finish here
    }
}

int App::msUntilIdleWork(uint64_t now) const {
    if (deferred_) return 0;
    return editor_ ? editor_->msUntilIdleWork(now) : -1;
}

std::vector<Damage> App::takeDamage() { return editor_ ? editor_->takeDamage() : library_->takeDamage(); }

bool App::saveNow(uint64_t now) {
    if (!editor_ || !editor_->modified()) return true;
    return editor_->save(now, true);
}

}  // namespace scrible
