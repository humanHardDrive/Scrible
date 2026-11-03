#include "TextPrompt.h"

#include <algorithm>

namespace scrible {
namespace {

constexpr int kMargin = 40;
constexpr int kFieldScale = 6, kMessageScale = 3, kCharScale = 6, kWordScale = 4;
constexpr size_t kMaxChars = 80;

// Thins out black pixels in a 50% checkerboard: reads as gray, but stays black/white.
void checker(Canvas& c, const IRect& r) {
    const IRect a = r.intersected(c.rect());
    for (int y = a.y0; y < a.y1; ++y)
        for (int x = a.x0 + ((a.x0 + y) & 1); x < a.x1; x += 2) c.row(y)[x] = 255;
}

// The last `n` code points of `s`.
std::string tailCodePoints(const std::string& s, size_t n) {
    size_t i = s.size();
    while (i > 0 && n > 0)
        if ((static_cast<unsigned char>(s[--i]) & 0xc0) != 0x80) --n;
    return s.substr(i);
}

void eraseLastCodePoint(std::string& s) {
    while (!s.empty()) {
        const unsigned char ch = static_cast<unsigned char>(s.back());
        s.pop_back();
        if ((ch & 0xc0) != 0x80) break;
    }
}

}  // namespace

TextPrompt::TextPrompt(int width, int height, int toolbarHeight)
    : bar_(width, toolbarHeight,
           {{{Action::Cancel, "CANCEL"}}, {{Action::None, "DUPLICATE NOTEBOOK"}}, {{Action::Confirm, "OK"}}}) {
    field_ = {kMargin, toolbarHeight + 60, width - kMargin, toolbarHeight + 60 + 140};
    message_ = {kMargin, field_.y1 + 20, width - kMargin, field_.y1 + 80};

    // Rows of keys, in units of a tenth of the keyboard width.
    struct Spec {
        KeyKind kind;
        const char* label;
        const char* shifted;
        double units;
    };
    const std::vector<std::vector<Spec>> rows = {
        {{KeyKind::Char, "1", "1", 1}, {KeyKind::Char, "2", "2", 1}, {KeyKind::Char, "3", "3", 1},
         {KeyKind::Char, "4", "4", 1}, {KeyKind::Char, "5", "5", 1}, {KeyKind::Char, "6", "6", 1},
         {KeyKind::Char, "7", "7", 1}, {KeyKind::Char, "8", "8", 1}, {KeyKind::Char, "9", "9", 1},
         {KeyKind::Char, "0", "0", 1}},
        {{KeyKind::Char, "q", "Q", 1}, {KeyKind::Char, "w", "W", 1}, {KeyKind::Char, "e", "E", 1},
         {KeyKind::Char, "r", "R", 1}, {KeyKind::Char, "t", "T", 1}, {KeyKind::Char, "y", "Y", 1},
         {KeyKind::Char, "u", "U", 1}, {KeyKind::Char, "i", "I", 1}, {KeyKind::Char, "o", "O", 1},
         {KeyKind::Char, "p", "P", 1}},
        {{KeyKind::Char, "a", "A", 1}, {KeyKind::Char, "s", "S", 1}, {KeyKind::Char, "d", "D", 1},
         {KeyKind::Char, "f", "F", 1}, {KeyKind::Char, "g", "G", 1}, {KeyKind::Char, "h", "H", 1},
         {KeyKind::Char, "j", "J", 1}, {KeyKind::Char, "k", "K", 1}, {KeyKind::Char, "l", "L", 1},
         {KeyKind::Char, "'", "'", 1}},
        {{KeyKind::Shift, "SHIFT", "SHIFT", 1.5}, {KeyKind::Char, "z", "Z", 1}, {KeyKind::Char, "x", "X", 1},
         {KeyKind::Char, "c", "C", 1}, {KeyKind::Char, "v", "V", 1}, {KeyKind::Char, "b", "B", 1},
         {KeyKind::Char, "n", "N", 1}, {KeyKind::Char, "m", "M", 1}, {KeyKind::Delete, "DEL", "DEL", 1.5}},
        {{KeyKind::Clear, "CLEAR", "CLEAR", 1.5}, {KeyKind::Char, "-", "-", 1}, {KeyKind::Char, "_", "_", 1},
         {KeyKind::Space, "SPACE", "SPACE", 3.5}, {KeyKind::Char, "(", "(", 1}, {KeyKind::Char, ")", ")", 1},
         {KeyKind::Char, ".", ".", 1}},
    };

    const int keyH = 150, gap = 8;
    const double unit = (width - 2.0 * 20) / 10;
    const int top = height - 30 - static_cast<int>(rows.size()) * keyH;
    keyboard_ = {0, top - gap, width, height};
    for (size_t r = 0; r < rows.size(); ++r) {
        double x = 20;
        const int y = top + static_cast<int>(r) * keyH;
        for (const Spec& s : rows[r]) {
            const IRect rect{static_cast<int>(x) + gap / 2, y + gap / 2, static_cast<int>(x + s.units * unit) - gap / 2,
                             y + keyH - gap / 2};
            keys_.push_back({s.kind, s.label, s.shifted, rect});
            x += s.units * unit;
        }
    }
}

void TextPrompt::open(Canvas& c, const std::string& title, const std::string& text, const std::string& placeholder) {
    title_ = title;
    text_ = text;
    placeholder_ = placeholder;
    error_.clear();
    shift_ = text_.empty();  // capitalize the first letter
    redraw(c);
}

void TextPrompt::redraw(Canvas& c, Refresh mode) {
    c.fill(c.rect(), 255);
    ToolbarState s;
    s.text = title_;
    bar_.draw(c, s);
    drawField(c);
    drawMessage(c);
    drawKeys(c);
    damage_.clear();
    damage_.push_back({c.rect(), mode});
}

void TextPrompt::drawField(Canvas& c) {
    c.fill(field_, 255);
    c.strokeRect(field_, 3, 0);
    const IRect inner{field_.x0 + 30, field_.y0, field_.x1 - 30, field_.y1};
    const int ty = field_.y0 + (field_.height() - 8 * kFieldScale) / 2;
    const size_t fits = static_cast<size_t>(inner.width() / (8 * kFieldScale)) - 1;  // room for the cursor
    if (text_.empty()) {
        c.drawText(inner.x0 + 8 * kFieldScale, ty, placeholder_, kFieldScale, 0, inner);
        checker(c, {inner.x0 + 8 * kFieldScale, ty, inner.x1, ty + 8 * kFieldScale});
        c.fill({inner.x0, ty + 8 * kFieldScale - 6, inner.x0 + 8 * kFieldScale - 6, ty + 8 * kFieldScale}, 0);
        return;
    }
    // Show the end of a long name, where the typing happens.
    const std::string shown = codePoints(text_) > fits ? "..." + tailCodePoints(text_, fits - 3) : text_;
    c.drawText(inner.x0, ty, shown, kFieldScale, 0, inner);
    const int cx = inner.x0 + static_cast<int>(codePoints(shown)) * 8 * kFieldScale;
    c.fill(IRect{cx, ty + 8 * kFieldScale - 6, cx + 8 * kFieldScale - 6, ty + 8 * kFieldScale}.intersected(inner), 0);
}

void TextPrompt::drawMessage(Canvas& c) {
    c.fill(message_, 255);
    std::string msg = error_;
    if (msg.empty() && text_.empty() && !placeholder_.empty()) msg = "Leave it empty to use the name shown.";
    c.drawText(message_.x0, message_.y0 + 10, msg, kMessageScale, 0, message_);
}

void TextPrompt::drawKeys(Canvas& c) {
    c.fill(keyboard_, 255);
    for (const Key& k : keys_) {
        const bool active = k.kind == KeyKind::Shift && shift_;
        c.fill(k.r, active ? 0 : 255);
        c.strokeRect(k.r, 3, 0);
        const std::string& label = shift_ ? k.shifted : k.label;
        const int scale = k.kind == KeyKind::Char ? kCharScale : kWordScale;
        const int tx = k.r.x0 + (k.r.width() - Canvas::textWidth(label, scale)) / 2;
        const int ty = k.r.y0 + (k.r.height() - 8 * scale) / 2;
        c.drawText(tx, ty, label, scale, active ? 255 : 0, k.r);
    }
}

void TextPrompt::edited(Canvas& c, bool shiftWas) {
    error_.clear();
    drawField(c);
    drawMessage(c);
    damage_.push_back({field_.united(message_), Refresh::UI});
    if (shift_ != shiftWas) {
        drawKeys(c);
        damage_.push_back({keyboard_, Refresh::UI});
    }
}

TextPrompt::Result TextPrompt::tap(Canvas& c, int x, int y) {
    if (bar_.rect().contains(x, y)) return perform(bar_.hit(x, y));
    for (const Key& k : keys_) {
        if (!k.r.contains(x, y)) continue;
        const bool shiftWas = shift_;
        switch (k.kind) {
            case KeyKind::Char:
                if (codePoints(text_) < kMaxChars) text_ += shift_ ? k.shifted : k.label;
                shift_ = false;  // one capital at a time
                break;
            case KeyKind::Space:
                if (codePoints(text_) < kMaxChars) text_ += ' ';
                break;
            case KeyKind::Shift: shift_ = !shift_; break;
            case KeyKind::Delete:
                eraseLastCodePoint(text_);
                if (text_.empty()) shift_ = true;
                break;
            case KeyKind::Clear:
                text_.clear();
                shift_ = true;
                break;
        }
        edited(c, shiftWas);
        return Result::None;
    }
    return Result::None;
}

TextPrompt::Result TextPrompt::perform(Action a) {
    if (a == Action::Confirm) return Result::Done;
    if (a == Action::Cancel || a == Action::Exit) return Result::Cancel;
    return Result::None;
}

void TextPrompt::setError(Canvas& c, const std::string& message) {
    error_ = message;
    drawMessage(c);
    damage_.push_back({message_, Refresh::UI});
}

std::vector<Damage> TextPrompt::takeDamage() {
    std::vector<Damage> out;
    out.swap(damage_);
    return out;
}

IRect TextPrompt::keyRect(const std::string& label) const {
    for (const Key& k : keys_)
        if (k.label == label) return k.r;
    return {};
}

}  // namespace scrible
