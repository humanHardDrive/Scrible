#include "Library.h"

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace xs {
namespace {

constexpr int kMargin = 60;
constexpr int kTitleScale = 5, kNameScale = 5, kDetailScale = 3, kMessageScale = 3, kButtonScale = 4;
constexpr int kMenuButtonWidth = 160;
constexpr size_t kMaxNameChars = 120;
constexpr const char* kTrash = "Trash";

bool hasNotebookExtension(const std::string& name, size_t& stem) {
    for (const char* ext : {".xopp", ".xoj"}) {
        const size_t n = std::strlen(ext);
        if (name.size() > n && strcasecmp(name.c_str() + name.size() - n, ext) == 0) {
            stem = name.size() - n;
            return true;
        }
    }
    return false;
}

bool exists(const std::string& path) { return access(path.c_str(), F_OK) == 0; }

std::string parentOf(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? "." : path.substr(0, slash);
}

std::string fileOf(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Shortens to at most `maxChars` code points, ending in "...".
std::string ellipsize(const std::string& s, size_t maxChars) {
    if (codePoints(s) <= maxChars) return s;
    std::string out;
    size_t n = 0;
    for (char c : s) {
        if ((static_cast<unsigned char>(c) & 0xc0) != 0x80 && ++n > maxChars - 3) break;
        out += c;
    }
    return out + "...";
}

std::string describe(const NotebookInfo& nb) {
    char date[32] = "";
    if (std::tm* t = std::localtime(&nb.modified)) std::strftime(date, sizeof date, "%Y-%m-%d %H:%M", t);
    const long long kb = (nb.bytes + 1023) / 1024;
    return std::string(date) + "   " + std::to_string(kb) + " KB";
}

// Suffixes of Xournal++'s files that belong to `file` in `dir`: "file.xopp.bg.pdf" -> ".bg.pdf".
std::vector<std::string> companionSuffixes(const std::string& dir, const std::string& file) {
    std::vector<std::string> out;
    const std::string prefix = file + ".";
    if (DIR* d = opendir(dir.c_str())) {
        while (const dirent* e = readdir(d)) {
            const std::string name = e->d_name;
            if (name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0)
                out.push_back(name.substr(file.size()));
        }
        closedir(d);
    }
    return out;
}

bool copyFile(const std::string& from, const std::string& to, std::string& error) {
    std::ifstream in(from, std::ios::binary);
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!in || !out || !(out << in.rdbuf()) || !out.flush()) {
        error = "can't copy " + fileOf(from) + ": " + std::strerror(errno);
        std::remove(to.c_str());
        return false;
    }
    return true;
}

}  // namespace

// ---- notebook files ----

std::vector<NotebookInfo> listNotebooks(const std::string& dir, std::string* error, NotebookOrder order) {
    std::vector<NotebookInfo> out;
    DIR* d = opendir(dir.c_str());
    if (!d) {
        if (error) *error = dir + ": " + std::strerror(errno);
        return out;
    }
    while (const dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        size_t stem = 0;
        if (name.empty() || name[0] == '.' || !hasNotebookExtension(name, stem)) continue;
        NotebookInfo nb;
        nb.path = dir + "/" + name;
        struct stat st {};
        if (stat(nb.path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        nb.name = name.substr(0, stem);
        nb.modified = st.st_mtime;
        nb.bytes = st.st_size;
        out.push_back(std::move(nb));
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [order](const NotebookInfo& a, const NotebookInfo& b) {
        if (order == NotebookOrder::Name) {
            if (naturalLess(a.name, b.name)) return true;
            if (naturalLess(b.name, a.name)) return false;
        }
        return a.modified != b.modified ? a.modified > b.modified : a.name < b.name;
    });
    return out;
}

bool isNotebookFile(const std::string& path) {
    size_t stem = 0;
    if (hasNotebookExtension(path, stem)) return true;
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string dateName(std::time_t when) {
    char base[32] = "Notebook";
    if (std::tm* t = std::localtime(&when)) std::strftime(base, sizeof base, "%Y-%m-%d %H.%M", t);
    return base;
}

std::string newNotebookPath(const std::string& dir, std::time_t when) {
    const std::string base = dateName(when);
    std::string path = dir + "/" + base + ".xopp";
    for (int n = 2; exists(path); ++n) path = dir + "/" + base + " (" + std::to_string(n) + ").xopp";
    return path;
}

bool makeDirs(const std::string& dir, std::string* error) {
    for (size_t pos = 1; pos != std::string::npos;) {
        pos = dir.find('/', pos + 1);
        const std::string part = dir.substr(0, pos);
        if (mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) {
            if (error) *error = part + ": " + std::strerror(errno);
            return false;
        }
    }
    return true;
}

bool naturalLess(const std::string& a, const std::string& b) {
    auto digit = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; };
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (digit(a[i]) && digit(b[j])) {
            size_t ie = i, je = j;
            while (ie < a.size() && digit(a[ie])) ++ie;
            while (je < b.size() && digit(b[je])) ++je;
            while (i + 1 < ie && a[i] == '0') ++i;  // compare the numbers, not leading zeros
            while (j + 1 < je && b[j] == '0') ++j;
            if (ie - i != je - j) return ie - i < je - j;
            const int c = a.compare(i, ie - i, b, j, je - j);
            if (c != 0) return c < 0;
            i = ie;
            j = je;
            continue;
        }
        const int ca = std::tolower(static_cast<unsigned char>(a[i]));
        const int cb = std::tolower(static_cast<unsigned char>(b[j]));
        if (ca != cb) return ca < cb;
        ++i;
        ++j;
    }
    return a.size() - i < b.size() - j;
}

bool cleanNotebookName(const std::string& typed, std::string& name, std::string& error) {
    std::string s = typed;
    while (!s.empty() && (s.front() == ' ' || s.front() == '.')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    if (s.empty()) {
        error = "Type a name first.";
        return false;
    }
    for (char c : s) {
        if (std::strchr("/\\:*?\"<>|", c) || static_cast<unsigned char>(c) < 0x20) {
            error = "A name can't contain / \\ : * ? \" < > |";
            return false;
        }
    }
    if (codePoints(s) > kMaxNameChars) {
        error = "That name is too long.";
        return false;
    }
    name = s;
    return true;
}

bool renameNotebook(const NotebookInfo& nb, const std::string& newName, std::string& newPath, std::string& error) {
    const std::string dir = parentOf(nb.path), file = fileOf(nb.path);
    const std::string ext = file.substr(nb.name.size());
    newPath = dir + "/" + newName + ext;
    if (newPath == nb.path) return true;
    // A change of case only is the same file on case-insensitive storage.
    if (strcasecmp(newName.c_str(), nb.name.c_str()) != 0 && exists(newPath)) {
        error = "\"" + newName + "\" already exists.";
        return false;
    }
    const auto companions = companionSuffixes(dir, file);
    if (std::rename(nb.path.c_str(), newPath.c_str()) != 0) {
        error = std::string("can't rename: ") + std::strerror(errno);
        return false;
    }
    for (const std::string& suffix : companions) {
        const std::string from = dir + "/" + file + suffix, to = newPath + suffix;
        if (std::rename(from.c_str(), to.c_str()) != 0)
            std::fprintf(stderr, "xscribe: can't rename %s: %s\n", from.c_str(), std::strerror(errno));
    }
    return true;
}

bool duplicateNotebook(const NotebookInfo& nb, const std::string& newName, std::string& newPath,
                       std::string& error) {
    const std::string dir = parentOf(nb.path), file = fileOf(nb.path);
    newPath = dir + "/" + newName + file.substr(nb.name.size());
    if (exists(newPath)) {
        error = "\"" + newName + "\" already exists.";
        return false;
    }
    if (!copyFile(nb.path, newPath, error)) return false;
    for (const std::string& suffix : companionSuffixes(dir, file)) {
        std::string ignored;
        if (!copyFile(dir + "/" + file + suffix, newPath + suffix, ignored))
            std::fprintf(stderr, "xscribe: %s\n", ignored.c_str());
    }
    return true;
}

bool trashNotebook(const NotebookInfo& nb, std::string& error) {
    const std::string dir = parentOf(nb.path), file = fileOf(nb.path);
    const std::string ext = file.substr(nb.name.size());
    const std::string trash = dir + "/" + kTrash;
    if (!makeDirs(trash, &error)) return false;
    std::string target = trash + "/" + nb.name + ext;
    for (int n = 2; exists(target); ++n) target = trash + "/" + nb.name + " (" + std::to_string(n) + ")" + ext;

    const auto companions = companionSuffixes(dir, file);
    if (std::rename(nb.path.c_str(), target.c_str()) != 0) {
        error = std::string("can't move it to the Trash: ") + std::strerror(errno);
        return false;
    }
    for (const std::string& suffix : companions) {
        const std::string from = dir + "/" + file + suffix, to = target + suffix;
        if (std::rename(from.c_str(), to.c_str()) != 0)
            std::fprintf(stderr, "xscribe: can't move %s: %s\n", from.c_str(), std::strerror(errno));
    }
    return true;
}

// ---- the screen ----

Library::Library(int width, int height, std::string dir, Settings* settings, int toolbarHeight)
    : canvas_(width, height),
      toolbar_(width, toolbarHeight,
               {{{Action::NewNotebook, "NEW"}, {Action::Sort, "NEWEST"}},
                {{Action::PrevPage, "<"}, {Action::None, "000/000"}, {Action::NextPage, ">"}},
                {{Action::Exit, "EXIT"}}}),
      prompt_(width, height, toolbarHeight),
      dir_(std::move(dir)),
      settings_(settings),
      order_(settings_ && settings_->get("sort") == "name" ? NotebookOrder::Name : NotebookOrder::Newest),
      listTop_(toolbarHeight + 170),
      rowsPerPage_(static_cast<size_t>(std::max(1, (height - listTop_ - 20) / rowHeight_))) {
    rescan();
    redrawAll();
}

void Library::rescan() {
    scanError_.clear();
    notebooks_ = listNotebooks(dir_, &scanError_, order_);
    page_ = std::min(page_, pageCount() - 1);
}

size_t Library::pageCount() const { return std::max<size_t>(1, (notebooks_.size() + rowsPerPage_ - 1) / rowsPerPage_); }

IRect Library::rowRect(size_t index) const {
    if (mode_ == Mode::Prompt || index / rowsPerPage_ != page_ || index >= notebooks_.size()) return {};
    const int top = listTop_ + static_cast<int>(index % rowsPerPage_) * rowHeight_;
    return {0, top, canvas_.width(), top + rowHeight_};
}

IRect Library::menuRect(size_t index) const {
    const IRect r = rowRect(index);
    if (r.empty()) return {};
    return {r.x1 - kMargin - kMenuButtonWidth, r.y0 + 25, r.x1 - kMargin, r.y1 - 25};
}

IRect Library::dialogButton(Action a) const {
    if (mode_ != Mode::Menu && mode_ != Mode::ConfirmDelete) return {};
    for (const DialogButton& b : dialogButtons_)
        if (b.action == a) return b.r;
    return {};
}

void Library::drawRow(size_t index, bool highlighted, bool menuPressed) {
    const IRect r = rowRect(index);
    const uint8_t fg = highlighted ? 255 : 0;
    canvas_.fill(r, highlighted ? 0 : 255);
    canvas_.fill({r.x0 + kMargin, r.y1 - 2, r.x1 - kMargin, r.y1}, 0);
    const NotebookInfo& nb = notebooks_[index];
    const size_t maxChars =
        static_cast<size_t>((canvas_.width() - 2 * kMargin - kMenuButtonWidth - 40) / (8 * kNameScale));
    canvas_.drawText(kMargin, r.y0 + 30, ellipsize(nb.name, maxChars), kNameScale, fg, r);
    canvas_.drawText(kMargin, r.y0 + 95, describe(nb), kDetailScale, fg, r);

    // The "..." button, inverted while its menu is open.
    const IRect m = menuRect(index);
    const bool dark = highlighted != menuPressed;
    canvas_.fill(m, dark ? 0 : 255);
    canvas_.strokeRect(m, 3, highlighted ? 255 : 0);
    canvas_.drawText(m.x0 + (m.width() - Canvas::textWidth("...", kNameScale)) / 2,
                     m.y0 + (m.height() - 8 * kNameScale) / 2, "...", kNameScale, dark ? 255 : 0, m);
}

void Library::drawList() {
    // Strictly black and white, like the toolbar, so any part can be refreshed with DU.
    canvas_.fill(canvas_.rect(), 255);
    ToolbarState s;
    if (page_ == 0) s.disabled.push_back(Action::PrevPage);
    if (page_ + 1 >= pageCount()) s.disabled.push_back(Action::NextPage);
    s.labels = {{Action::Sort, order_ == NotebookOrder::Name ? "A-Z" : "NEWEST"}};
    s.text = std::to_string(page_ + 1) + "/" + std::to_string(pageCount());
    toolbar_.draw(canvas_, s);

    const int titleY = toolbar_.rect().y1 + 40;
    canvas_.drawText(kMargin, titleY, "NOTEBOOKS", kTitleScale, 0, canvas_.rect());
    const std::string& msg = !message_.empty() ? message_ : scanError_;
    if (!msg.empty()) {
        const size_t maxChars = static_cast<size_t>((canvas_.width() - 2 * kMargin) / (8 * kMessageScale));
        canvas_.drawText(kMargin, titleY + 70, ellipsize(msg, maxChars), kMessageScale, 0, canvas_.rect());
    }

    if (notebooks_.empty()) {
        canvas_.drawText(kMargin, listTop_ + 40, "No notebooks yet.", kNameScale, 0, canvas_.rect());
        canvas_.drawText(kMargin, listTop_ + 120, "Tap NEW to start one, or copy .xopp files into", kDetailScale, 0,
                         canvas_.rect());
        canvas_.drawText(kMargin, listTop_ + 160, dir_, kDetailScale, 0, canvas_.rect());
    }
    for (size_t i = page_ * rowsPerPage_; i < notebooks_.size() && i < (page_ + 1) * rowsPerPage_; ++i)
        drawRow(i, false);
}

void Library::drawDialog(const IRect& anchor, const std::string& title, const std::vector<std::string>& lines,
                         const std::vector<std::pair<Action, std::string>>& buttons) {
    const int w = canvas_.width() - 2 * 160, buttonH = 130, gap = 24, pad = 50;
    const int h = pad + 8 * kTitleScale + 30 + static_cast<int>(lines.size()) * 50 + 20 +
                  static_cast<int>(buttons.size()) * (buttonH + gap) + pad - gap;
    const int x0 = (canvas_.width() - w) / 2;
    // Below the row if it fits, else above it, so the tapped row stays visible.
    int y0 = (canvas_.height() - h) / 2;
    if (!anchor.empty()) {
        if (anchor.y1 + 20 + h <= canvas_.height() - 20) y0 = anchor.y1 + 20;
        else if (anchor.y0 - 20 - h >= toolbar_.rect().y1) y0 = anchor.y0 - 20 - h;
    }
    dialog_ = {x0, y0, x0 + w, y0 + h};
    canvas_.fill(dialog_, 255);
    canvas_.strokeRect(dialog_, 6, 0);

    const IRect inner{x0 + pad, y0 + pad, x0 + w - pad, y0 + h - pad};
    const size_t titleChars = static_cast<size_t>(inner.width() / (8 * kTitleScale));
    const size_t lineChars = static_cast<size_t>(inner.width() / (8 * kMessageScale));
    int y = inner.y0;
    canvas_.drawText(inner.x0, y, ellipsize(title, titleChars), kTitleScale, 0, inner);
    y += 8 * kTitleScale + 30;
    for (const std::string& line : lines) {
        canvas_.drawText(inner.x0, y, ellipsize(line, lineChars), kMessageScale, 0, inner);
        y += 50;
    }
    y += 20;
    dialogButtons_.clear();
    for (const auto& [action, label] : buttons) {
        const IRect r{inner.x0, y, inner.x1, y + buttonH};
        canvas_.strokeRect(r, 3, 0);
        canvas_.drawText(r.x0 + (r.width() - Canvas::textWidth(label, kButtonScale)) / 2,
                         r.y0 + (r.height() - 8 * kButtonScale) / 2, label, kButtonScale, 0, r);
        dialogButtons_.push_back({action, label, r});
        y += buttonH + gap;
    }
}

void Library::redrawAll(Refresh mode) {
    damage_.clear();
    switch (mode_) {
        case Mode::Prompt:
            prompt_.redraw(canvas_, mode);
            return;
        case Mode::List: drawList(); break;
        case Mode::Menu:
            drawList();
            drawRow(target_, false, true);
            drawDialog(rowRect(target_), notebooks_[target_].name, {describe(notebooks_[target_])},
                       {{Action::Rename, "RENAME"},
                        {Action::Duplicate, "DUPLICATE"},
                        {Action::Delete, "DELETE"},
                        {Action::Cancel, "CANCEL"}});
            break;
        case Mode::ConfirmDelete:
            drawList();
            drawRow(target_, false, true);
            drawDialog(rowRect(target_), "Delete this notebook?",
                       {notebooks_[target_].name, "It goes into the Trash folder next to your notebooks,",
                        "where you can still get it back over USB."},
                       {{Action::Confirm, "DELETE"}, {Action::Cancel, "CANCEL"}});
            break;
    }
    damage_.push_back({canvas_.rect(), mode});
}

void Library::showList(Refresh mode) {
    mode_ = Mode::List;
    rescan();
    redrawAll(mode);
}

void Library::openPrompt(PromptFor what) {
    promptFor_ = what;
    mode_ = Mode::Prompt;
    message_.clear();
    switch (what) {
        case PromptFor::New: {
            const std::string file = fileOf(newNotebookPath(dir_, std::time(nullptr)));  // unused, date-based
            prompt_.open(canvas_, "NEW NOTEBOOK", "", file.substr(0, file.size() - 5));
            break;
        }
        case PromptFor::Rename: prompt_.open(canvas_, "RENAME NOTEBOOK", notebooks_[target_].name, ""); break;
        case PromptFor::Duplicate:
            prompt_.open(canvas_, "DUPLICATE NOTEBOOK", notebooks_[target_].name + " copy", "");
            break;
    }
}

Library::Intent Library::promptResult(TextPrompt::Result r) {
    if (r == TextPrompt::Result::None) return {};
    if (r == TextPrompt::Result::Cancel) {
        showList(Refresh::Full);
        return {};
    }
    std::string name, error, path;
    if (!cleanNotebookName(prompt_.value(), name, error)) {
        prompt_.setError(canvas_, error);
        return {};
    }
    switch (promptFor_) {
        case PromptFor::New:
            path = dir_ + "/" + name + ".xopp";
            if (exists(path)) {
                prompt_.setError(canvas_, "\"" + name + "\" already exists.");
                return {};
            }
            mode_ = Mode::List;  // where BACK returns to
            return {Intent::Kind::New, path};
        case PromptFor::Rename:
            if (!renameNotebook(notebooks_[target_], name, path, error)) {
                prompt_.setError(canvas_, error);
                return {};
            }
            break;
        case PromptFor::Duplicate:
            if (!duplicateNotebook(notebooks_[target_], name, path, error)) {
                prompt_.setError(canvas_, error);
                return {};
            }
            break;
    }
    showList(Refresh::Full);
    return {};
}

Library::Intent Library::tap(int x, int y) {
    switch (mode_) {
        case Mode::Prompt: return promptResult(prompt_.tap(canvas_, x, y));
        case Mode::Menu:
        case Mode::ConfirmDelete:
            for (const DialogButton& b : dialogButtons_)
                if (b.r.contains(x, y)) return perform(b.action);
            if (!dialog_.contains(x, y)) return perform(Action::Cancel);  // tapping outside closes it
            return {};
        case Mode::List: break;
    }
    if (toolbar_.rect().contains(x, y)) return perform(toolbar_.hit(x, y));
    for (size_t i = page_ * rowsPerPage_; i < notebooks_.size() && i < (page_ + 1) * rowsPerPage_; ++i) {
        const IRect r = rowRect(i);
        if (!r.contains(x, y)) continue;
        if (menuRect(i).grown(20).contains(x, y)) {
            target_ = i;
            mode_ = Mode::Menu;
            // Everything is black and white, so DU redraws just what changed: the
            // pressed button and the menu.
            redrawAll(Refresh::UI);
            return {};
        }
        drawRow(i, true);  // feedback right away; opening can take a moment
        damage_.push_back({r, Refresh::UI});
        return {Intent::Kind::Open, notebooks_[i].path};
    }
    return {};
}

Library::Intent Library::perform(Action a) {
    if (mode_ == Mode::Prompt) return promptResult(prompt_.perform(a));

    if (mode_ == Mode::Menu || mode_ == Mode::ConfirmDelete) {
        std::string error;
        switch (a) {
            case Action::Rename: openPrompt(PromptFor::Rename); break;
            case Action::Duplicate: openPrompt(PromptFor::Duplicate); break;
            case Action::Delete:
                mode_ = Mode::ConfirmDelete;
                redrawAll(Refresh::UI);
                break;
            case Action::Confirm: {
                const std::string name = notebooks_[target_].name;
                if (trashNotebook(notebooks_[target_], error)) message_ = "\"" + name + "\" moved to the Trash folder.";
                else message_ = "Couldn't delete \"" + name + "\": " + error;
                showList(Refresh::Content);
                break;
            }
            case Action::Cancel:
            case Action::Exit: showList(Refresh::Content); break;
            default: break;
        }
        return {};
    }

    switch (a) {
        case Action::NewNotebook: openPrompt(PromptFor::New); return {};
        case Action::Exit: return {Intent::Kind::Exit, {}};
        case Action::Sort:
            order_ = order_ == NotebookOrder::Newest ? NotebookOrder::Name : NotebookOrder::Newest;
            if (settings_) settings_->set("sort", order_ == NotebookOrder::Name ? "name" : "newest");
            page_ = 0;
            showList(Refresh::Full);
            return {};
        case Action::PrevPage:
        case Action::NextPage: {
            const size_t page = a == Action::PrevPage ? (page_ ? page_ - 1 : 0) : std::min(page_ + 1, pageCount() - 1);
            if (page != page_) {
                page_ = page;
                redrawAll(Refresh::Full);
            }
            return {};
        }
        default: return {};
    }
}

std::vector<Damage> Library::takeDamage() {
    std::vector<Damage> out;
    out.swap(damage_);
    for (const Damage& d : prompt_.takeDamage()) out.push_back(d);
    return out;
}

}  // namespace xs
