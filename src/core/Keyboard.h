#pragma once

#include <string>
#include <vector>

#include "Canvas.h"

namespace scrible {

// An on-screen keyboard for typing on the page, across the bottom of the screen (or
// the top, so it doesn't hide what's being typed). Letters, with SHIFT for one
// capital; ?123 swaps them for symbols. Strictly black and white, like the toolbar,
// so it can be shown with the fast DU waveform.
class Keyboard {
public:
    enum class Key { None, Text, Shift, Symbols, Delete, Enter, Done };
    struct Press {
        Key key = Key::None;
        std::string text;  // for Key::Text
        IRect r;           // the key's place
    };

    Keyboard(int width, int height, int top);  // the screen; `top`: where it goes when docked at the top

    bool open() const { return open_; }
    void show(bool atTop);
    void close() { open_ = false; }
    bool atTop() const { return atTop_; }
    const IRect& rect() const { return rect_; }
    // Where it would be, docked at the bottom or the top.
    IRect rectAt(bool atTop) const;
    bool covers(int x, int y) const { return open_ && rect_.contains(x, y); }

    // A tap: what the key there does. SHIFT and ?123 change the keys, and typing
    // a letter lets go of SHIFT; changed() tells whether they need drawing again.
    Press tap(int x, int y);
    bool changed() const { return changed_; }
    void setShift(bool on);
    bool shift() const { return shift_; }
    bool symbols() const { return symbols_; }

    IRect keyRect(const std::string& label) const;  // by what it shows now, e.g. "a", "SPACE"

    void draw(Canvas& c, const IRect& clip) const;

private:
    struct KeyDef {
        Key key;
        std::string label, shifted, symbol;  // symbol empty: the same key among the symbols
        double units;
        IRect r;  // relative to the top of the keyboard
    };
    const std::string& shown(const KeyDef& k) const;
    Key action(const KeyDef& k) const;
    IRect placed(const KeyDef& k) const { return {k.r.x0, rect_.y0 + k.r.y0, k.r.x1, rect_.y0 + k.r.y1}; }

    int width_, height_, top_;
    int keyboardH_ = 0;
    std::vector<KeyDef> keys_;
    IRect rect_;
    bool open_ = false, atTop_ = false;
    bool shift_ = false, symbols_ = false, changed_ = false;
};

}  // namespace scrible
