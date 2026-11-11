#include "Editor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "Eraser.h"
#include "Font.h"
#include "Lasso.h"
#include "Shapes.h"
#include "XoppFile.h"

namespace scrible {
namespace {

constexpr uint8_t kOffPageGray = 220;
constexpr double kMinPointDistPx = 0.75;  // closer samples are dropped
constexpr size_t kMaxUndo = 1000;
constexpr uint64_t kDragFrameMs = 50;  // at most this often while moving a selection (e-ink can't keep up)
constexpr uint64_t kViewFrameMs = 60;  // ...or the whole page (the Kindle also waits for the last one)
constexpr double kMaxZoom = 4;
constexpr double kZoomSteps[] = {1, 1.5, 2, 3, 4};  // the zoom buttons'
constexpr uint64_t kFlashMs = 250;                  // how long a pressed button shows it
constexpr int kTapId = -1;                         // the finger of touchDown(x, y)
constexpr double kFingerSlop = 50;                 // px fingers move before they zoom or pan (~4 mm)
constexpr double kMoveSlop = 12;                   // px the pen moves before a text it's on moves (~1 mm)

bool sameView(const PageTransform& a, const PageTransform& b) {
    return a.scale == b.scale && a.offsetX == b.offsetX && a.offsetY == b.offsetY;
}

double msSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}
constexpr int kFramePad = 14, kHandle = 24, kOverlayScale = 3;
constexpr int kInfoGap = 8, kInfoSize = 44;  // a math answer's i: this far right of it, this big

// Preset widths (points) of the original Xournal, indexed by ToolSize.
constexpr double kPenWidths[] = {0.85, 1.41, 2.26};
constexpr double kHighlighterWidths[] = {2.83, 8.50, 19.84};
constexpr double kEraserWidths[] = {2.83, 8.50, 19.84};
constexpr double kTextSizes[] = {10, 14, 20};  // points, like Xournal++'s font sizes
constexpr double kTextEdge = 5;                // points kept clear at the page's right edge while typing
// Xournal++'s dotted and dashed patterns (points along the line), for the fine and
// medium pens; thicker ones space them out in proportion, or the dots would touch.
const std::vector<double> kDotPattern = {0.5, 3}, kDashPattern = {6, 3};

std::vector<double> linePattern(LineStyle line, double width) {
    if (line == LineStyle::Solid) return {};
    std::vector<double> p = line == LineStyle::Dots ? kDotPattern : kDashPattern;
    const double k = std::max(1.0, width / kPenWidths[1]);
    for (double& d : p) d = std::round(d * k * 100) / 100;  // as it's saved
    return p;
}

// The drawing tools are in the panel along the left edge (ToolPanel). The notebook's
// name fills the room between UNDO / REDO and the page controls.
ToolbarGroups editorToolbar(bool fromList) {
    ToolbarGroups g = {
        {{Action::Undo, "UNDO"}, {Action::Redo, "REDO"}},
        {{Action::None, ""}},
        {{Action::PageStyle, "PAGE"}, {Action::PrevPage, "<"}, {Action::None, "000/000"}, {Action::NextPage, ">"}},
        {{Action::Save, "SAVE*"}},
    };
    if (fromList) g.back().insert(g.back().end(), {{Action::Exit, "BACK"}, {Action::Quit, "EXIT"}});
    else g.back().push_back({Action::Exit, "EXIT"});
    return g;
}

// How far something spanning lo..hi may move (by d) and stay within 0..size: up to
// the edges. If it's over an edge already, it may stay there but go no further.
double keepOnPage(double d, double lo, double hi, double size) {
    return std::clamp(d, std::min(0.0, -lo), std::max(0.0, size - hi));
}

// Whether a sweep of the eraser from a to b, r to either side, touches the box.
bool sweepTouches(const RectF& box, const StrokePoint& a, const StrokePoint& b, double r) {
    RectF g = box;
    g.grow(r);
    const int steps = std::max(1, static_cast<int>(std::ceil(std::hypot(b.x - a.x, b.y - a.y) / std::max(r, 0.5))));
    for (int k = 0; k <= steps; ++k) {
        const double x = a.x + (b.x - a.x) * k / steps, y = a.y + (b.y - a.y) * k / steps;
        if (x >= g.x0 && x <= g.x1 && y >= g.y0 && y <= g.y1) return true;
    }
    return false;
}

// A dashed outline just inside `r`, black and white, like the selection's frame.
void dashedFrame(Canvas& c, const IRect& f, const IRect& clip) {
    auto fill = [&](const IRect& r) { c.fill(r.intersected(clip), 0); };
    for (int x = f.x0; x < f.x1; x += 24) {
        fill({x, f.y0, std::min(x + 14, f.x1), f.y0 + 3});
        fill({x, f.y1 - 3, std::min(x + 14, f.x1), f.y1});
    }
    for (int y = f.y0; y < f.y1; y += 24) {
        fill({f.x0, y, f.x0 + 3, std::min(y + 14, f.y1)});
        fill({f.x1 - 3, y, f.x1, std::min(y + 14, f.y1)});
    }
}

// A text (or a math answer) with its top left at (x, y) and `size` points, as it
// would be read back from the file.
ForeignElement placedText(const ForeignElement& f, double x, double y, double size) {
    ForeignElement out = f.mathOp.empty() ? textElement(&f, f.text, x, y, size)
                                          : answerElement(f.mathOp, f.mathNumbers, f.text, x, y, size);
    out.mathShapes = f.mathShapes;
    return out;
}

// What the selection's moves and resizes do to strokes and texts.
Element movedElement(const Element& e, double dx, double dy) {
    if (const auto* s = std::get_if<Stroke>(&e)) return movedStroke(*s, dx, dy);
    const auto& f = std::get<ForeignElement>(e);
    return placedText(f, f.textX + dx, f.textY + dy, f.textSize);
}

Element scaledElement(const Element& e, const StrokePoint& anchor, double factor) {
    if (const auto* s = std::get_if<Stroke>(&e)) return scaledStroke(*s, anchor, factor);
    const auto& f = std::get<ForeignElement>(e);
    return placedText(f, anchor.x + (f.textX - anchor.x) * factor, anchor.y + (f.textY - anchor.y) * factor,
                      f.textSize * factor);
}

constexpr uint32_t kPenColor = 0x000000;
constexpr uint32_t kHighlighterColor = 0xffff00;  // e-ink shows gray; the desktop shows yellow

}  // namespace

Editor::Editor(int screenWidth, int screenHeight, Document doc, std::string path, EditorOptions opts)
    : opts_(std::move(opts)),
      canvas_(screenWidth, screenHeight),
      toolbar_(screenWidth, opts_.toolbarHeight, editorToolbar(opts_.fromList)),
      pageArea_{0, opts_.toolbarHeight, screenWidth, screenHeight},
      panel_(pageArea_),
      paperMenu_(toolbar_.buttonRect(Action::PageStyle), pageArea_),
      math_(pageArea_),
      keyboard_(screenWidth, screenHeight, panel_.tabRect().y1 + 12),  // at the top, clear of the tools tab
      doc_(std::move(doc)),
      path_(std::move(path)),
      sizes_(opts_.sizes),
      eraseWhole_(opts_.eraseWholeStrokes),
      shapes_(opts_.recognizeShapes),
      line_(opts_.penLine) {
    if (doc_.pages.empty()) {
        // New notebook: one page exactly the size of the drawing area.
        doc_.pages.push_back(makeBlankPage(pageArea_.width() * 72.0 / opts_.dpi,
                                           pageArea_.height() * 72.0 / opts_.dpi, opts_.pageStyle));
    }
    xf_ = fitPage(currentPage(), pageArea_);
    redrawAll();
}

size_t Editor::currentLayer() {
    Page& p = currentPage();
    if (p.layers.empty()) p.layers.emplace_back();
    return p.layers.size() - 1;  // new strokes go on the top layer, like Xournal++'s default
}

IRect Editor::drawingClip() const { return xf_.pageRect(doc_.pages[page_]).intersected(pageArea_); }

double Editor::toolWidth() const {
    if (inkTool_ == ToolKind::Highlighter) return kHighlighterWidths[static_cast<size_t>(toolSize(ToolKind::Highlighter))];
    return kPenWidths[static_cast<size_t>(toolSize(ToolKind::Pen))];
}

double Editor::eraserWidth() const { return kEraserWidths[static_cast<size_t>(toolSize(ToolKind::Eraser))]; }

double Editor::pointWidth(const PenSample& s) const {
    if (inkTool_ == ToolKind::Highlighter) return toolWidth();
    // Light touch -> 60% of the preset, medium (0.5) -> 100%, full pressure -> 140%.
    return toolWidth() * std::clamp(0.6 + 0.8 * s.pressure, 0.6, 1.4);
}

// ---- drawing to the canvas ----

ToolbarState Editor::toolbarState() const {
    ToolbarState s;
    if (flashAction_ != Action::None) s.active.push_back(flashAction_);
    // A change in the open paper menu becomes an undo step when the menu closes, which UNDO does.
    const Background& bg = doc_.pages[page_].background;
    const bool paperChanged = paperMenu_.open() && (bg.style != paperBefore_.style || bg.config != paperBefore_.config);
    if (undo_.empty() && !paperChanged) s.disabled.push_back(Action::Undo);
    if (redo_.empty() || paperChanged) s.disabled.push_back(Action::Redo);
    if (paperMenu_.open()) s.active.push_back(Action::PageStyle);
    if (pendingExit_ != Action::None) s.active.push_back(pendingExit_);
    if (bg.type != "solid" && doc_.pages.size() < 2) s.disabled.push_back(Action::PageStyle);  // nothing to offer
    s.labels = {{Action::Save, modified_ ? "SAVE*" : "SAVE"}};
    s.text = !status_.empty() ? status_ : std::to_string(page_ + 1) + "/" + std::to_string(doc_.pages.size());
    // The notebook's name: the file's, without the folder or the extension.
    std::string name = path_.substr(path_.rfind('/') == std::string::npos ? 0 : path_.rfind('/') + 1);
    for (const char* ext : {".xopp", ".xoj"})
        if (name.size() > std::strlen(ext) && name.compare(name.size() - std::strlen(ext), std::string::npos, ext) == 0)
            name.resize(name.size() - std::strlen(ext));
    s.title = name;
    s.titleOnly = titleOnly_;
    s.disabled.erase(std::remove(s.disabled.begin(), s.disabled.end(), flashAction_), s.disabled.end());
    return s;
}

// While the whole name shows, any tap on the bar goes back to the buttons.
Action Editor::toolbarHit(int x, int y) const { return titleOnly_ ? Action::Title : toolbar_.hit(x, y); }

void Editor::press(Action a, uint64_t now) {
    if (a == Action::None) return;
    const auto has = [](const std::vector<Action>& v, Action x) { return std::find(v.begin(), v.end(), x) != v.end(); };
    const bool toolbarButton = !toolbar_.buttonRect(a).empty();
    const bool wasDisabled = toolbarButton && has(toolbarState().disabled, a);
    perform(a, now);
    if (toolbarButton) {
        if (wasDisabled || has(toolbarState().active, a)) return;
        endFlash();
        flashAction_ = a;
        flashUntil_ = now + kFlashMs;
        updateToolbar();
        return;
    }
    // Elsewhere only the buttons that leave no other trace: their place, if they're still there.
    IRect r;
    switch (a) {
        case Action::ZoomOut:
        case Action::ZoomIn:
        case Action::ZoomFit: r = panel_.buttonRect(a); break;
        case Action::PaperMargin:
        case Action::PaperEnds: r = paperMenu_.buttonRect(a); break;
        case Action::Duplicate:
        case Action::MathSum:
        case Action::MathAverage:
        case Action::MathMin:
        case Action::MathMax:
        case Action::MathCount: r = selectionButton(a); break;
        default: return;
    }
    flash(r, now);
}

// Inverts a black-and-white button where it is, to be drawn again by endFlash().
void Editor::flash(const IRect& r, uint64_t now) {
    if (r.empty()) return;
    endFlash();
    const IRect c = r.intersected(canvas_.rect());
    for (int y = c.y0; y < c.y1; ++y) {
        uint8_t* row = canvas_.row(y);
        for (int x = c.x0; x < c.x1; ++x) row[x] = static_cast<uint8_t>(255 - row[x]);
    }
    damage_.push_back({c, Refresh::UI});
    flashRect_ = c;
    flashUntil_ = now + kFlashMs;
}

void Editor::endFlash() {
    flashUntil_ = 0;
    if (flashAction_ != Action::None) {
        flashAction_ = Action::None;
        updateToolbar();
    }
    if (!flashRect_.empty()) {
        drawChrome(flashRect_);  // the button is drawn whole again, over itself
        damage_.push_back({flashRect_, Refresh::UI});
        flashRect_ = {};
    }
}

ToolPanelState Editor::panelState() const {
    static constexpr Action tools[] = {Action::Pen,  Action::Highlighter, Action::Eraser,
                                       Action::Text, Action::Lasso,       Action::Pan};
    ToolPanelState s;
    s.tool = tools[static_cast<int>(tool_)];
    s.shapes = shapes_;
    s.eraseWhole = eraseWhole_;
    s.line = line_;
    if (tool_ != ToolKind::Lasso && tool_ != ToolKind::Pan) {
        // For TEXT, the font sizes.
        const double* widths = tool_ == ToolKind::Pen           ? kPenWidths
                               : tool_ == ToolKind::Highlighter ? kHighlighterWidths
                               : tool_ == ToolKind::Text        ? kTextSizes
                                                                : kEraserWidths;
        s.size = toolSize(tool_);
        for (size_t i = 0; i < 3; ++i) s.widthsPx[i] = widths[i] * xf_.scale;
    }
    return s;
}

void Editor::redrawAll(Refresh mode) {
    toolbar_.draw(canvas_, toolbarState());
    canvas_.fill(pageArea_, kOffPageGray);
    drawPage(canvas_, currentPage(), xf_, pageArea_, opts_.render);
    drawChrome(pageArea_);
    damage_.clear();
    damage_.push_back({canvas_.rect(), mode});
}

void Editor::repaint(const IRect& r, Refresh mode) {
    const IRect area = r.intersected(pageArea_);
    if (area.empty()) return;
    canvas_.fill(area, kOffPageGray);
    drawPage(canvas_, currentPage(), xf_, area, opts_.render);
    drawChrome(area);
    damage_.push_back({area, mode});
}

// What goes over the page: the selection's frame and buttons, the text being typed
// and the keyboard, the tools panel, the paper menu.
void Editor::drawChrome(const IRect& clip) {
    if (sel_.active) drawOverlay(clip);
    drawAnswerButtons(clip);
    drawTextEdit(clip);
    keyboard_.draw(canvas_, clip);
    panel_.draw(canvas_, panelState(), clip);
    paperMenu_.draw(canvas_, clip);
    math_.draw(canvas_, clip);
}

// Shows the panel's new state. Covering more of the page (or changing in place)
// only draws black and white, so it goes out with DU; whatever it uncovers is
// repainted from the document and needs the grayscale waveform.
void Editor::refreshPanel(const std::vector<IRect>& before) {
    const std::vector<IRect> after = panel_.rects();
    IRect uncovered;
    for (const IRect& r : before) {
        bool covered = false;
        for (const IRect& n : after) covered = covered || n.contains(r);
        if (!covered) uncovered = uncovered.united(r);
    }
    repaint(uncovered, Refresh::Content);
    for (const IRect& r : after) {
        if (uncovered.contains(r)) continue;  // drawn just now
        panel_.draw(canvas_, panelState(), r);
        damage_.push_back({r, Refresh::UI});
    }
}

void Editor::closePanel() {
    closePanelOnUp_ = false;
    if (!panel_.open()) return;
    const std::vector<IRect> before = panel_.rects();
    panel_.setOpen(false);
    refreshPanel(before);
}

// Ink and the lasso's dots are drawn straight onto the canvas: the panel and the
// paper menu stay on top.
void Editor::keepPanelOnTop(const IRect& r) {
    for (const IRect& p : panel_.rects()) {
        if (p.intersected(r).empty()) continue;
        panel_.draw(canvas_, panelState(), r);
        break;
    }
    paperMenu_.draw(canvas_, r);
}

PaperMenuState Editor::paperMenuState() const {
    const Background& bg = doc_.pages[page_].background;
    // A PDF or image page keeps its background.
    return {paperOf(bg), bg.type == "solid", doc_.pages.size() > 1, deleteArmed_};
}

void Editor::openPaperMenu() {
    const PaperMenuState s = paperMenuState();
    if (!s.paperChoices && !s.canDelete) return;
    closePanel();
    paperBefore_ = currentPage().background;
    paperMenu_.show(s);
    paperMenu_.draw(canvas_, pageArea_);
    damage_.push_back({paperMenu_.rect(), Refresh::UI});
    updateToolbar();
}

// Changes the page's paper right away, so it can be seen behind the menu.
void Editor::changePaper(Action a, uint64_t now) {
    if (!paperMenu_.open() || currentPage().background.type != "solid") return;
    Background& bg = currentPage().background;
    Paper p = paperOf(bg).value_or(Paper{});
    const PaperKind kind = a == Action::PaperBlank   ? PaperKind::Blank
                           : a == Action::PaperRuled ? PaperKind::Ruled
                                                     : PaperKind::Grid;
    switch (a) {
        case Action::PaperBlank:
        case Action::PaperRuled:
        case Action::PaperGrid:
            if (p.kind == kind) return;
            if (kind == PaperKind::Ruled && p.kind != PaperKind::Ruled) p.margin = true;
            if (p.spacing < 0) p.spacing = 1;  // coming from blank, or from a spacing of the other kind's
            p.kind = kind;
            break;
        case Action::PaperNarrow: p.spacing = 0; break;
        case Action::PaperMedium: p.spacing = 1; break;
        case Action::PaperWide: p.spacing = 2; break;
        case Action::PaperMargin: p.margin = !p.margin; break;
        case Action::PaperEnds: p.ends = !p.ends; break;
        default: return;
    }
    const Background before = bg;
    setPaper(bg, p);
    if (bg.style == before.style && bg.config == before.config) return;
    markModified(now);
    deleteArmed_ = false;
    paperMenu_.show(paperMenuState());
    repaint(pageArea_, Refresh::Content);  // with the menu on top
    updateToolbar();                       // UNDO is available now
}

void Editor::closePaperMenu() {
    closePanelOnUp_ = false;
    deleteArmed_ = false;
    if (!paperMenu_.open()) return;
    const IRect area = paperMenu_.rect();
    paperMenu_.close();
    // All the changes made while it was open are one undo step.
    const Background& bg = currentPage().background;
    if (bg.style != paperBefore_.style || bg.config != paperBefore_.config) {
        UndoAction u{UndoAction::Kind::Background, page_, 0, {}, std::nullopt};
        u.background = paperBefore_;
        pushUndo(std::move(u));
    }
    repaint(area, Refresh::Content);
    updateToolbar();
}

// The first tap only arms DELETE PAGE; the second deletes the page, and one UNDO
// brings it back.
void Editor::deletePage(uint64_t now) {
    if (!paperMenu_.open() || doc_.pages.size() < 2) return;
    if (!deleteArmed_) {
        deleteArmed_ = true;
        paperMenu_.show(paperMenuState());
        paperMenu_.draw(canvas_, pageArea_);
        damage_.push_back({paperMenu_.rect(), Refresh::UI});
        return;
    }
    closePaperMenu();  // a paper change made in this visit becomes its own undo step first
    UndoAction u{UndoAction::Kind::DeletePage, page_, 0, {}, std::move(doc_.pages[page_])};
    doc_.pages.erase(doc_.pages.begin() + static_cast<std::ptrdiff_t>(page_));
    pushUndo(std::move(u));
    markModified(now);
    setPage(std::min(page_, doc_.pages.size() - 1));
    setStatus("DELETED", now);
}

void Editor::updateToolbar() {
    toolbar_.draw(canvas_, toolbarState());
    damage_.push_back({toolbar_.rect(), Refresh::UI});
}

std::vector<Damage> Editor::takeDamage() {
    std::vector<Damage> out;
    for (const Damage& d : damage_) {
        if (d.mode == Refresh::Full) {
            out = {d};
            damage_.clear();
            return out;
        }
    }
    // One rectangle per refresh mode is plenty: changes within a batch are local.
    // Content goes before UI: the UI rectangle (toolbar and tools panel together) can
    // span page pixels that the Content one repaints, and DU reaching them first would
    // leave them stale (see Refresh). After the Content refresh they are unchanged, so
    // DU passes over them. Ink goes first, for speed, unless Content repaints some of
    // it too (typing, then DONE): then for the same reason it waits.
    auto unite = [&](Refresh m) {
        IRect u;
        for (const Damage& d : damage_)
            if (d.mode == m) u = u.united(d.rect);
        return u;
    };
    const IRect content = unite(Refresh::Content);
    const bool overlap = !content.intersected(unite(Refresh::Ink)).empty() ||
                         !content.intersected(unite(Refresh::Preview)).empty();
    const std::vector<Refresh> order = overlap
                                           ? std::vector<Refresh>{Refresh::Content, Refresh::Ink, Refresh::Preview, Refresh::UI}
                                           : std::vector<Refresh>{Refresh::Ink, Refresh::Preview, Refresh::Content, Refresh::UI};
    for (Refresh m : order) {
        const IRect u = unite(m);
        if (!u.empty()) out.push_back({u, m});
    }
    damage_.clear();
    return out;
}

// ---- input ----

void Editor::penActivity(uint64_t now) {
    if (settleAt_) settleAt_ = now + opts_.settleDelayMs;
}

void Editor::scheduleSettle(const IRect& r, uint64_t now) {
    settle_ = settle_.united(r);
    settleAt_ = now + opts_.settleDelayMs;
}

void Editor::penDown(const PenSample& s, uint64_t now) {
    if (gesture_ != Gesture::None) penUp(now);  // missed an "up"; close the old gesture first
    penActivity(now);

    const int x = static_cast<int>(s.x), y = static_cast<int>(s.y);
    if (toolbar_.rect().contains(x, y)) {
        gesture_ = Gesture::Toolbar;
        press(toolbarHit(x, y), now);
        return;
    }
    if (panel_.covers(x, y) || paperMenu_.covers(x, y)) {
        gesture_ = Gesture::Toolbar;
        press(panel_.covers(x, y) ? panel_.hit(x, y) : paperMenu_.hit(x, y), now);
        return;
    }
    if (keyboard_.covers(x, y)) {
        gesture_ = Gesture::Toolbar;
        return keyPress(x, y, now);
    }
    if (math_.open()) {  // its buttons, or a tap elsewhere closes it (and draws nothing)
        gesture_ = Gesture::Toolbar;
        if (math_.covers(x, y)) mathTap(x, y, now);
        else closeMath();
        return;
    }
    if (viewGesture_ != ViewGesture::None) {  // the pen takes over from the fingers
        viewGesture_ = ViewGesture::None;
        endViewGesture(now);
    }
    const bool erasing = s.eraser || (tool_ == ToolKind::Eraser && !s.button);
    // While typing, the pen puts the caret in the text, or drags it; elsewhere it's
    // done with it, and with the TEXT tool starts another or picks up one that's there
    // (the keyboard stays up).
    if (text_.active) {
        if (!erasing && textFrame().contains(x, y)) return startMove(text_.index, s, true);
        bool another = tool_ == ToolKind::Text && !s.button && !erasing && drawingClip().contains(x, y);
        for (size_t i : answers()) another = another && !answerArea(i).grown(8).contains(x, y);
        finishText(now, another);
        if (another) {
            if (const int i = textAt(x, y); i >= 0) return startMove(static_cast<size_t>(i), s, true);
            gesture_ = Gesture::Toolbar;
            return startText(x, y, now);
        }
    }
    // A math answer: its i shows what's behind it; anywhere else on it, the pen drags
    // it (the eraser erases it). A typed text too, with any tool but the eraser; with
    // TEXT, a tap types in it. In the selection, or on its buttons, the selection's
    // the one that moves.
    bool onSelection = sel_.active && (selectionFrame().contains(x, y) || selectionHandle().grown(16).contains(x, y));
    for (const OverlayButton& b : overlayButtons_) onSelection = onSelection || (sel_.active && b.r.contains(x, y));
    for (size_t i : answers()) {
        if (answerInfoButton(i).grown(8).contains(x, y)) {
            gesture_ = Gesture::Toolbar;
            return openAnswer(i);
        }
        if (!erasing && !onSelection && answerRect(i).contains(x, y)) return startMove(i, s, false);
    }
    if (const int i = textAt(x, y); i >= 0 && !erasing && !onSelection)
        return startMove(static_cast<size_t>(i), s, tool_ == ToolKind::Text);
    // Writing beside the open panel or menu puts it away, once the pen lifts:
    // repainting what it covers needs the slow waveform, which mustn't delay the ink.
    closePanelOnUp_ = panel_.open() || paperMenu_.open();
    if (tool_ == ToolKind::Pan && !s.eraser && !s.button) {
        if (!pageArea_.contains(x, y)) return;
        gesture_ = Gesture::Pan;
        beginViewGesture();
        viewAnchor_ = {xf_.toPageX(s.x), xf_.toPageY(s.y)};
        return;
    }
    if (sel_.active) {
        for (const OverlayButton& b : overlayButtons_) {
            if (!b.r.contains(x, y)) continue;
            gesture_ = Gesture::Toolbar;
            press(b.action, now);
            return;
        }
        const StrokePoint p{xf_.toPageX(s.x), xf_.toPageY(s.y)};
        const bool tip = !s.eraser && !s.button;
        if (tip && selectionHandle().grown(16).contains(x, y)) return startDrag(DragKind::Resize, p);
        if (tip && selectionFrame().contains(x, y)) return startDrag(DragKind::Move, p);
        clearSelection();  // tapping elsewhere lets go of it
    }
    if (!drawingClip().contains(x, y)) return;

    if (tool_ == ToolKind::Text && !s.eraser && !s.button) {
        gesture_ = Gesture::Toolbar;
        return startText(x, y, now);
    }
    if (tool_ == ToolKind::Lasso && !s.eraser && !s.button) {
        gesture_ = Gesture::Lasso;
        lasso_ = {{xf_.toPageX(s.x), xf_.toPageY(s.y)}};
        lassoArea_ = {};
        lassoDots_ = 0;
        return;
    }
    if (s.eraser || (tool_ == ToolKind::Eraser && !s.button)) {
        gesture_ = Gesture::Erase;
        pendingErase_ = UndoAction{UndoAction::Kind::Edit, page_, currentLayer(), {}, std::nullopt};
        eraserLast_.reset();
        chain_.valid = false;
        eraseAt(s, now);
        return;
    }

    gesture_ = Gesture::Draw;
    live_ = Stroke{};
    inkTool_ = s.button ? ToolKind::Highlighter : tool_;
    const bool hl = inkTool_ == ToolKind::Highlighter;
    live_.tool = hl ? StrokeTool::Highlighter : StrokeTool::Pen;
    live_.color = hl ? kHighlighterColor : kPenColor;
    live_.width = toolWidth();
    if (!hl) live_.dashes = linePattern(line_, live_.width);
    live_.points = {{xf_.toPageX(s.x), xf_.toPageY(s.y)}};
    livePointWidths_ = {pointWidth(s)};

    // Show a dot right away, before the pen moves.
    Stroke dot = live_;
    dot.points.push_back(dot.points[0]);
    if (!hl) dot.segmentWidths = {livePointWidths_[0]};
    const IRect drawn = drawLiveSegment(canvas_, dot, 0, xf_, drawingClip(), opts_.render);
    keepPanelOnTop(drawn);
    damage_.push_back({drawn, Refresh::Ink});
}

void Editor::penMove(const PenSample& s, uint64_t now) {
    penActivity(now);
    if (gesture_ == Gesture::Erase) {
        eraseAt(s, now);
        return;
    }
    if (gesture_ == Gesture::Pan) {
        moveViewTo(zoom_, viewAnchor_, s.x, s.y);
        if (now >= lastViewFrame_ + kViewFrameMs) viewFrame(now);
        return;
    }
    const StrokePoint p{xf_.toPageX(s.x), xf_.toPageY(s.y)};
    if (gesture_ == Gesture::Lasso) return lassoTo(p);
    if (gesture_ == Gesture::Drag || gesture_ == Gesture::MoveText) {
        if (gesture_ == Gesture::Drag) dragTo(p);
        else dragTextTo(p);
        if (now >= lastDragFrame_ + kDragFrameMs) dragFrame(now);
        return;
    }
    if (gesture_ != Gesture::Draw) return;

    const StrokePoint& last = live_.points.back();
    const double dx = (p.x - last.x) * xf_.scale, dy = (p.y - last.y) * xf_.scale;
    if (dx * dx + dy * dy < kMinPointDistPx * kMinPointDistPx) return;

    live_.points.push_back(p);
    livePointWidths_.push_back(pointWidth(s));
    const size_t i = live_.points.size() - 2;
    if (live_.tool == StrokeTool::Pen) live_.segmentWidths.push_back((livePointWidths_[i] + livePointWidths_[i + 1]) / 2);
    const IRect drawn = drawLiveSegment(canvas_, live_, i, xf_, drawingClip(), opts_.render);
    keepPanelOnTop(drawn);
    damage_.push_back({drawn, Refresh::Ink});
}

void Editor::penUp(uint64_t now) {
    penActivity(now);
    if (gesture_ == Gesture::Draw) {
        commitStroke(now);
    } else if (gesture_ == Gesture::Lasso) {
        finishLasso();
    } else if (gesture_ == Gesture::Drag) {
        finishDrag(now);
    } else if (gesture_ == Gesture::Pan) {
        endViewGesture(now);
    } else if (gesture_ == Gesture::MoveText) {
        finishMove(now);
    } else if (gesture_ == Gesture::Erase && !pendingErase_.edits.empty()) {
        pushUndo(std::move(pendingErase_));
        markModified(now);
    }
    pendingErase_ = UndoAction{};
    gesture_ = Gesture::None;
    if (closePanelOnUp_) {
        closePanel();
        closePaperMenu();
    }
}

void Editor::touchDown(int x, int y, uint64_t now) {
    touchDown(kTapId, x, y, now);
    touchUp(kTapId, now);
}

void Editor::touchDown(int id, int x, int y, uint64_t now) {
    if (gesture_ != Gesture::None) return;  // the pen is busy; this is a palm
    if (fingers_.empty() && viewGesture_ == ViewGesture::None) {  // buttons answer the first finger only
        if (toolbar_.rect().contains(x, y)) return press(toolbarHit(x, y), now);
        if (panel_.covers(x, y)) return press(panel_.hit(x, y), now);
        if (paperMenu_.covers(x, y)) return press(paperMenu_.hit(x, y), now);
        if (keyboard_.covers(x, y)) return keyPress(x, y, now);
        if (text_.active && textFrame().contains(x, y)) return caretTo(x, y, now);
        if (math_.open()) return math_.covers(x, y) ? mathTap(x, y, now) : closeMath();
        for (size_t i : answers())
            if (answerInfoButton(i).grown(8).contains(x, y)) return openAnswer(i);
        for (const OverlayButton& b : overlayButtons_)
            if (sel_.active && b.r.contains(x, y)) return press(b.action, now);
        // Fingers don't draw, so a tap elsewhere puts the tools or the menu away.
        closePanel();
        closePaperMenu();
    }
    if (penNear_ || !pageArea_.contains(x, y)) return;  // a palm, most likely
    fingers_.push_back({id, double(x), double(y)});
    if (palm_) return;
    if (fingers_.size() > 2) return rejectPalm("three or more fingers");
    if (fingers_.size() == 2) {
        // Pinch: the page point between the fingers stays between them.
        if (viewGesture_ == ViewGesture::None) beginViewGesture();
        viewGesture_ = ViewGesture::Pinch;
        const Finger &a = fingers_[0], &b = fingers_[1];
        pinchStartZoom_ = zoom_;
        pinchStartDist_ = std::max(1.0, std::hypot(a.x - b.x, a.y - b.y));
        startX_ = (a.x + b.x) / 2;
        startY_ = (a.y + b.y) / 2;
        viewAnchor_ = {xf_.toPageX(startX_), xf_.toPageY(startY_)};
        viewCommitted_ = false;
    } else if (fingers_.size() == 1 && tool_ == ToolKind::Pan) {
        beginViewGesture();
        viewGesture_ = ViewGesture::Pan;
        startX_ = x;
        startY_ = y;
        viewAnchor_ = {xf_.toPageX(x), xf_.toPageY(y)};
        viewCommitted_ = false;
    }
}

void Editor::setPenNear(bool near) {
    penNear_ = near;
    if (near && viewGesture_ != ViewGesture::None && !fingers_.empty()) rejectPalm("the pen came near");
}

// The fingers turned out to be a hand resting on the page: put the view back as it
// was, and ignore them until they lift.
void Editor::rejectPalm(const char* why) {
    palm_ = true;
    if (viewGesture_ == ViewGesture::None) return;
    viewGesture_ = ViewGesture::None;
    viewDirty_ = false;
    const bool shown = viewFrames_ > 0;
    setView(viewStart_, zoomStart_);
    if (shown) redrawAll(Refresh::Full);  // the previews went out: clear them
    std::fprintf(stderr, "scrible: touch taken for a palm (%s)%s\n", why, shown ? " after it began to zoom" : "");
}

void Editor::touchMove(int id, int x, int y, uint64_t now) {
    auto f = std::find_if(fingers_.begin(), fingers_.end(), [&](const Finger& g) { return g.id == id; });
    if (f == fingers_.end()) return;
    f->x = x;
    f->y = y;
    if (viewGesture_ == ViewGesture::Pinch && fingers_.size() >= 2) {
        const Finger &a = fingers_[0], &b = fingers_[1];
        const double dist = std::hypot(a.x - b.x, a.y - b.y);
        const double mx = (a.x + b.x) / 2, my = (a.y + b.y) / 2;
        if (!viewCommitted_ && std::fabs(dist - pinchStartDist_) < kFingerSlop &&
            std::hypot(mx - startX_, my - startY_) < kFingerSlop)
            return;
        viewCommitted_ = true;
        moveViewTo(pinchStartZoom_ * dist / pinchStartDist_, viewAnchor_, mx, my);
    } else if (viewGesture_ == ViewGesture::Pan) {
        if (!viewCommitted_ && std::hypot(x - startX_, y - startY_) < kFingerSlop) return;
        viewCommitted_ = true;
        moveViewTo(zoom_, viewAnchor_, x, y);
    } else {
        return;
    }
    if (now >= lastViewFrame_ + kViewFrameMs) viewFrame(now);
}

void Editor::touchUp(int id, uint64_t now) {
    fingers_.erase(std::remove_if(fingers_.begin(), fingers_.end(), [&](const Finger& g) { return g.id == id; }),
                   fingers_.end());
    if (fingers_.empty()) palm_ = false;
    const bool over = (viewGesture_ == ViewGesture::Pinch && fingers_.size() < 2) ||
                      (viewGesture_ == ViewGesture::Pan && fingers_.empty());
    if (!over) return;
    viewGesture_ = ViewGesture::None;
    endViewGesture(now);
}

// ---- zoom and pan ----

// Scaled `zoom` times from fitting the page in the drawing area, and moved no
// further than the page's edges (centered where the page is smaller than the area).
PageTransform Editor::clampedView(double zoom, double offsetX, double offsetY) const {
    const Page& p = doc_.pages[page_];
    PageTransform t;
    t.scale = fitPage(p, pageArea_).scale * zoom;
    auto axis = [](double offset, double length, int a0, int a1) {
        if (length <= a1 - a0) return a0 + (a1 - a0 - length) / 2;
        return std::clamp(offset, a1 - length, double(a0));
    };
    t.offsetX = axis(offsetX, p.width * t.scale, pageArea_.x0, pageArea_.x1);
    t.offsetY = axis(offsetY, p.height * t.scale, pageArea_.y0, pageArea_.y1);
    return t;
}

void Editor::setView(const PageTransform& t, double zoom) {
    xf_ = t;
    zoom_ = zoom;
    if (sel_.active) layoutOverlay();
}

void Editor::moveViewTo(double zoom, const StrokePoint& anchor, double x, double y) {
    const double z = std::clamp(zoom, 1.0, kMaxZoom);
    const double scale = fitPage(currentPage(), pageArea_).scale * z;
    const PageTransform t = clampedView(z, x - anchor.x * scale, y - anchor.y * scale);
    if (sameView(t, xf_)) return;
    setView(t, z);
    viewDirty_ = true;
}

// The zoom buttons and the mouse wheel: straight to a proper redraw.
void Editor::zoomTo(double zoom, double x, double y, uint64_t now) {
    const PageTransform before = xf_;
    moveViewTo(zoom, {xf_.toPageX(x), xf_.toPageY(y)}, x, y);
    viewDirty_ = false;
    if (sameView(before, xf_)) return;
    repaint(pageArea_, Refresh::Content);
    setStatus(std::to_string(std::lround(zoom_ * 100)) + "%", now);
}

void Editor::zoomBy(double factor, double x, double y, uint64_t now) { zoomTo(zoom_ * factor, x, y, now); }

void Editor::panBy(double dx, double dy, uint64_t now) {
    (void)now;
    const PageTransform t = clampedView(zoom_, xf_.offsetX + dx, xf_.offsetY + dy);
    if (sameView(t, xf_)) return;
    setView(t, zoom_);
    repaint(pageArea_, Refresh::Content);
}

void Editor::beginViewGesture() {
    closePanel();
    closePaperMenu();
    viewStart_ = xf_;
    zoomStart_ = zoom_;
    viewCommitted_ = true;  // the pen pans right away; fingers set it back
    viewDirty_ = false;
    viewFrames_ = 0;
    slowestViewFrameMs_ = 0;
    lastViewFrame_ = 0;
}

// A preview: the page drawn where it is now, in black and white (rule lines and
// highlights vanish for the moment) for the fast waveforms.
void Editor::viewFrame(uint64_t now) {
    if (!viewDirty_) return;
    const auto start = std::chrono::steady_clock::now();
    const IRect a = pageArea_;
    canvas_.fill(a, kOffPageGray);
    drawPage(canvas_, currentPage(), xf_, a, opts_.render);
    for (int y = a.y0; y < a.y1; ++y) {
        uint8_t* row = canvas_.row(y);
        for (int x = a.x0; x < a.x1; ++x) row[x] = row[x] < 128 ? 0 : 255;
    }
    drawChrome(a);
    damage_.push_back({a, Refresh::Preview});
    viewDirty_ = false;
    lastViewFrame_ = now;
    ++viewFrames_;
    slowestViewFrameMs_ = std::max(slowestViewFrameMs_, msSince(start));
}

void Editor::endViewGesture(uint64_t now) {
    const bool changed = viewFrames_ > 0 || viewDirty_;
    viewDirty_ = false;
    if (!changed) return;
    status_ = std::to_string(std::lround(zoom_ * 100)) + "%";
    statusUntil_ = now + 1500;
    const auto start = std::chrono::steady_clock::now();
    redrawAll(Refresh::Full);  // with a flash, which clears what the quick previews leave behind
    std::fprintf(stderr, "scrible: zoom %ld%%: %d preview frames (slowest %.0f ms), page redrawn in %.0f ms\n",
                 std::lround(zoom_ * 100), viewFrames_, slowestViewFrameMs_, msSince(start));
}

void Editor::commitStroke(uint64_t now) {
    if (live_.points.size() == 1) {
        live_.points.push_back(live_.points[0]);
        if (live_.tool == StrokeTool::Pen) live_.segmentWidths = {livePointWidths_[0]};
    }
    const size_t layer = currentLayer();
    auto& elements = currentPage().layers[layer].elements;
    const RectF bounds = live_.bounds();
    const bool pen = live_.tool == StrokeTool::Pen;
    elements.emplace_back(std::move(live_));
    live_ = Stroke{};
    pushUndo({UndoAction::Kind::Edit, page_, layer, {{true, elements.size() - 1, Stroke{}}}, std::nullopt});
    markModified(now);
    scheduleSettle(xf_.toScreen(bounds), now);
    if (shapes_ && pen) recognize(layer);
    else chain_.valid = false;
}

// Replaces the stroke just drawn (and, for polygons and arrows, the lines drawn
// right before it) with the clean shape. It's a separate undo step, so the first
// undo brings back what was drawn by hand.
void Editor::recognize(size_t layer) {
    auto& els = currentPage().layers[layer].elements;
    const size_t last = els.size() - 1;
    const Stroke drawn = std::get<Stroke>(els[last]);
    const bool continuing = chain_.valid && chain_.page == page_ && chain_.layer == layer && chain_.last + 1 == last;

    Shape shape = recognizeStroke(drawn.points);
    size_t replaced = 1;
    if (shape.kind == ShapeKind::Line) {
        ShapeChain chain = continuing ? chain_ : ShapeChain{true, page_, layer, last, 0, {}};
        Shape closed;
        if (!extendChain(chain.points, shape.points[0], shape.points[1], closed)) {
            chain.points.clear();  // doesn't connect: this line starts a new chain
            chain.lines = 0;
            extendChain(chain.points, shape.points[0], shape.points[1], closed);
        }
        chain.lines++;
        chain.last = last;
        if (closed.kind != ShapeKind::None) {
            shape = closed;
            replaced = chain.lines;
            chain.valid = false;
        }
        chain_ = chain;
    } else {
        Shape arrow;
        if (shape.kind == ShapeKind::None && continuing && chain_.lines == 1 &&
            makeArrow(chain_.points.front(), chain_.points.back(), drawn.points, arrow)) {
            shape = arrow;
            replaced = 2;
        }
        chain_.valid = false;
    }
    if (shape.kind == ShapeKind::None) return;

    // The hand-drawn widths vary with pressure; the clean shape gets their average.
    double widthSum = 0, segments = 0;
    for (size_t k = els.size() - replaced; k < els.size(); ++k) {
        const Stroke& s = std::get<Stroke>(els[k]);
        for (size_t i = 0; i + 1 < s.points.size(); ++i, ++segments) widthSum += s.segmentWidth(i);
    }
    Stroke clean = drawn;
    clean.points = shape.points;
    clean.segmentWidths.clear();
    if (segments > 0) clean.width = widthSum / segments;
    clean.invalidateBounds();

    UndoAction a{UndoAction::Kind::Edit, page_, layer, {}, std::nullopt};
    RectF changed;
    auto unite = [&](const RectF& b) {
        changed.add(b.x0, b.y0);
        changed.add(b.x1, b.y1);
    };
    for (size_t k = 0; k < replaced; ++k) {
        unite(elementBounds(els.back()));
        a.edits.push_back({false, els.size() - 1, std::move(els.back())});
        els.pop_back();
    }
    unite(clean.bounds());
    els.push_back(std::move(clean));
    a.edits.push_back({true, els.size() - 1, Stroke{}});
    pushUndo(std::move(a));
    // Ink disappears over gray rule lines, so not DU (see Refresh).
    repaint(xf_.toScreen(changed), Refresh::Content);
}

void Editor::eraseAt(const PenSample& s, uint64_t now) {
    // The eraser sweeps from its previous position, so a fast swipe leaves no gaps.
    const StrokePoint to{xf_.toPageX(s.x), xf_.toPageY(s.y)};
    const StrokePoint from = eraserLast_.value_or(to);
    eraserLast_ = to;
    const double r = eraserWidth() / 2;

    auto& elements = currentPage().layers[pendingErase_.layer].elements;
    RectF changed;
    IRect screen;  // also changing: the i of answers that go
    std::vector<Stroke> pieces;
    for (size_t i = 0; i < elements.size();) {
        // A typed text, or a math answer, goes as a whole as soon as the eraser touches it.
        if (const auto* f = std::get_if<ForeignElement>(&elements[i]); f && f->tag == "text") {
            if (pendingErase_.layer + 1 != currentPage().layers.size() || !sweepTouches(f->bounds, from, to, r)) {
                ++i;
                continue;
            }
            screen = screen.united(f->mathOp.empty() ? xf_.toScreen(f->bounds).grown(2) : answerArea(i));
            pendingErase_.edits.push_back({false, i, std::move(elements[i])});
            elements.erase(elements.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        const auto* st = std::get_if<Stroke>(&elements[i]);
        if (!st || !eraseFromStroke(*st, from, to, r, pieces)) {
            ++i;
            continue;
        }
        RectF area;  // what changes on screen
        if (eraseWhole_) {
            area = st->bounds();
            pieces.clear();
        } else {
            // Only the swept area and the new end caps.
            area.add(from.x, from.y);
            area.add(to.x, to.y);
            area.grow(r + st->maxWidth());
        }
        changed.add(area.x0, area.y0);
        changed.add(area.x1, area.y1);

        pendingErase_.edits.push_back({false, i, std::move(elements[i])});
        elements.erase(elements.begin() + static_cast<std::ptrdiff_t>(i));
        for (Stroke& piece : pieces) {
            elements.insert(elements.begin() + static_cast<std::ptrdiff_t>(i), std::move(piece));
            pendingErase_.edits.push_back({true, i, Stroke{}});
            ++i;
        }
    }
    if (changed.valid) screen = screen.united(xf_.toScreen(changed));
    if (!screen.empty()) previewArea(screen, now);
}

void Editor::previewArea(const IRect& r, uint64_t now) {
    const IRect area = r.intersected(pageArea_);
    if (area.empty()) return;
    canvas_.fill(area, kOffPageGray);
    drawPage(canvas_, currentPage(), xf_, area, opts_.render);
    drawChrome(area);
    // Shown right away with fast DU, which can only draw black and white, so the
    // preview is strictly black and white: rule lines and highlights in the area
    // vanish for a moment. (Sending their real gray with DU would leave the old
    // black ink showing.) When the pen rests, the area is repainted for real and
    // refreshed with GC16; those pixels really change then, so they get redrawn.
    for (int y = area.y0; y < area.y1; ++y) {
        uint8_t* row = canvas_.row(y);
        for (int x = area.x0; x < area.x1; ++x) row[x] = row[x] < 128 ? 0 : 255;
    }
    damage_.push_back({area, Refresh::Ink});
    scheduleSettle(area, now);
}

// ---- lasso selection ----

void Editor::lassoTo(const StrokePoint& p) {
    const StrokePoint last = lasso_.back();
    const double ax = xf_.toScreenX(last.x), ay = xf_.toScreenY(last.y);
    const double bx = xf_.toScreenX(p.x), by = xf_.toScreenY(p.y);
    const double len = std::hypot(bx - ax, by - ay);
    if (len < 3) return;
    lasso_.push_back(p);
    // A dotted outline, so it doesn't look like ink.
    const IRect clip = drawingClip();
    IRect drawn;
    for (double d = 8 - lassoDots_; d <= len; d += 8) {
        const int x = static_cast<int>(ax + (bx - ax) * d / len), y = static_cast<int>(ay + (by - ay) * d / len);
        const IRect dot = IRect{x - 2, y - 2, x + 2, y + 2}.intersected(clip);
        canvas_.fill(dot, 0);
        drawn = drawn.united(dot);
    }
    lassoDots_ = std::fmod(lassoDots_ + len, 8.0);
    if (drawn.empty()) return;
    keepPanelOnTop(drawn);
    lassoArea_ = lassoArea_.united(drawn);
    damage_.push_back({drawn, Refresh::Ink});
}

void Editor::finishLasso() {
    std::vector<size_t> picked = elementsInLasso(currentPage().layers[currentLayer()].elements, lasso_);
    IRect area = lassoArea_;
    lasso_.clear();
    lassoArea_ = {};
    if (!picked.empty()) {
        selectionMenu_ = SelectionMenu::None;  // a new selection starts with just EDIT and MATH
        select(std::move(picked));
        area = area.united(overlayArea());
    }
    // The dotted outline goes, over gray rule lines too, so not DU.
    repaint(area, Refresh::Content);
}

void Editor::select(std::vector<size_t> indices) {
    sel_.active = true;
    sel_.layer = currentLayer();
    sel_.indices = std::move(indices);
    sel_.bounds = selectedBounds();
    layoutOverlay();
}

void Editor::clearSelection() {
    if (!sel_.active) return;
    const IRect area = overlayArea();
    sel_ = Selection{};
    overlayButtons_.clear();
    repaint(area, Refresh::Content);
}

RectF Editor::selectedBounds() const {
    RectF r;
    const auto& els = doc_.pages[page_].layers[sel_.layer].elements;
    for (size_t i : sel_.indices) {
        const RectF b = elementBounds(els[i]);
        if (!b.valid) continue;
        r.add(b.x0, b.y0);
        r.add(b.x1, b.y1);
    }
    return r;
}

IRect Editor::selectionFrame() const { return sel_.active ? xf_.toScreen(sel_.bounds).grown(kFramePad) : IRect{}; }

IRect Editor::selectionHandle() const {
    if (!sel_.active) return {};
    const IRect f = selectionFrame();
    return {f.x1 - kHandle, f.y1 - kHandle, f.x1 + kHandle, f.y1 + kHandle};
}

IRect Editor::selectionButton(Action a) const {
    for (const OverlayButton& b : overlayButtons_)
        if (b.action == a) return b.r;
    return {};
}

// The selection's menu, one row: EDIT and MATH, or, once one is picked, that one
// (with an up arrow, to go back) followed by its options. Above the frame if there's
// room, else below; on the page either way, and not under the tools tab. It stays in
// one place, on a white bar, so going into a menu (a longer row) is black and white
// over black and white.
void Editor::layoutOverlay() {
    overlayButtons_.clear();
    const IRect f = selectionFrame();
    using Row = std::vector<std::pair<Action, std::string>>;
    const Row menus = {{Action::EditMenu, "EDIT"}, {Action::MathMenu, "MATH"}};
    const Row edit = {{Action::EditMenu, "EDIT"}, {Action::Delete, "DELETE"}, {Action::Duplicate, "DUPLICATE"}};
    const Row math = {{Action::MathMenu, "MATH"}, {Action::MathSum, "SUM"},    {Action::MathAverage, "AVERAGE"},
                      {Action::MathMin, "MIN"},   {Action::MathMax, "MAX"},    {Action::MathCount, "COUNT"}};
    const int h = 96, gap = 12, arrow = 32;  // room for the up arrow
    auto buttonWidth = [&](const Row& row, size_t k) {
        const int w = Canvas::textWidth(row[k].second, kOverlayScale) + 48;
        return k == 0 && &row != &menus ? w + arrow : w;
    };
    int total = 0;
    for (const Row* row : {&menus, &edit, &math}) {
        int w = -gap;
        for (size_t k = 0; k < row->size(); ++k) w += buttonWidth(*row, k) + gap;
        total = std::max(total, w);
    }
    const Row& row = selectionMenu_ == SelectionMenu::Edit ? edit : selectionMenu_ == SelectionMenu::Math ? math : menus;
    int y0 = f.y0 - 20 - h;
    if (y0 < pageArea_.y0 + 10) y0 = std::min(f.y1 + kHandle + 20, pageArea_.y1 - 10 - h);
    const IRect tab = panel_.tabRect();
    const int minX = (y0 < tab.y1 && y0 + h > tab.y0 ? tab.x1 : pageArea_.x0) + 10;
    const int x0 = std::clamp(f.x0, minX, std::max(minX, pageArea_.x1 - 10 - total));
    int x = x0;
    for (size_t k = 0; k < row.size(); ++k) {
        const int w = buttonWidth(row, k);
        overlayButtons_.push_back({row[k].first, row[k].second, {x, y0, x + w, y0 + h}});
        x += w + gap;
    }
}

// Goes into one of the selection's menus, or back out of it.
void Editor::showSelectionMenu(SelectionMenu m) {
    if (!sel_.active) return;
    const IRect before = overlayArea();
    const bool wasClosed = selectionMenu_ == SelectionMenu::None;
    selectionMenu_ = selectionMenu_ == m ? SelectionMenu::None : m;
    layoutOverlay();
    const IRect after = overlayArea();
    // Into a menu: a longer row over the shorter one, black and white, so DU. Back
    // out: the shorter row uncovers the page, grays and all.
    if (!wasClosed) return repaint(before.united(after), Refresh::Content);
    drawOverlay(after);
    damage_.push_back({after, Refresh::UI});
}

// Reads the numbers in the selection, and puts the answer on the page just below it,
// about as tall as them. Handwriting goes through recognition; typed texts, earlier
// answers among them, are read as they are, each line a number (see typedNumber), and
// lines that don't start with one are left out. Its i shows what it was worked out from.
void Editor::openMath(Action op, uint64_t now) {
    if (!sel_.active) return;
    std::vector<Stroke> strokes;  // in the order they were written
    std::vector<ReadNumber> typed;
    const Font& font = Font::sans();
    const auto& els = currentPage().layers[sel_.layer].elements;
    for (size_t i : sel_.indices) {
        if (const auto* s = std::get_if<Stroke>(&els[i])) {
            if (s->tool == StrokeTool::Pen) strokes.push_back(*s);
            continue;
        }
        const ForeignElement& f = std::get<ForeignElement>(els[i]);
        if (f.tag != "text") continue;
        size_t row = 0;
        for (size_t start = 0; start <= f.text.size(); ++row) {
            const size_t end = std::min(f.text.find('\n', start), f.text.size());
            const std::string number = typedNumber(f.text.substr(start, end - start));
            start = end + 1;
            if (number.empty()) continue;
            // Each character's box is the digits' (0.73 em tall, on the baseline), as for handwriting.
            const double base = f.textY + (row * font.lineHeight() + font.ascent()) * f.textSize;
            RectF box;
            box.add(f.textX, base - 0.73 * f.textSize);
            box.add(f.textX + font.advance(number, 0, number.size()) * f.textSize, base);
            ReadNumber n;
            for (char c : number) n.chars.push_back({c, {}, box, {}});
            typed.push_back(std::move(n));
        }
    }
    const auto start = std::chrono::steady_clock::now();
    if (!digits_ && !strokes.empty()) {
        digits_ = std::make_unique<DigitReader>();
        if (!opts_.digitExamplesPath.empty()) digits_->loadExamples(opts_.digitExamplesPath);
    }
    std::vector<ReadNumber> numbers = strokes.empty() ? std::vector<ReadNumber>{} : readNumbers(strokes, *digits_);
    size_t chars = 0;
    for (const ReadNumber& n : numbers) chars += n.chars.size();
    std::fprintf(stderr, "scrible: read %zu numbers (%zu characters, %zu of your own examples) in %.0f ms, and %zu typed\n",
                 numbers.size(), chars, digits_ ? digits_->exampleCount() : 0,
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(),
                 typed.size());
    // The typed ones among the written ones, in reading order: before the first that's on a
    // lower line, or further right on the same line.
    auto box = [](const ReadNumber& n) {
        RectF r;
        for (const ReadChar& c : n.chars) {
            r.add(c.bounds.x0, c.bounds.y0);
            r.add(c.bounds.x1, c.bounds.y1);
        }
        return r;
    };
    for (ReadNumber& t : typed) {
        const RectF a = box(t);
        auto it = std::find_if(numbers.begin(), numbers.end(), [&](const ReadNumber& n) {
            const RectF b = box(n);
            const double gap = (b.y0 + b.y1) / 2 - (a.y0 + a.y1) / 2;
            const double row = 0.5 * std::min(a.y1 - a.y0, b.y1 - b.y0);
            return gap > row || (std::fabs(gap) <= row && b.x0 > a.x0);
        });
        numbers.insert(it, std::move(t));
    }
    const std::string text = MathPopup::answer(op, numbers);
    if (text.empty()) return setStatus("NO NUMBERS", now);

    std::vector<std::string> texts;
    std::vector<std::vector<std::array<uint8_t, 16>>> shapes;
    std::vector<double> sizes;  // what each digit suggests for the answer's font size
    for (const ReadNumber& n : numbers) {
        texts.push_back(n.text());
        shapes.emplace_back();
        for (const ReadChar& c : n.chars) {
            shapes.back().push_back(c.features);
            if (c.c < '0' || c.c > '9') continue;
            // Its digits (0.73 em tall) as tall as typed ones, a little smaller than written ones.
            const double h = c.bounds.y1 - c.bounds.y0;
            sizes.push_back(c.strokes.empty() ? h / 0.73 : 0.95 * h);
        }
    }
    std::sort(sizes.begin(), sizes.end());
    const double size = std::clamp(sizes.empty() ? 10.0 : sizes[sizes.size() / 2], 6.0, 60.0);
    const Page& page = currentPage();
    ForeignElement answer =
        answerElement(MathPopup::opKey(op), texts, text, sel_.bounds.x0, sel_.bounds.y1 + 0.4 * size, size);
    const double w = answer.bounds.x1 - answer.bounds.x0, h = answer.bounds.y1 - answer.bounds.y0;
    const double x = std::clamp(answer.textX, 0.0, std::max(0.0, page.width - w));
    const double y = std::clamp(answer.textY, 0.0, std::max(0.0, page.height - h));
    answer = answerElement(MathPopup::opKey(op), texts, text, x, y, size);
    answer.mathShapes = std::move(shapes);

    const size_t layer = currentLayer();
    auto& elements = currentPage().layers[layer].elements;
    elements.emplace_back(std::move(answer));
    pushUndo({UndoAction::Kind::Edit, page_, layer, {{true, elements.size() - 1, Stroke{}}}, std::nullopt});
    markModified(now);
    repaint(answerArea(elements.size() - 1), Refresh::Content);
}

std::vector<size_t> Editor::answers() const {
    std::vector<size_t> out;
    const Page& p = doc_.pages[page_];
    if (p.layers.empty()) return out;
    const auto& els = p.layers.back().elements;
    for (size_t i = 0; i < els.size(); ++i)
        if (const auto* f = std::get_if<ForeignElement>(&els[i]); f && !f->mathOp.empty()) out.push_back(i);
    return out;
}

const ForeignElement& Editor::answerAt(size_t i) const {
    return std::get<ForeignElement>(doc_.pages[page_].layers.back().elements[i]);
}

IRect Editor::answerRect(size_t i) const { return xf_.toScreen(answerAt(i).bounds); }

// A small box with an i, at the answer's top right.
IRect Editor::answerInfoButton(size_t i) const {
    const IRect r = answerRect(i);
    return {r.x1 + kInfoGap, r.y0, r.x1 + kInfoGap + kInfoSize, r.y0 + kInfoSize};
}

IRect Editor::answerArea(size_t i) const { return answerRect(i).united(answerInfoButton(i)).grown(2); }

void Editor::drawAnswerButtons(const IRect& clip) {
    for (size_t i : answers()) {
        const IRect b = answerInfoButton(i);
        if (b.intersected(clip).empty()) continue;
        canvas_.fill(b.intersected(clip), 0);
        canvas_.fill(b.grown(-3).intersected(clip), 255);
        canvas_.drawText(b.x0 + (b.width() - 24) / 2, b.y0 + (b.height() - 24) / 2, "i", 3, 0, b.intersected(clip));
    }
}

// The popup with the numbers behind an answer, ready for corrections.
void Editor::openAnswer(size_t i) {
    closeMath();
    const ForeignElement& a = answerAt(i);
    std::vector<ReadNumber> numbers;
    for (size_t k = 0; k < a.mathNumbers.size(); ++k) {
        ReadNumber n;
        for (size_t j = 0; j < a.mathNumbers[k].size(); ++j) {
            ReadChar c;
            c.c = a.mathNumbers[k][j];
            if (k < a.mathShapes.size() && j < a.mathShapes[k].size()) c.features = a.mathShapes[k][j];
            n.chars.push_back(c);
        }
        numbers.push_back(std::move(n));
    }
    answer_ = static_cast<int>(i);
    math_.show(MathPopup::opFromKey(a.mathOp), std::move(numbers));
    math_.draw(canvas_, pageArea_);
    damage_.push_back({math_.rect(), Refresh::UI});
}

void Editor::mathTap(int x, int y, uint64_t now) {
    const MathPopup::Tap t = math_.tap(x, y);
    if (t.close) return closeMath();
    if (t.remove) return deleteAnswer(now);
    if (t.taught) {
        // The correction is an example of the user's handwriting from now on.
        if (!digits_) {
            digits_ = std::make_unique<DigitReader>();
            if (!opts_.digitExamplesPath.empty()) digits_->loadExamples(opts_.digitExamplesPath);
        }
        digits_->addExample(t.features, t.digit);
        if (!opts_.digitExamplesPath.empty()) digits_->appendExample(opts_.digitExamplesPath, t.features, t.digit);
    }
    if (t.edited && answer_ >= 0) {
        // The answer on the page follows the corrected numbers.
        const ForeignElement& a = answerAt(static_cast<size_t>(answer_));
        std::vector<std::string> texts;
        std::vector<std::vector<std::array<uint8_t, 16>>> shapes;
        for (const ReadNumber& n : math_.numbers()) {
            texts.push_back(n.text());
            shapes.emplace_back();
            for (const ReadChar& c : n.chars) shapes.back().push_back(c.features);
        }
        std::string text = MathPopup::answer(MathPopup::opFromKey(a.mathOp), math_.numbers());
        if (text.empty()) text = "?";
        ForeignElement updated = answerElement(a.mathOp, texts, text, a.textX, a.textY, a.textSize);
        updated.mathShapes = std::move(shapes);
        replaceAnswer(static_cast<size_t>(answer_), std::move(updated), now);
    }
    if (!t.changed) return;
    math_.draw(canvas_, math_.rect());
    damage_.push_back({math_.rect(), Refresh::UI});
}

void Editor::closeMath() {
    answer_ = -1;
    if (!math_.open()) return;
    const IRect area = math_.rect();
    math_.close();
    repaint(area, Refresh::Content);
}

// One undo step: the answer as it was, then as it is.
void Editor::replaceAnswer(size_t i, ForeignElement a, uint64_t now) {
    const IRect before = answerArea(i);
    auto& els = currentPage().layers[currentLayer()].elements;
    Element old = std::move(els[i]);
    els[i] = std::move(a);
    pushUndo({UndoAction::Kind::Edit, page_, currentLayer(), {{false, i, std::move(old)}, {true, i, Stroke{}}},
              std::nullopt});
    markModified(now);
    repaint(before.united(answerArea(i)), Refresh::Content);
}

void Editor::deleteAnswer(uint64_t now) {
    if (answer_ < 0) return closeMath();
    const size_t i = static_cast<size_t>(answer_);
    const IRect area = answerArea(i);
    closeMath();
    auto& els = currentPage().layers[currentLayer()].elements;
    UndoAction u{UndoAction::Kind::Edit, page_, currentLayer(), {{false, i, std::move(els[i])}}, std::nullopt};
    els.erase(els.begin() + static_cast<std::ptrdiff_t>(i));
    pushUndo(std::move(u));
    markModified(now);
    repaint(area, Refresh::Content);
}

void Editor::startMove(size_t i, const PenSample& s, bool editOnTap) {
    gesture_ = Gesture::MoveText;
    dragText_ = i;
    textBefore_ = answerAt(i);
    dragStart_ = {xf_.toPageX(s.x), xf_.toPageY(s.y)};
    downX_ = s.x;
    downY_ = s.y;
    moved_ = false;
    editOnTap_ = editOnTap;
    dragDirty_ = {};
}

IRect Editor::movedArea(size_t i) const {
    if (text_.active && text_.index == i) return textArea();
    if (!answerAt(i).mathOp.empty()) return answerArea(i);
    return answerRect(i).grown(2);
}

// Moves the text or answer being dragged with the pen; the screen catches up in dragFrame().
void Editor::dragTextTo(const StrokePoint& p) {
    if (!moved_ && std::hypot(xf_.toScreenX(p.x) - downX_, xf_.toScreenY(p.y) - downY_) < kMoveSlop) return;
    moved_ = true;
    const IRect before = movedArea(dragText_);
    const ForeignElement& a = textBefore_;
    const Page& page = currentPage();
    const double dx = keepOnPage(p.x - dragStart_.x, a.bounds.x0, a.bounds.x1, page.width);
    const double dy = keepOnPage(p.y - dragStart_.y, a.bounds.y0, a.bounds.y1, page.height);
    currentPage().layers[currentLayer()].elements[dragText_] = placedText(a, a.textX + dx, a.textY + dy, a.textSize);
    dragDirty_ = dragDirty_.united(before).united(movedArea(dragText_));
}

// A move is one undo step, except of the text being typed, whose typing is the step.
// A tap that didn't move it types in it, with `editOnTap`.
void Editor::finishMove(uint64_t now) {
    dragFrame(now);
    const bool typingIt = text_.active && text_.index == dragText_;
    if (!moved_) {
        if (editOnTap_) {
            const int x = static_cast<int>(downX_), y = static_cast<int>(downY_);
            if (typingIt) caretTo(x, y, now);
            else startText(x, y, now);
        }
    } else if (typingIt) {
        markModified(now);
        placeKeyboard();
    } else if (answerAt(dragText_).textX != textBefore_.textX || answerAt(dragText_).textY != textBefore_.textY) {
        pushUndo({UndoAction::Kind::Edit, page_, currentLayer(),
                  {{false, dragText_, std::move(textBefore_)}, {true, dragText_, Stroke{}}}, std::nullopt});
        markModified(now);
    }
    if (!text_.active) closeKeyboard();  // kept up for a tap that turned out to be a drag
}

IRect Editor::overlayArea() const {
    if (!sel_.active) return {};
    IRect r = selectionFrame().united(selectionHandle());
    for (const OverlayButton& b : overlayButtons_) r = r.united(b.r);
    for (size_t i : sel_.indices)  // a math answer's i, beside it
        if (const auto* f = std::get_if<ForeignElement>(&doc_.pages[page_].layers[sel_.layer].elements[i]);
            f && !f->mathOp.empty())
            r = r.united(answerArea(i));
    return r.grown(2);
}

// The selection's dashed frame, resize handle and buttons: black and white only, so
// they can appear with DU.
void Editor::drawOverlay(const IRect& clip) {
    auto fill = [&](const IRect& r, uint8_t v) { canvas_.fill(r.intersected(clip), v); };
    dashedFrame(canvas_, selectionFrame(), clip);
    const IRect h = selectionHandle();
    fill(h, 0);
    fill(h.grown(-8), 255);
    fill(h.grown(-14), 0);
    if (!overlayButtons_.empty()) {
        // The menu's white bar, so the gaps between its buttons hide the page too.
        fill({overlayButtons_.front().r.x0, overlayButtons_.front().r.y0, overlayButtons_.back().r.x1,
              overlayButtons_.back().r.y1},
             255);
    }
    for (const OverlayButton& b : overlayButtons_) {
        // The menu that's open leads its row, inverted, with an arrow back up.
        const bool open = (b.action == Action::EditMenu && selectionMenu_ == SelectionMenu::Edit) ||
                          (b.action == Action::MathMenu && selectionMenu_ == SelectionMenu::Math);
        fill(b.r, 0);
        if (!open) fill(b.r.grown(-3), 255);
        const uint8_t ink = open ? 255 : 0;
        int tx = b.r.x0 + (b.r.width() - Canvas::textWidth(b.label, kOverlayScale)) / 2;
        const int ty = b.r.y0 + (b.r.height() - 8 * kOverlayScale) / 2;
        if (open) {
            const int ax = b.r.x0 + 34, top = ty;
            for (int j = 0; j < 14; ++j) fill({ax - j * 10 / 14, top + j, ax + j * 10 / 14 + 1, top + j + 1}, ink);
            fill({ax - 3, top + 14, ax + 4, top + 24}, ink);
            tx += 16;
        }
        canvas_.drawText(tx, ty, b.label, kOverlayScale, ink, b.r.intersected(clip));
    }
}

void Editor::startDrag(DragKind kind, const StrokePoint& p) {
    gesture_ = Gesture::Drag;
    dragKind_ = kind;
    dragStart_ = p;
    dragFrom_ = sel_.bounds;
    dragDirty_ = {};
    dragOriginals_.clear();
    const auto& els = currentPage().layers[sel_.layer].elements;
    for (size_t i : sel_.indices) dragOriginals_.push_back(els[i]);
}

// Moves the selected strokes in the document as the pen goes; the screen catches
// up in dragFrame().
void Editor::dragTo(const StrokePoint& p) {
    const IRect before = overlayArea();
    auto& els = currentPage().layers[sel_.layer].elements;
    const Page& page = currentPage();
    if (dragKind_ == DragKind::Move) {
        // It stops at the page's edges.
        const double dx = keepOnPage(p.x - dragStart_.x, dragFrom_.x0, dragFrom_.x1, page.width);
        const double dy = keepOnPage(p.y - dragStart_.y, dragFrom_.y0, dragFrom_.y1, page.height);
        for (size_t k = 0; k < sel_.indices.size(); ++k) els[sel_.indices[k]] = movedElement(dragOriginals_[k], dx, dy);
    } else {
        // Uniform scale about the top-left corner, following the pen along the diagonal,
        // and growing no further than the page's right and bottom edges.
        const StrokePoint anchor{dragFrom_.x0, dragFrom_.y0};
        const double vx = dragFrom_.x1 - anchor.x, vy = dragFrom_.y1 - anchor.y;
        const double len2 = vx * vx + vy * vy;
        double most = 10.0;
        if (vx > 0) most = std::min(most, (page.width - anchor.x) / vx);
        if (vy > 0) most = std::min(most, (page.height - anchor.y) / vy);
        const double factor =
            len2 > 0 ? std::clamp(((p.x - anchor.x) * vx + (p.y - anchor.y) * vy) / len2, 0.1, std::max(1.0, most)) : 1.0;
        for (size_t k = 0; k < sel_.indices.size(); ++k)
            els[sel_.indices[k]] = scaledElement(dragOriginals_[k], anchor, factor);
    }
    sel_.bounds = selectedBounds();
    layoutOverlay();
    dragDirty_ = dragDirty_.united(before).united(overlayArea());
}

void Editor::dragFrame(uint64_t now) {
    if (dragDirty_.empty()) return;
    previewArea(dragDirty_, now);  // black and white with DU, cleaned up once the pen rests
    dragDirty_ = {};
    lastDragFrame_ = now;
}

void Editor::finishDrag(uint64_t now) {
    dragFrame(now);
    const auto& els = currentPage().layers[sel_.layer].elements;
    bool changed = false;
    for (size_t k = 0; k < sel_.indices.size() && !changed; ++k) {
        const RectF a = elementBounds(els[sel_.indices[k]]), b = elementBounds(dragOriginals_[k]);
        changed = a.x0 != b.x0 || a.y0 != b.y0 || a.x1 != b.x1 || a.y1 != b.y1;
    }
    if (changed) {
        // Each stroke replaced by its moved or resized self, in place.
        UndoAction a{UndoAction::Kind::Edit, page_, sel_.layer, {}, std::nullopt};
        for (size_t k = 0; k < sel_.indices.size(); ++k) {
            a.edits.push_back({false, sel_.indices[k], std::move(dragOriginals_[k])});
            a.edits.push_back({true, sel_.indices[k], Stroke{}});
        }
        pushUndo(std::move(a));
        markModified(now);
    }
    dragOriginals_.clear();
}

void Editor::deleteSelection(uint64_t now) {
    const IRect area = overlayArea();
    auto& els = currentPage().layers[sel_.layer].elements;
    UndoAction a{UndoAction::Kind::Edit, page_, sel_.layer, {}, std::nullopt};
    for (auto it = sel_.indices.rbegin(); it != sel_.indices.rend(); ++it) {
        a.edits.push_back({false, *it, std::move(els[*it])});
        els.erase(els.begin() + static_cast<std::ptrdiff_t>(*it));
    }
    sel_ = Selection{};
    overlayButtons_.clear();
    pushUndo(std::move(a));
    markModified(now);
    repaint(area, Refresh::Content);
}

void Editor::duplicateSelection(uint64_t now) {
    const IRect before = overlayArea();
    auto& els = currentPage().layers[sel_.layer].elements;
    // The copy lands a little down and to the right, on top, and becomes the selection.
    const double offset = std::max(8.0, 0.1 * std::max(sel_.bounds.x1 - sel_.bounds.x0, sel_.bounds.y1 - sel_.bounds.y0));
    UndoAction a{UndoAction::Kind::Edit, page_, sel_.layer, {}, std::nullopt};
    std::vector<size_t> copies;
    for (size_t i : sel_.indices) {
        els.push_back(movedElement(els[i], offset, offset));
        a.edits.push_back({true, els.size() - 1, Stroke{}});
        copies.push_back(els.size() - 1);
    }
    select(std::move(copies));
    pushUndo(std::move(a));
    markModified(now);
    repaint(before.united(overlayArea()), Refresh::Content);
}

// ---- typing ----

double Editor::textSize() const { return kTextSizes[static_cast<size_t>(toolSize(ToolKind::Text))]; }

const ForeignElement& Editor::typed() const {
    return std::get<ForeignElement>(doc_.pages[page_].layers.back().elements[text_.index]);
}

int Editor::textAt(int x, int y) const {
    const Page& p = doc_.pages[page_];
    if (p.layers.empty()) return -1;
    const auto& els = p.layers.back().elements;
    for (size_t i = els.size(); i-- > 0;) {  // the topmost
        const auto* f = std::get_if<ForeignElement>(&els[i]);
        if (f && f->tag == "text" && f->mathOp.empty() && xf_.toScreen(f->bounds).grown(12).contains(x, y))
            return static_cast<int>(i);
    }
    return -1;
}

// On a text, the caret goes where the pen is in it; elsewhere a new one starts, its
// first line level with the pen.
void Editor::startText(int x, int y, uint64_t now) {
    (void)now;
    const size_t layer = currentLayer();
    auto& els = currentPage().layers[layer].elements;
    const int hit = textAt(x, y);
    text_ = TextEdit{};
    text_.active = true;
    if (hit >= 0) {
        text_.index = static_cast<size_t>(hit);
        text_.before = els[text_.index];
        const ForeignElement& f = typed();
        text_.caret =
            Font::sans().offsetAt(f.text, f.textSize, f.textWrap, xf_.toPageX(x) - f.textX, xf_.toPageY(y) - f.textY);
    } else {
        const Page& page = currentPage();
        const double size = textSize(), line = Font::sans().lineHeight() * size;
        const double tx = std::clamp(xf_.toPageX(x), 0.0, std::max(0.0, page.width - size));
        const double ty = std::clamp(xf_.toPageY(y) - line / 2, 0.0, std::max(0.0, page.height - line));
        els.push_back(textElement(nullptr, "", tx, ty, size));
        text_.index = els.size() - 1;
        text_.isNew = true;
    }
    keyboard_.setShift(typed().text.empty());  // a capital to start with
    placeKeyboard();
    // The frame and caret: black over the page, so DU.
    drawChrome(textArea());
    damage_.push_back({textArea(), Refresh::UI});
}

// The text, at least two ems wide and a line tall, so an empty one shows where it is.
IRect Editor::textFrame() const {
    if (!text_.active) return {};
    const ForeignElement& f = typed();
    RectF b = f.bounds;
    b.add(f.textX + 2 * f.textSize, f.textY + Font::sans().lineHeight() * f.textSize);
    return xf_.toScreen(b).grown(kFramePad);
}

IRect Editor::caretRect() const {
    if (!text_.active) return {};
    const ForeignElement& f = typed();
    const Font& font = Font::sans();
    const std::vector<TextLine> lines = font.layout(f.text, f.textSize, f.textWrap);
    size_t row = 0;
    while (row + 1 < lines.size() && text_.caret > lines[row].end) ++row;
    const double x = f.textX + font.advance(f.text, lines[row].begin, text_.caret) * f.textSize;
    const double y = f.textY + row * font.lineHeight() * f.textSize;
    const int sx = static_cast<int>(std::lround(xf_.toScreenX(x)));
    return {sx - 1, static_cast<int>(std::lround(xf_.toScreenY(y))), sx + 2,
            static_cast<int>(std::lround(xf_.toScreenY(y + font.lineHeight() * f.textSize)))};
}

void Editor::drawTextEdit(const IRect& clip) {
    if (!text_.active) return;
    dashedFrame(canvas_, textFrame(), clip);
    canvas_.fill(caretRect().intersected(clip), 0);
}

// The keyboard goes along the bottom, unless it would hide the line being typed.
void Editor::placeKeyboard() {
    const bool top = caretRect().y1 + 20 > keyboard_.rectAt(false).y0;
    if (keyboard_.open() && keyboard_.atTop() == top) return;
    const IRect before = keyboard_.open() ? keyboard_.rect() : IRect{};
    keyboard_.show(top);
    drawChrome(keyboard_.rect());  // black and white over the page: DU
    damage_.push_back({keyboard_.rect(), Refresh::UI});
    repaint(before, Refresh::Content);  // where it was
}

void Editor::setText(std::string text, size_t caret, double size, uint64_t now) {
    const IRect before = textArea();
    Element& el = currentPage().layers[currentLayer()].elements[text_.index];
    ForeignElement next = textElement(&std::get<ForeignElement>(el), text, typed().textX, typed().textY, size);
    el = std::move(next);
    text_.caret = caret;
    markModified(now);
    // Shown at once in black and white, and properly once the typing pauses.
    previewArea(before.united(textArea()), now);
    placeKeyboard();
}

void Editor::type(const std::string& s, uint64_t now) {
    if (!text_.active || s.empty()) return;
    std::string t = typed().text;
    size_t caret = std::min(text_.caret, t.size());
    if (s == "\b") {
        if (caret == 0) return;
        const size_t from = prevCodePointStart(t, caret);
        t.erase(from, caret - from);
        caret = from;
    } else {
        t.insert(caret, s);
        caret += s.size();
        // A line that runs past the page's right edge is broken at its last space before
        // the caret, with a real line break: Xournal++ doesn't wrap text.
        const size_t nl = caret > 0 ? t.rfind('\n', caret - 1) : std::string::npos;
        const size_t start = nl == std::string::npos ? 0 : nl + 1;
        const size_t end = std::min(t.find('\n', caret), t.size());
        const ForeignElement& f = typed();
        if (start < caret && Font::sans().advance(t, start, end) * f.textSize > currentPage().width - f.textX - kTextEdge) {
            const size_t sp = t.rfind(' ', caret - 1);
            if (sp != std::string::npos && sp > start) t[sp] = '\n';
        }
    }
    setText(std::move(t), caret, typed().textSize, now);
    if (keyboard_.shift() && s != "\b" && s != "\n") {  // typed on a real keyboard: SHIFT was for one capital
        keyboard_.setShift(false);
        drawChrome(keyboard_.rect());
        damage_.push_back({keyboard_.rect(), Refresh::UI});
    }
}

void Editor::keyPress(int x, int y, uint64_t now) {
    const bool top = keyboard_.atTop();
    const Keyboard::Press p = keyboard_.tap(x, y);
    const bool changed = keyboard_.changed();
    switch (p.key) {
        case Keyboard::Key::None: return;
        case Keyboard::Key::Done: return finishText(now);
        case Keyboard::Key::Text: type(p.text, now); break;
        case Keyboard::Key::Delete: type("\b", now); break;
        case Keyboard::Key::Enter: type("\n", now); break;
        case Keyboard::Key::Shift:
        case Keyboard::Key::Symbols: break;
    }
    if (!keyboard_.open() || keyboard_.atTop() != top) return;  // it moved, and was drawn whole
    if (changed) {
        drawChrome(keyboard_.rect());
        damage_.push_back({keyboard_.rect(), Refresh::UI});
    }
    // A key that types shows it was pressed; SHIFT and ?123 show whether they're on.
    if (p.key != Keyboard::Key::Shift && p.key != Keyboard::Key::Symbols) flash(p.r, now);
}

void Editor::caretTo(int x, int y, uint64_t now) {
    const IRect before = caretRect();
    const ForeignElement& f = typed();
    text_.caret =
        Font::sans().offsetAt(f.text, f.textSize, f.textWrap, xf_.toPageX(x) - f.textX, xf_.toPageY(y) - f.textY);
    previewArea(before.united(caretRect()), now);
}

void Editor::closeKeyboard() {
    if (!keyboard_.open()) return;
    const IRect r = keyboard_.rect();
    keyboard_.close();
    repaint(r, Refresh::Content);
}

void Editor::finishText(uint64_t now, bool keepKeyboard) {
    if (!text_.active) return;
    const IRect area = textArea();
    IRect keyboard;
    if (!keepKeyboard && keyboard_.open()) {
        keyboard = keyboard_.rect();
        keyboard_.close();
    }
    TextEdit t = std::move(text_);
    text_ = TextEdit{};
    const size_t layer = currentLayer();
    auto& els = currentPage().layers[layer].elements;
    const ForeignElement& f = std::get<ForeignElement>(els[t.index]);
    if (f.text.find_first_not_of(" \n") == std::string::npos) {
        // Nothing typed, or all of it deleted: it goes.
        els.erase(els.begin() + static_cast<std::ptrdiff_t>(t.index));
        if (!t.isNew) {
            pushUndo({UndoAction::Kind::Edit, page_, layer, {{false, t.index, std::move(t.before)}}, std::nullopt});
            markModified(now);
        }
    } else if (t.isNew) {
        pushUndo({UndoAction::Kind::Edit, page_, layer, {{true, t.index, Stroke{}}}, std::nullopt});
    } else {
        const ForeignElement& b = std::get<ForeignElement>(t.before);
        if (b.text != f.text || b.textSize != f.textSize || b.textX != f.textX || b.textY != f.textY)
            pushUndo({UndoAction::Kind::Edit, page_, layer, {{false, t.index, std::move(t.before)}, {true, t.index, Stroke{}}},
                      std::nullopt});
    }
    repaint(area, Refresh::Content);
    repaint(keyboard, Refresh::Content);
}

// ---- actions ----

void Editor::perform(Action a, uint64_t now) {
    // Typing is done with anything but the tools panel, whose sizes go for the text too.
    const bool keepsTyping = a == Action::None || a == Action::Tools || a == Action::Text || a == Action::SizeFine ||
                             a == Action::SizeMedium || a == Action::SizeThick;
    if (!keepsTyping) finishText(now);

    // The selection's math buttons; anything else closes their answer.
    switch (a) {
        case Action::None: return;
        case Action::MathSum:
        case Action::MathAverage:
        case Action::MathMin:
        case Action::MathMax:
        case Action::MathCount:
            closeMath();
            return openMath(a, now);
        case Action::EditMenu:
        case Action::MathMenu:
            closeMath();
            return showSelectionMenu(a == Action::EditMenu ? SelectionMenu::Edit : SelectionMenu::Math);
        default: closeMath(); break;
    }

    // The paper menu's buttons; anything else puts the menu away.
    switch (a) {
        case Action::None: return;  // between buttons
        case Action::PageStyle:
            if (paperMenu_.open()) closePaperMenu();
            else openPaperMenu();
            return;
        case Action::PaperBlank:
        case Action::PaperRuled:
        case Action::PaperGrid:
        case Action::PaperNarrow:
        case Action::PaperMedium:
        case Action::PaperWide:
        case Action::PaperMargin:
        case Action::PaperEnds: return changePaper(a, now);
        case Action::DeletePage: return deletePage(now);
        default: closePaperMenu(); break;
    }

    // The tools panel's buttons (and their keyboard shortcuts).
    const std::vector<IRect> panelBefore = panel_.rects();
    switch (a) {
        case Action::Tools:
            panel_.setOpen(!panel_.open());
            return refreshPanel(panelBefore);
        case Action::Pen:
        case Action::Highlighter:
        case Action::Eraser:
        case Action::Text:
            clearSelection();
            tool_ = a == Action::Pen           ? ToolKind::Pen
                    : a == Action::Highlighter ? ToolKind::Highlighter
                    : a == Action::Eraser      ? ToolKind::Eraser
                                               : ToolKind::Text;
            // In the panel, the tool's options come up beside it; tapping it again puts the panel away.
            if (panel_.open() && panel_.options() != a) panel_.showOptions(a);
            else panel_.setOpen(false);
            return refreshPanel(panelBefore);
        case Action::Lasso:
        case Action::Pan:
            tool_ = a == Action::Lasso ? ToolKind::Lasso : ToolKind::Pan;
            panel_.setOpen(false);
            return refreshPanel(panelBefore);
        case Action::ZoomIn:
        case Action::ZoomOut:
        case Action::ZoomFit: {
            // In steps, about the middle of the screen; the panel stays open for more.
            double z = 1;
            if (a == Action::ZoomIn) {
                z = kMaxZoom;
                for (double step : kZoomSteps)
                    if (step > zoom_ + 0.01) { z = step; break; }
            } else if (a == Action::ZoomOut) {
                for (double step : kZoomSteps)
                    if (step < zoom_ - 0.01) z = step;
            }
            zoomTo(z, (pageArea_.x0 + pageArea_.x1) / 2.0, (pageArea_.y0 + pageArea_.y1) / 2.0, now);
            return;
        }
        case Action::Shapes:
            clearSelection();
            shapes_ = !shapes_;
            tool_ = ToolKind::Pen;  // only the pen's strokes become shapes
            panel_.setOpen(false);
            setStatus(shapes_ ? "SHAPES" : "FREE", now);
            return refreshPanel(panelBefore);
        case Action::SizeFine:
        case Action::SizeMedium:
        case Action::SizeThick:
            if (tool_ != ToolKind::Lasso && tool_ != ToolKind::Pan) {
                sizes_[static_cast<size_t>(tool_)] = a == Action::SizeFine     ? ToolSize::Fine
                                                     : a == Action::SizeMedium ? ToolSize::Medium
                                                                               : ToolSize::Thick;
            }
            if (text_.active) setText(typed().text, text_.caret, textSize(), now);
            panel_.setOpen(false);
            return refreshPanel(panelBefore);
        case Action::LineSolid:
        case Action::LineDots:
        case Action::LineDashes:
            clearSelection();
            line_ = a == Action::LineDots ? LineStyle::Dots : a == Action::LineDashes ? LineStyle::Dashes : LineStyle::Solid;
            tool_ = ToolKind::Pen;
            panel_.setOpen(false);
            setStatus(a == Action::LineDots ? "DOTTED" : a == Action::LineDashes ? "DASHED" : "SOLID", now);
            return refreshPanel(panelBefore);
        case Action::ErasePartial:
        case Action::EraseWhole:
            clearSelection();
            eraseWhole_ = a == Action::EraseWhole;
            tool_ = ToolKind::Eraser;
            panel_.setOpen(false);
            setStatus(eraseWhole_ ? "WHOLE" : "PARTIAL", now);
            return refreshPanel(panelBefore);
        default: break;
    }

    closePanel();  // anything else puts the tools away first
    switch (a) {
        case Action::Delete:
            if (sel_.active) deleteSelection(now);
            return;
        case Action::Duplicate:
            if (sel_.active) duplicateSelection(now);
            return;
        case Action::Cancel: clearSelection(); return;
        case Action::Undo:
            clearSelection();
            undo(now);
            return;
        case Action::Redo:
            clearSelection();
            redo(now);
            return;
        case Action::PrevPage:
            if (page_ > 0) setPage(page_ - 1);
            return;
        case Action::NextPage:
            if (page_ + 1 < doc_.pages.size()) setPage(page_ + 1);
            else addPage(now);
            return;
        case Action::Save: save(now); return;
        case Action::Title:
            titleOnly_ = !titleOnly_;
            updateToolbar();
            return;
        case Action::Exit:
        case Action::Quit:
            // Saving, and leaving the app, take a moment: show the button pressed first.
            pendingExit_ = a;
            pendingExitShown_ = false;
            updateToolbar();
            return;
        case Action::None:  // handled above
        case Action::PageStyle:
        case Action::PaperBlank:
        case Action::PaperRuled:
        case Action::PaperGrid:
        case Action::PaperNarrow:
        case Action::PaperMedium:
        case Action::PaperWide:
        case Action::PaperMargin:
        case Action::PaperEnds:
        case Action::DeletePage:
        case Action::EditMenu:
        case Action::MathMenu:
        case Action::MathSum:
        case Action::MathAverage:
        case Action::MathMin:
        case Action::MathMax:
        case Action::MathCount:
        case Action::Tools:
        case Action::Pen:
        case Action::Shapes:
        case Action::Lasso:
        case Action::Pan:
        case Action::ZoomOut:
        case Action::ZoomIn:
        case Action::ZoomFit:
        case Action::Highlighter:
        case Action::Eraser:
        case Action::Text:
        case Action::SizeFine:
        case Action::SizeMedium:
        case Action::SizeThick:
        case Action::ErasePartial:
        case Action::EraseWhole:
        case Action::LineSolid:
        case Action::LineDots:
        case Action::LineDashes:
        case Action::NewNotebook:  // the library's
        case Action::About:
        case Action::Sort:
        case Action::Rename:
        case Action::Confirm: return;
    }
}

void Editor::finishExit(uint64_t now) {
    const Action a = pendingExit_;
    pendingExit_ = Action::None;
    if (!modified_ || now < exitArmedUntil_ || save(now)) {
        close_ = true;
        quit_ = a == Action::Quit;
    } else {
        // Save failed: a second tap within a few seconds leaves without saving.
        exitArmedUntil_ = now + 4000;
        setStatus("UNSAVED", now, 4000);
    }
}

void Editor::setPage(size_t index) {
    page_ = index;
    chain_.valid = false;
    sel_ = Selection{};
    overlayButtons_.clear();
    math_.close();
    answer_ = -1;
    text_ = TextEdit{};  // done with already (see perform)
    keyboard_.close();
    xf_ = fitPage(currentPage(), pageArea_);  // back to fitting the page
    zoom_ = 1;
    fingers_.clear();
    viewGesture_ = ViewGesture::None;
    viewDirty_ = false;
    palm_ = false;
    settle_ = {};
    settleAt_ = 0;
    redrawAll(Refresh::Full);
}

void Editor::addPage(uint64_t now) {
    const Page& cur = currentPage();
    Page p = makeBlankPage(cur.width, cur.height, opts_.pageStyle);
    if (cur.background.type == "solid") {
        p.background.color = cur.background.color;
        p.background.style = cur.background.style;
        p.background.config = cur.background.config;
    }
    doc_.pages.insert(doc_.pages.begin() + static_cast<std::ptrdiff_t>(page_ + 1), std::move(p));
    pushUndo({UndoAction::Kind::AddPage, page_ + 1, 0, {}, std::nullopt});
    markModified(now);
    setPage(page_ + 1);
}

bool Editor::save(uint64_t now, bool quiet) {
    std::string error;
    if (!saveXopp(path_, doc_, error)) {
        std::fprintf(stderr, "scrible: saving %s failed: %s\n", path_.c_str(), error.c_str());
        setStatus("FAILED", now, 3000);
        return false;
    }
    modified_ = false;
    if (quiet) updateToolbar();
    else setStatus("SAVED", now);
    return true;
}

void Editor::markModified(uint64_t now) {
    if (modified_) return;
    modified_ = true;
    modifiedSince_ = now;
    updateToolbar();
}

void Editor::setStatus(const std::string& text, uint64_t now, uint64_t durationMs) {
    status_ = text;
    statusUntil_ = now + durationMs;
    updateToolbar();
}

// ---- undo/redo ----

void Editor::pushUndo(UndoAction a) {
    undo_.push_back(std::move(a));
    if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
    const bool hadRedo = !redo_.empty();
    redo_.clear();
    if (undo_.size() == 1 || hadRedo) updateToolbar();
}

// Applies (forward) or reverts an action in place, swapping elements between the
// document and the action so it can be replayed in the other direction.
RectF Editor::apply(UndoAction& a, bool forward) {
    RectF changed;
    auto unite = [&](const Element& e) {
        const RectF b = elementBounds(e);
        if (!b.valid) return;
        changed.add(b.x0, b.y0);
        changed.add(b.x1, b.y1);
        // A math answer's i, beside it, comes and goes with it.
        if (const auto* f = std::get_if<ForeignElement>(&e); f && !f->mathOp.empty()) {
            const double reach = (kInfoGap + kInfoSize + 4) / xf_.scale;
            changed.add(b.x1 + reach, b.y0 + reach);
        }
    };
    auto take = [](std::vector<Element>& els, size_t i) {
        Element e = std::move(els[i]);
        els.erase(els.begin() + static_cast<std::ptrdiff_t>(i));
        return e;
    };
    auto put = [](std::vector<Element>& els, size_t i, Element e) {
        els.insert(els.begin() + static_cast<std::ptrdiff_t>(i), std::move(e));
    };

    if (a.kind == UndoAction::Kind::Background) {
        Page& p = doc_.pages[a.page];
        std::swap(p.background, a.background);
        changed.add(0, 0);
        changed.add(p.width, p.height);
        return changed;
    }
    if (a.kind == UndoAction::Kind::AddPage || a.kind == UndoAction::Kind::DeletePage) {
        if (forward == (a.kind == UndoAction::Kind::AddPage)) {
            doc_.pages.insert(doc_.pages.begin() + static_cast<std::ptrdiff_t>(a.page), std::move(*a.pageData));
            a.pageData.reset();
        } else {
            a.pageData = std::move(doc_.pages[a.page]);
            doc_.pages.erase(doc_.pages.begin() + static_cast<std::ptrdiff_t>(a.page));
        }
        return changed;
    }

    auto& els = doc_.pages[a.page].layers[a.layer].elements;
    auto step = [&](ElementEdit& e, bool redo) {
        if (e.insert == redo) {
            unite(e.element);
            put(els, e.index, std::move(e.element));
        } else {
            e.element = take(els, e.index);
            unite(e.element);
        }
    };
    if (forward) {
        for (auto& e : a.edits) step(e, true);
    } else {
        for (auto it = a.edits.rbegin(); it != a.edits.rend(); ++it) step(*it, false);
    }
    return changed;
}

void Editor::undo(uint64_t now) {
    if (undo_.empty() || penBusy()) return;
    chain_.valid = false;
    UndoAction a = std::move(undo_.back());
    undo_.pop_back();
    const RectF changed = apply(a, false);
    const size_t page = a.kind == UndoAction::Kind::AddPage ? std::min(a.page - 1, doc_.pages.size() - 1) : a.page;
    redo_.push_back(std::move(a));
    afterHistoryChange(page, changed, now);
}

void Editor::redo(uint64_t now) {
    if (redo_.empty() || penBusy()) return;
    chain_.valid = false;
    UndoAction a = std::move(redo_.back());
    redo_.pop_back();
    const RectF changed = apply(a, true);
    const size_t page = a.kind == UndoAction::Kind::DeletePage ? std::min(a.page, doc_.pages.size() - 1) : a.page;
    undo_.push_back(std::move(a));
    afterHistoryChange(page, changed, now);
}

void Editor::afterHistoryChange(size_t page, const RectF& changed, uint64_t now) {
    markModified(now);
    if (page != page_ || !changed.valid) {
        // Jump to where the change happened, so it's never invisible.
        page_ = std::min(page, doc_.pages.size() - 1);
        setPage(page_);
        return;
    }
    repaint(xf_.toScreen(changed), Refresh::Content);
    updateToolbar();
}

// ---- deferred work ----

void Editor::onIdle(uint64_t now) {
    if (pendingExit_ != Action::None) {
        // The first pass only lets the pressed button reach the screen.
        if (!pendingExitShown_) pendingExitShown_ = true;
        else return finishExit(now);
    }
    if (flashUntil_ && now >= flashUntil_) endFlash();
    const bool busy = penBusy();
    if ((gesture_ == Gesture::Drag || gesture_ == Gesture::MoveText) && !dragDirty_.empty() &&
        now >= lastDragFrame_ + kDragFrameMs)
        dragFrame(now);

    if (viewDirty_ && now >= lastViewFrame_ + kViewFrameMs) viewFrame(now);
    if (settleAt_ && now >= settleAt_ && !busy) {
        // Repaint from the document (replacing the live-ink approximations) and refresh
        // with the grayscale waveform, so antialiasing and highlighters show properly.
        repaint(settle_, Refresh::Content);
        settle_ = {};
        settleAt_ = 0;
    }
    if (statusUntil_ && now >= statusUntil_) {
        status_.clear();
        statusUntil_ = 0;
        updateToolbar();
    }
    if (modified_ && !busy && !settleAt_ && now >= modifiedSince_ + opts_.autosaveDelayMs) {
        if (!save(now, true)) modifiedSince_ = now;  // retry later rather than on every wakeup
    }
}

int Editor::msUntilIdleWork(uint64_t now) const {
    // Only deadlines onIdle() can act on right now; anything else would spin.
    // While the pen is down, its own events keep waking us up.
    if (pendingExit_ != Action::None) return 0;
    const bool busy = penBusy();
    uint64_t next = 0;
    auto consider = [&](uint64_t t) {
        if (t && (!next || t < next)) next = t;
    };
    if ((gesture_ == Gesture::Drag || gesture_ == Gesture::MoveText) && !dragDirty_.empty())
        consider(lastDragFrame_ + kDragFrameMs);

    if (viewDirty_) consider(std::max<uint64_t>(lastViewFrame_ + kViewFrameMs, 1));
    if (!busy) consider(settleAt_);
    consider(statusUntil_);
    consider(flashUntil_);
    if (modified_ && !busy && !settleAt_) consider(std::max<uint64_t>(modifiedSince_ + opts_.autosaveDelayMs, 1));
    if (!next) return -1;
    return next <= now ? 0 : static_cast<int>(std::min<uint64_t>(next - now, 1u << 30));
}

}  // namespace scrible
