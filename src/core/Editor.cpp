#include "Editor.h"

#include <cmath>
#include <cstdio>

#include "Eraser.h"
#include "Lasso.h"
#include "Shapes.h"
#include "XoppFile.h"

namespace scrible {
namespace {

constexpr uint8_t kOffPageGray = 220;
constexpr double kMinPointDistPx = 0.75;  // closer samples are dropped
constexpr size_t kMaxUndo = 1000;
constexpr uint64_t kDragFrameMs = 50;  // at most this often while moving a selection (e-ink can't keep up)
constexpr int kFramePad = 14, kHandle = 24, kOverlayScale = 3;

// Preset widths (points) of the original Xournal, indexed by ToolSize.
constexpr double kPenWidths[] = {0.85, 1.41, 2.26};
constexpr double kHighlighterWidths[] = {2.83, 8.50, 19.84};
constexpr double kEraserWidths[] = {2.83, 8.50, 19.84};

// The drawing tools are in the panel along the left edge (ToolPanel).
ToolbarGroups editorToolbar(bool fromList) {
    ToolbarGroups g = {
        {{Action::Undo, "UNDO"}, {Action::Redo, "REDO"}},
        {{Action::PageStyle, "PAGE"}, {Action::PrevPage, "<"}, {Action::None, "000/000"}, {Action::NextPage, ">"}},
        {{Action::Save, "SAVE*"}},
    };
    if (fromList) g.back().insert(g.back().end(), {{Action::Exit, "BACK"}, {Action::Quit, "EXIT"}});
    else g.back().push_back({Action::Exit, "EXIT"});
    return g;
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
      doc_(std::move(doc)),
      path_(std::move(path)),
      sizes_(opts_.sizes),
      eraseWhole_(opts_.eraseWholeStrokes),
      shapes_(opts_.recognizeShapes) {
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
    // A change in the open paper menu becomes an undo step when the menu closes, which UNDO does.
    const Background& bg = doc_.pages[page_].background;
    const bool paperChanged = paperMenu_.open() && (bg.style != paperBefore_.style || bg.config != paperBefore_.config);
    if (undo_.empty() && !paperChanged) s.disabled.push_back(Action::Undo);
    if (redo_.empty() || paperChanged) s.disabled.push_back(Action::Redo);
    if (paperMenu_.open()) s.active.push_back(Action::PageStyle);
    if (bg.type != "solid" && doc_.pages.size() < 2) s.disabled.push_back(Action::PageStyle);  // nothing to offer
    s.labels = {{Action::Save, modified_ ? "SAVE*" : "SAVE"}};
    s.text = !status_.empty() ? status_ : std::to_string(page_ + 1) + "/" + std::to_string(doc_.pages.size());
    return s;
}

ToolPanelState Editor::panelState() const {
    static constexpr Action tools[] = {Action::Pen, Action::Highlighter, Action::Eraser, Action::Lasso};
    ToolPanelState s;
    s.tool = tools[static_cast<int>(tool_)];
    s.shapes = shapes_;
    s.eraseWhole = eraseWhole_;
    if (tool_ != ToolKind::Lasso) {
        const double* widths = tool_ == ToolKind::Pen           ? kPenWidths
                               : tool_ == ToolKind::Highlighter ? kHighlighterWidths
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

// What goes over the page: the selection's frame and buttons, the tools panel, the paper menu.
void Editor::drawChrome(const IRect& clip) {
    if (sel_.active) drawOverlay(clip);
    panel_.draw(canvas_, panelState(), clip);
    paperMenu_.draw(canvas_, clip);
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
    // DU passes over them.
    for (Refresh m : {Refresh::Ink, Refresh::Content, Refresh::UI}) {
        IRect u;
        for (const Damage& d : damage_)
            if (d.mode == m) u = u.united(d.rect);
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
        perform(toolbar_.hit(x, y), now);
        return;
    }
    if (panel_.covers(x, y) || paperMenu_.covers(x, y)) {
        gesture_ = Gesture::Toolbar;
        perform(panel_.covers(x, y) ? panel_.hit(x, y) : paperMenu_.hit(x, y), now);
        return;
    }
    // Writing beside the open panel or menu puts it away, once the pen lifts:
    // repainting what it covers needs the slow waveform, which mustn't delay the ink.
    closePanelOnUp_ = panel_.open() || paperMenu_.open();
    if (sel_.active) {
        for (const OverlayButton& b : overlayButtons_) {
            if (!b.r.contains(x, y)) continue;
            gesture_ = Gesture::Toolbar;
            perform(b.action, now);
            return;
        }
        const StrokePoint p{xf_.toPageX(s.x), xf_.toPageY(s.y)};
        const bool tip = !s.eraser && !s.button;
        if (tip && selectionHandle().grown(16).contains(x, y)) return startDrag(DragKind::Resize, p);
        if (tip && selectionFrame().contains(x, y)) return startDrag(DragKind::Move, p);
        clearSelection();  // tapping elsewhere lets go of it
    }
    if (!drawingClip().contains(x, y)) return;

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
    const StrokePoint p{xf_.toPageX(s.x), xf_.toPageY(s.y)};
    if (gesture_ == Gesture::Lasso) return lassoTo(p);
    if (gesture_ == Gesture::Drag) {
        dragTo(p);
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
    if (gesture_ != Gesture::None) return;  // the pen is busy; this is a palm
    if (toolbar_.rect().contains(x, y)) return perform(toolbar_.hit(x, y), now);
    if (panel_.covers(x, y)) return perform(panel_.hit(x, y), now);
    if (paperMenu_.covers(x, y)) return perform(paperMenu_.hit(x, y), now);
    for (const OverlayButton& b : overlayButtons_)
        if (sel_.active && b.r.contains(x, y)) return perform(b.action, now);
    // Fingers don't draw, so a tap elsewhere just puts the tools or the menu away.
    closePanel();
    closePaperMenu();
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
    std::vector<Stroke> pieces;
    for (size_t i = 0; i < elements.size();) {
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
    if (changed.valid) previewArea(xf_.toScreen(changed), now);
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
    std::vector<size_t> picked = strokesInLasso(currentPage().layers[currentLayer()].elements, lasso_);
    IRect area = lassoArea_;
    lasso_.clear();
    lassoArea_ = {};
    if (!picked.empty()) {
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

// The buttons go above the frame if there's room, else below; on the page either
// way, and not under the tools tab.
void Editor::layoutOverlay() {
    overlayButtons_.clear();
    const IRect f = selectionFrame();
    const std::vector<std::pair<Action, std::string>> specs = {
        {Action::Delete, "DELETE"}, {Action::Duplicate, "DUPLICATE"}, {Action::Cancel, "DONE"}};
    const int h = 96, gap = 12;
    int total = -gap;
    for (const auto& [a, label] : specs) total += Canvas::textWidth(label, kOverlayScale) + 48 + gap;
    int y0 = f.y0 - 20 - h;
    if (y0 < pageArea_.y0 + 10) y0 = std::min(f.y1 + kHandle + 20, pageArea_.y1 - 10 - h);
    const IRect tab = panel_.tabRect();
    const int minX = (y0 < tab.y1 && y0 + h > tab.y0 ? tab.x1 : pageArea_.x0) + 10;
    const int x0 = std::clamp(f.x0, minX, std::max(minX, pageArea_.x1 - 10 - total));
    int x = x0;
    for (const auto& [a, label] : specs) {
        const int w = Canvas::textWidth(label, kOverlayScale) + 48;
        overlayButtons_.push_back({a, label, {x, y0, x + w, y0 + h}});
        x += w + gap;
    }
}

IRect Editor::overlayArea() const {
    if (!sel_.active) return {};
    IRect r = selectionFrame().united(selectionHandle());
    for (const OverlayButton& b : overlayButtons_) r = r.united(b.r);
    return r.grown(2);
}

// The selection's dashed frame, resize handle and buttons: black and white only, so
// they can appear with DU.
void Editor::drawOverlay(const IRect& clip) {
    const IRect f = selectionFrame();
    auto fill = [&](const IRect& r, uint8_t v) { canvas_.fill(r.intersected(clip), v); };
    for (int x = f.x0; x < f.x1; x += 24) {
        fill({x, f.y0, std::min(x + 14, f.x1), f.y0 + 3}, 0);
        fill({x, f.y1 - 3, std::min(x + 14, f.x1), f.y1}, 0);
    }
    for (int y = f.y0; y < f.y1; y += 24) {
        fill({f.x0, y, f.x0 + 3, std::min(y + 14, f.y1)}, 0);
        fill({f.x1 - 3, y, f.x1, std::min(y + 14, f.y1)}, 0);
    }
    const IRect h = selectionHandle();
    fill(h, 0);
    fill(h.grown(-8), 255);
    fill(h.grown(-14), 0);
    for (const OverlayButton& b : overlayButtons_) {
        fill(b.r, 0);
        fill(b.r.grown(-3), 255);
        const int tx = b.r.x0 + (b.r.width() - Canvas::textWidth(b.label, kOverlayScale)) / 2;
        const int ty = b.r.y0 + (b.r.height() - 8 * kOverlayScale) / 2;
        canvas_.drawText(tx, ty, b.label, kOverlayScale, 0, b.r.intersected(clip));
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
    for (size_t i : sel_.indices) dragOriginals_.push_back(std::get<Stroke>(els[i]));
}

// Moves the selected strokes in the document as the pen goes; the screen catches
// up in dragFrame().
void Editor::dragTo(const StrokePoint& p) {
    const IRect before = overlayArea();
    auto& els = currentPage().layers[sel_.layer].elements;
    if (dragKind_ == DragKind::Move) {
        const double dx = p.x - dragStart_.x, dy = p.y - dragStart_.y;
        for (size_t k = 0; k < sel_.indices.size(); ++k) els[sel_.indices[k]] = movedStroke(dragOriginals_[k], dx, dy);
    } else {
        // Uniform scale about the top-left corner, following the pen along the diagonal.
        const StrokePoint anchor{dragFrom_.x0, dragFrom_.y0};
        const double vx = dragFrom_.x1 - anchor.x, vy = dragFrom_.y1 - anchor.y;
        const double len2 = vx * vx + vy * vy;
        const double factor =
            len2 > 0 ? std::clamp(((p.x - anchor.x) * vx + (p.y - anchor.y) * vy) / len2, 0.1, 10.0) : 1.0;
        for (size_t k = 0; k < sel_.indices.size(); ++k)
            els[sel_.indices[k]] = scaledStroke(dragOriginals_[k], anchor, factor);
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
        const Stroke& s = std::get<Stroke>(els[sel_.indices[k]]);
        changed = s.width != dragOriginals_[k].width || s.points.front().x != dragOriginals_[k].points.front().x ||
                  s.points.front().y != dragOriginals_[k].points.front().y;
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
        els.push_back(movedStroke(std::get<Stroke>(els[i]), offset, offset));
        a.edits.push_back({true, els.size() - 1, Stroke{}});
        copies.push_back(els.size() - 1);
    }
    select(std::move(copies));
    pushUndo(std::move(a));
    markModified(now);
    repaint(before.united(overlayArea()), Refresh::Content);
}

// ---- actions ----

void Editor::perform(Action a, uint64_t now) {
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
        case Action::PaperMargin: return changePaper(a, now);
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
            clearSelection();
            tool_ = a == Action::Pen ? ToolKind::Pen : a == Action::Highlighter ? ToolKind::Highlighter : ToolKind::Eraser;
            // In the panel, the tool's options come up beside it; tapping it again puts the panel away.
            if (panel_.open() && panel_.options() != a) panel_.showOptions(a);
            else panel_.setOpen(false);
            return refreshPanel(panelBefore);
        case Action::Lasso:
            tool_ = ToolKind::Lasso;
            panel_.setOpen(false);
            return refreshPanel(panelBefore);
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
            if (tool_ != ToolKind::Lasso) {
                sizes_[static_cast<size_t>(tool_)] = a == Action::SizeFine     ? ToolSize::Fine
                                                     : a == Action::SizeMedium ? ToolSize::Medium
                                                                               : ToolSize::Thick;
            }
            panel_.setOpen(false);
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
        case Action::Exit:
        case Action::Quit:
            if (!modified_ || now < exitArmedUntil_ || save(now)) {
                close_ = true;
                quit_ = a == Action::Quit;
            } else {
                // Save failed: a second tap within a few seconds leaves without saving.
                exitArmedUntil_ = now + 4000;
                setStatus("UNSAVED", now, 4000);
            }
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
        case Action::DeletePage:
        case Action::Tools:
        case Action::Pen:
        case Action::Shapes:
        case Action::Lasso:
        case Action::Highlighter:
        case Action::Eraser:
        case Action::SizeFine:
        case Action::SizeMedium:
        case Action::SizeThick:
        case Action::ErasePartial:
        case Action::EraseWhole:
        case Action::NewNotebook:  // the library's
        case Action::About:
        case Action::Sort:
        case Action::Rename:
        case Action::Confirm: return;
    }
}

void Editor::setPage(size_t index) {
    page_ = index;
    chain_.valid = false;
    sel_ = Selection{};
    overlayButtons_.clear();
    xf_ = fitPage(currentPage(), pageArea_);
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
    const bool busy = penBusy();
    if (gesture_ == Gesture::Drag && !dragDirty_.empty() && now >= lastDragFrame_ + kDragFrameMs) dragFrame(now);
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
    const bool busy = penBusy();
    uint64_t next = 0;
    auto consider = [&](uint64_t t) {
        if (t && (!next || t < next)) next = t;
    };
    if (gesture_ == Gesture::Drag && !dragDirty_.empty()) consider(lastDragFrame_ + kDragFrameMs);
    if (!busy) consider(settleAt_);
    consider(statusUntil_);
    if (modified_ && !busy && !settleAt_) consider(std::max<uint64_t>(modifiedSince_ + opts_.autosaveDelayMs, 1));
    if (!next) return -1;
    return next <= now ? 0 : static_cast<int>(std::min<uint64_t>(next - now, 1u << 30));
}

}  // namespace scrible
