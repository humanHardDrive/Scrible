#pragma once

// The notebook list, a small file manager: every .xopp/.xoj in one folder, with
// naming, renaming, duplicating, and deleting (into a Trash folder).

#include <ctime>
#include <string>
#include <vector>

#include "Canvas.h"
#include "Editor.h"
#include "Settings.h"
#include "TextPrompt.h"
#include "Toolbar.h"

namespace scrible {

struct NotebookInfo {
    std::string path;
    std::string name;  // file name without the extension
    std::time_t modified = 0;
    long long bytes = 0;
};

enum class NotebookOrder { Newest, Name };

// .xopp and .xoj files in `dir` (not hidden ones).
std::vector<NotebookInfo> listNotebooks(const std::string& dir, std::string* error = nullptr,
                                        NotebookOrder order = NotebookOrder::Newest);
// True for a path ending in .xopp/.xoj or naming an existing file (as opposed to a folder).
bool isNotebookFile(const std::string& path);
// "2026-11-02 14.30": the default name of a notebook started at `when`.
std::string dateName(std::time_t when);
// An unused path for a new notebook named after `when`, e.g. "2026-11-02 14.30.xopp".
std::string newNotebookPath(const std::string& dir, std::time_t when);
bool makeDirs(const std::string& dir, std::string* error = nullptr);
// Case-insensitive, with digit runs compared as numbers ("Lecture 2" < "Lecture 10").
bool naturalLess(const std::string& a, const std::string& b);

// Checks a typed name and tidies it: surrounding spaces and leading dots go, and so
// do trailing dots and spaces (FAT storage would drop them anyway).
bool cleanNotebookName(const std::string& typed, std::string& name, std::string& error);

// These move or copy Xournal++'s files named after the notebook along with it
// ("<name>.xopp.bg.pdf" for an attached PDF, "<name>.xopp.bg_1.png", ...).
bool renameNotebook(const NotebookInfo& nb, const std::string& newName, std::string& newPath, std::string& error);
bool duplicateNotebook(const NotebookInfo& nb, const std::string& newName, std::string& newPath,
                       std::string& error);
bool trashNotebook(const NotebookInfo& nb, std::string& error);  // into "<folder>/Trash"

class Library {
public:
    // What the user asked for that the App has to act on.
    struct Intent {
        enum class Kind { None, Open, New, Exit } kind = Kind::None;
        std::string path;  // Open: the notebook; New: where to create it
    };

    // `settings` (may be null) keeps the sort order; it's shared with the rest of the app.
    Library(int width, int height, std::string dir, Settings* settings, int toolbarHeight = 120);

    Canvas& canvas() { return canvas_; }
    const std::string& dir() const { return dir_; }
    const std::vector<NotebookInfo>& notebooks() const { return notebooks_; }
    const Toolbar& toolbar() const { return toolbar_; }
    const std::string& message() const { return message_; }
    NotebookOrder order() const { return order_; }
    bool prompting() const { return mode_ == Mode::Prompt; }
    const TextPrompt& prompt() const { return prompt_; }

    // Row of notebooks()[index] and its "..." menu button, or empty if the row
    // isn't on the current list page.
    IRect rowRect(size_t index) const;
    IRect menuRect(size_t index) const;
    // The open menu or confirmation dialog, and one of its buttons; empty if none is open.
    IRect dialogRect() const { return dialogOpen() ? dialog_ : IRect{}; }
    IRect dialogButton(Action a) const;

    void rescan();
    void setMessage(std::string text) { message_ = std::move(text); }
    // Lines about the platform for the About dialog (the device, its libraries).
    void setPlatformInfo(std::vector<std::string> lines) { platformInfo_ = std::move(lines); }
    void redrawAll(Refresh mode = Refresh::Full);

    Intent tap(int x, int y);  // pen or finger
    Intent perform(Action a);
    std::vector<Damage> takeDamage();

private:
    enum class Mode { List, Menu, ConfirmDelete, About, Prompt };
    enum class PromptFor { New, Rename, Duplicate };
    struct DialogButton {
        Action action;
        std::string label;
        IRect r;
    };

    bool dialogOpen() const { return mode_ == Mode::Menu || mode_ == Mode::ConfirmDelete || mode_ == Mode::About; }
    size_t pageCount() const;
    void drawList();
    void drawRow(size_t index, bool highlighted, bool menuPressed = false);
    // Draws a dialog next to (not over) `anchor`, the row it's about.
    void drawDialog(const IRect& anchor, const std::string& title, const std::vector<std::string>& lines,
                    const std::vector<std::pair<Action, std::string>>& buttons);
    void showList(Refresh mode);
    void openPrompt(PromptFor what);
    Intent promptResult(TextPrompt::Result r);

    Canvas canvas_;
    Toolbar toolbar_;
    TextPrompt prompt_;
    std::string dir_;
    Settings* settings_;
    NotebookOrder order_;
    std::vector<NotebookInfo> notebooks_;
    std::string scanError_, message_;
    std::vector<std::string> platformInfo_;
    size_t page_ = 0;
    int listTop_;
    int rowHeight_ = 160;
    size_t rowsPerPage_;

    Mode mode_ = Mode::List;
    PromptFor promptFor_ = PromptFor::New;
    size_t target_ = 0;  // the notebook a menu, confirmation or rename is about
    IRect dialog_;
    std::vector<DialogButton> dialogButtons_;
    std::vector<Damage> damage_;
};

}  // namespace scrible
