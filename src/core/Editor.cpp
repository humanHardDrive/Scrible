#include "Editor.h"

#include <cstdio>

#include "Eraser.h"
#include "Shapes.h"
#include "XoppFile.h"

namespace scrible {
namespace {

constexpr uint8_t kOffPageGray = 220;
constexpr double kMinPointDistPx = 0.75;  // closer samples are dropped
constexpr size_t kMaxUndo = 1000;

// Preset widths (points) of the original Xournal, indexed by ToolSize.
constexpr double kPenWidths[] = {0.85, 1.41, 2.26};
constexpr double kHighlighterWidths[] = {2.83, 8.50, 19.84};
constexpr double kEraserWidths[] = {2.83, 8.50, 19.84};

ToolbarGroups editorToolbar(bool fromList) {
    ToolbarGroups g = {
        // Buttons are sized for their longer label: the pen is "SHAPE" while recognizing
        // shapes, the eraser "STROKE" while removing whole strokes.
        {{Action::Pen, "SHAPE"}, {Action::Highlighter, "MARK"}, {Action::Eraser, "STROKE"}},
        {{Action::SizeFine, "S"}, {Action::SizeMedium, "M"}, {Action::SizeThick, "L"}},
        {{Action::Undo, "UNDO"}, {Action::Redo, "REDO"}},
        {{Action::PrevPage, "<"}, {Action::None, "000/000"}, {Action::NextPage, ">"}},
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
      doc_(std::move(doc)),
      path_(std::move(path)),
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
    const auto i = static_cast<size_t>(size_);
    return tool_ == ToolKind::Highlighter ? kHighlighterWidths[i] : kPenWidths[i];
}

double Editor::eraserWidth() const { return kEraserWidths[static_cast<size_t>(size_)]; }

double Editor::pointWidth(const PenSample& s) const {
    if (tool_ == ToolKind::Highlighter) return toolWidth();
    // Light touch -> 60% of the preset, medium (0.5) -> 100%, full pressure -> 140%.
    return toolWidth() * std::clamp(0.6 + 0.8 * s.pressure, 0.6, 1.4);
}

// ---- drawing to the canvas ----

ToolbarState Editor::toolbarState() const {
    ToolbarState s;
    static constexpr Action tools[] = {Action::Pen, Action::Highlighter, Action::Eraser};
    static constexpr Action sizes[] = {Action::SizeFine, Action::SizeMedium, Action::SizeThick};
    s.active = {tools[static_cast<int>(tool_)], sizes[static_cast<int>(size_)]};
    if (undo_.empty()) s.disabled.push_back(Action::Undo);
    if (redo_.empty()) s.disabled.push_back(Action::Redo);
    s.labels = {{Action::Save, modified_ ? "SAVE*" : "SAVE"},
                {Action::Pen, shapes_ ? "SHAPE" : "PEN"},
                {Action::Eraser, eraseWhole_ ? "STROKE" : "ERASE"}};
    s.text = !status_.empty() ? status_ : std::to_string(page_ + 1) + "/" + std::to_string(doc_.pages.size());
    return s;
}

void Editor::redrawAll(Refresh mode) {
    toolbar_.draw(canvas_, toolbarState());
    canvas_.fill(pageArea_, kOffPageGray);
    drawPage(canvas_, currentPage(), xf_, pageArea_, opts_.render);
    damage_.clear();
    damage_.push_back({canvas_.rect(), mode});
}

void Editor::repaint(const IRect& r, Refresh mode) {
    const IRect area = r.intersected(pageArea_);
    if (area.empty()) return;
    canvas_.fill(area, kOffPageGray);
    drawPage(canvas_, currentPage(), xf_, area, opts_.render);
    damage_.push_back({area, mode});
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
    for (Refresh m : {Refresh::Ink, Refresh::UI, Refresh::Content}) {
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
    if (!drawingClip().contains(x, y)) return;

    if (s.eraser || tool_ == ToolKind::Eraser) {
        gesture_ = Gesture::Erase;
        pendingErase_ = UndoAction{UndoAction::Kind::Edit, page_, currentLayer(), {}, std::nullopt};
        eraserLast_.reset();
        chain_.valid = false;
        eraseAt(s, now);
        return;
    }

    gesture_ = Gesture::Draw;
    live_ = Stroke{};
    const bool hl = tool_ == ToolKind::Highlighter;
    live_.tool = hl ? StrokeTool::Highlighter : StrokeTool::Pen;
    live_.color = hl ? kHighlighterColor : kPenColor;
    live_.width = toolWidth();
    live_.points = {{xf_.toPageX(s.x), xf_.toPageY(s.y)}};
    livePointWidths_ = {pointWidth(s)};

    // Show a dot right away, before the pen moves.
    Stroke dot = live_;
    dot.points.push_back(dot.points[0]);
    if (!hl) dot.segmentWidths = {livePointWidths_[0]};
    damage_.push_back({drawLiveSegment(canvas_, dot, 0, xf_, drawingClip(), opts_.render), Refresh::Ink});
}

void Editor::penMove(const PenSample& s, uint64_t now) {
    penActivity(now);
    if (gesture_ == Gesture::Erase) {
        eraseAt(s, now);
        return;
    }
    if (gesture_ != Gesture::Draw) return;

    const StrokePoint p{xf_.toPageX(s.x), xf_.toPageY(s.y)};
    const StrokePoint& last = live_.points.back();
    const double dx = (p.x - last.x) * xf_.scale, dy = (p.y - last.y) * xf_.scale;
    if (dx * dx + dy * dy < kMinPointDistPx * kMinPointDistPx) return;

    live_.points.push_back(p);
    livePointWidths_.push_back(pointWidth(s));
    const size_t i = live_.points.size() - 2;
    if (live_.tool == StrokeTool::Pen) live_.segmentWidths.push_back((livePointWidths_[i] + livePointWidths_[i + 1]) / 2);
    damage_.push_back({drawLiveSegment(canvas_, live_, i, xf_, drawingClip(), opts_.render), Refresh::Ink});
}

void Editor::penUp(uint64_t now) {
    penActivity(now);
    if (gesture_ == Gesture::Draw) {
        commitStroke(now);
    } else if (gesture_ == Gesture::Erase && !pendingErase_.edits.empty()) {
        pushUndo(std::move(pendingErase_));
        markModified(now);
    }
    pendingErase_ = UndoAction{};
    gesture_ = Gesture::None;
}

void Editor::touchDown(int x, int y, uint64_t now) {
    if (gesture_ != Gesture::None) return;  // the pen is busy; this is a palm
    if (toolbar_.rect().contains(x, y)) perform(toolbar_.hit(x, y), now);
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
    if (changed.valid) previewErase(xf_.toScreen(changed), now);
}

void Editor::previewErase(const IRect& r, uint64_t now) {
    const IRect area = r.intersected(pageArea_);
    if (area.empty()) return;
    canvas_.fill(area, kOffPageGray);
    drawPage(canvas_, currentPage(), xf_, area, opts_.render);
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

// ---- actions ----

void Editor::perform(Action a, uint64_t now) {
    switch (a) {
        case Action::Pen:
            if (tool_ == ToolKind::Pen) {
                // Tapping the selected pen switches shape recognition on and off.
                shapes_ = !shapes_;
                setStatus(shapes_ ? "SHAPES" : "FREE", now);
            }
            tool_ = ToolKind::Pen;
            break;
        case Action::Highlighter: tool_ = ToolKind::Highlighter; break;
        case Action::Eraser:
            if (tool_ == ToolKind::Eraser) {
                // Tapping the selected eraser switches between cutting strokes and removing them whole.
                eraseWhole_ = !eraseWhole_;
                setStatus(eraseWhole_ ? "WHOLE" : "PARTIAL", now);
            }
            tool_ = ToolKind::Eraser;
            break;
        case Action::SizeFine: size_ = ToolSize::Fine; break;
        case Action::SizeMedium: size_ = ToolSize::Medium; break;
        case Action::SizeThick: size_ = ToolSize::Thick; break;
        case Action::Undo: undo(now); return;
        case Action::Redo: redo(now); return;
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
        case Action::None:
        case Action::NewNotebook:
        case Action::Sort:
        case Action::Rename:
        case Action::Duplicate:
        case Action::Delete:
        case Action::Confirm:
        case Action::Cancel: return;  // not editor actions
    }
    updateToolbar();
}

void Editor::setPage(size_t index) {
    page_ = index;
    chain_.valid = false;
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

    if (a.kind == UndoAction::Kind::AddPage) {
        if (forward) {
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
    if (undo_.empty() || gesture_ == Gesture::Draw || gesture_ == Gesture::Erase) return;
    chain_.valid = false;
    UndoAction a = std::move(undo_.back());
    undo_.pop_back();
    const RectF changed = apply(a, false);
    const size_t page = a.kind == UndoAction::Kind::AddPage ? std::min(a.page - 1, doc_.pages.size() - 1) : a.page;
    redo_.push_back(std::move(a));
    afterHistoryChange(page, changed, now);
}

void Editor::redo(uint64_t now) {
    if (redo_.empty() || gesture_ == Gesture::Draw || gesture_ == Gesture::Erase) return;
    chain_.valid = false;
    UndoAction a = std::move(redo_.back());
    redo_.pop_back();
    const RectF changed = apply(a, true);
    const size_t page = a.page;
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
    const bool penBusy = gesture_ == Gesture::Draw || gesture_ == Gesture::Erase;
    if (settleAt_ && now >= settleAt_ && !penBusy) {
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
    if (modified_ && !penBusy && !settleAt_ && now >= modifiedSince_ + opts_.autosaveDelayMs) {
        if (!save(now, true)) modifiedSince_ = now;  // retry later rather than on every wakeup
    }
}

int Editor::msUntilIdleWork(uint64_t now) const {
    // Only deadlines onIdle() can act on right now; anything else would spin.
    // While the pen is down, its own events keep waking us up.
    const bool penBusy = gesture_ == Gesture::Draw || gesture_ == Gesture::Erase;
    uint64_t next = 0;
    auto consider = [&](uint64_t t) {
        if (t && (!next || t < next)) next = t;
    };
    if (!penBusy) consider(settleAt_);
    consider(statusUntil_);
    if (modified_ && !penBusy && !settleAt_) consider(std::max<uint64_t>(modifiedSince_ + opts_.autosaveDelayMs, 1));
    if (!next) return -1;
    return next <= now ? 0 : static_cast<int>(std::min<uint64_t>(next - now, 1u << 30));
}

}  // namespace scrible
