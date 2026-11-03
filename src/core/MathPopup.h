#pragma once

#include <string>
#include <vector>

#include "Canvas.h"
#include "DigitReader.h"
#include "Toolbar.h"

namespace scrible {

// What's behind a math answer on the page (SUM, AVERAGE, MIN, MAX, COUNT): the
// answer, above the numbers it was worked out from, each character in a box of its
// own. A misread one is tapped, and the right character picked from a row of keys.
// Black and white only, like the toolbar.
class MathPopup {
public:
    // The answer alone ("49.5"; empty if there are no numbers), and the function's
    // name in the file ("sum") and back.
    static std::string answer(Action op, const std::vector<ReadNumber>& numbers);
    static const char* opKey(Action op);
    static Action opFromKey(const std::string& key);

    explicit MathPopup(const IRect& area);  // the page area; the popup is centered in it

    bool open() const { return open_; }
    void show(Action op, std::vector<ReadNumber> numbers);
    void close();

    const IRect& rect() const { return rect_; }
    bool covers(int x, int y) const { return rect_.contains(x, y); }

    struct Tap {
        bool close = false;    // DONE
        bool remove = false;   // DELETE: the answer goes
        bool changed = false;  // something to redraw
        bool edited = false;   // a character was changed
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
    IRect deleteRect() const { return delete_; }

private:
    void layout();

    IRect area_, rect_, done_, delete_;
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
