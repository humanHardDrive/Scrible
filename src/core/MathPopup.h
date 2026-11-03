#pragma once

#include <string>
#include <vector>

#include "Canvas.h"
#include "DigitReader.h"
#include "Toolbar.h"

namespace scrible {

// The answer to one of the selection's math buttons (SUM, AVERAGE, MIN, MAX, COUNT),
// above the numbers it was worked out from, each character in a box of its own: a
// misread one is tapped, and the right character picked from a row of keys. Black
// and white only, like the toolbar.
class MathPopup {
public:
    explicit MathPopup(const IRect& area);  // the page area; the popup is centered in it

    bool open() const { return open_; }
    void show(Action op, std::vector<ReadNumber> numbers);
    void close();

    const IRect& rect() const { return rect_; }
    bool covers(int x, int y) const { return rect_.contains(x, y); }

    struct Tap {
        bool close = false;    // DONE
        bool changed = false;  // something to redraw
        // A character was corrected to this digit: its strokes can teach the reader.
        bool taught = false;
        DigitFeatures features{};
        int digit = -1;
    };
    Tap tap(int x, int y);

    void draw(Canvas& c, const IRect& clip) const;

    // What it shows.
    std::string result() const;  // "SUM = 49.5"
    const std::vector<ReadNumber>& numbers() const { return numbers_; }
    IRect cellRect(size_t number, size_t ch) const;
    IRect keyRect(char c) const;  // while a character is picked
    IRect doneRect() const { return done_; }

private:
    void layout();

    IRect area_, rect_, done_;
    bool open_ = false;
    Action op_ = Action::None;
    std::vector<ReadNumber> numbers_;
    size_t rows_ = 0;  // numbers shown (the rest still count)
    int pickedNumber_ = -1, pickedChar_ = -1;
    struct Key {
        char c;
        IRect r;
    };
    std::vector<Key> keys_;
};

}  // namespace scrible
