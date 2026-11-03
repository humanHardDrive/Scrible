#pragma once

#include <string>
#include <vector>

#include "Canvas.h"
#include "Editor.h"
#include "Toolbar.h"

namespace scrible {

// Full-screen text entry with an on-screen keyboard, for naming notebooks.
// Strictly black and white, so every keystroke can use the fast DU refresh.
class TextPrompt {
public:
    enum class Result { None, Done, Cancel };

    TextPrompt(int width, int height, int toolbarHeight = 120);

    // Draws the whole prompt. While nothing is typed, `placeholder` is shown
    // (faintly) and is what OK uses.
    void open(Canvas& c, const std::string& title, const std::string& text, const std::string& placeholder);
    void redraw(Canvas& c, Refresh mode = Refresh::Full);
    Result tap(Canvas& c, int x, int y);
    Result perform(Action a);  // Confirm / Cancel (keyboard shortcuts)
    void setError(Canvas& c, const std::string& message);

    const std::string& text() const { return text_; }
    std::string value() const { return text_.empty() ? placeholder_ : text_; }
    std::vector<Damage> takeDamage();
    const Toolbar& toolbar() const { return bar_; }
    IRect keyRect(const std::string& label) const;  // by unshifted label, e.g. "a", "SPACE"
    IRect keyAt(int x, int y) const;                // the key there, if any
    void redrawKeys(Canvas& c);

private:
    enum class KeyKind { Char, Shift, Delete, Clear, Space };
    struct Key {
        KeyKind kind;
        std::string label, shifted;
        IRect r;
    };

    void drawField(Canvas& c);
    void drawMessage(Canvas& c);
    void drawKeys(Canvas& c);
    void edited(Canvas& c, bool shiftWas);

    Toolbar bar_;
    std::vector<Key> keys_;
    IRect field_, message_, keyboard_;
    std::string title_, text_, placeholder_, error_;
    bool shift_ = false;
    std::vector<Damage> damage_;
};

}  // namespace scrible
