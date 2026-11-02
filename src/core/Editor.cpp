#include "Editor.h"

#include <cstdio>

#include "XoppFile.h"

namespace xs {
namespace {

constexpr uint8_t kOffPageGray = 220;
constexpr double kMinPointDistPx = 0.75;  // closer samples are dropped
constexpr size_t kMaxUndo = 1000;

// Preset widths (points) of the original Xournal, indexed by ToolSize.
constexpr double kPenWidths[] = {0.85, 1.41, 2.26};
constexpr double kHighlighterWidths[] = {2.83, 8.50, 19.84};
constexpr double kEraserWidths[] = {2.83, 8.50, 19.84};

constexpr uint32_t kPenColor = 0x000000;
constexpr uint32_t kHighlighterColor = 0xffff00;  // e-ink shows gray; the desktop shows yellow

bool strokeHit(const Stroke& s, double x, double y, double r) {
    RectF probe;
    probe.add(x, y);
    probe.grow(r);
    if (!s.bounds().intersects(probe)) return false;
    for (size_t i = 0; i + 1 < s.points.size(); ++i) {
        const double reach = r + s.segmentWidth(i) / 2;
        const auto& a = s.points[i];
        const auto& b = s.points[i + 1];
        if (distSqToSegment(x, y, a.x, a.y, b.x, b.y) <= reach * reach) return true;
    }
    return false;
}

}  // namespace

Editor::Editor(int screenWidth, int screenHeight, Document doc, std::string path, EditorOptions opts)
    : opts_(std::move(opts)),
      canvas_(screenWidth, screenHeight),
      toolbar_(screenWidth, opts_.toolbarHeight),
      pageArea_{0, opts_.toolbarHeight, screenWidth, screenHeight},
      doc_(std::move(doc)),
      path_(std::move(path)) {
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
    return {tool_, size_, !undo_.empty(), !redo_.empty(), modified_, page_, doc_.pages.size(), status_};
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
        pendingErase_ = UndoAction{UndoAction::Kind::Erase, page_, currentLayer(), {}, std::nullopt};
        eraseAt(s);
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
        eraseAt(s);
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
    } else if (gesture_ == Gesture::Erase && !pendingErase_.items.empty()) {
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
    elements.emplace_back(std::move(live_));
    live_ = Stroke{};
    pushUndo({UndoAction::Kind::AddStroke, page_, layer, {{elements.size() - 1, Stroke{}}}, std::nullopt});
    markModified(now);
    scheduleSettle(xf_.toScreen(bounds), now);
}

void Editor::eraseAt(const PenSample& s) {
    const double x = xf_.toPageX(s.x), y = xf_.toPageY(s.y), r = eraserWidth() / 2;
    auto& elements = currentPage().layers[pendingErase_.layer].elements;
    IRect dirty;
    for (size_t i = 0; i < elements.size();) {
        const auto* st = std::get_if<Stroke>(&elements[i]);
        if (st && strokeHit(*st, x, y, r)) {
            dirty = dirty.united(xf_.toScreen(st->bounds()));
            pendingErase_.items.emplace_back(i, std::move(elements[i]));
            elements.erase(elements.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    if (dirty.empty()) return;
    // Not the fast DU: the page under erased ink isn't only white (rule lines are
    // gray), and DU leaves pixels with gray targets showing the old black.
    repaint(dirty, Refresh::Content);
}

// ---- actions ----

void Editor::perform(Action a, uint64_t now) {
    switch (a) {
        case Action::Pen: tool_ = ToolKind::Pen; break;
        case Action::Highlighter: tool_ = ToolKind::Highlighter; break;
        case Action::Eraser: tool_ = ToolKind::Eraser; break;
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
            if (!modified_ || now < exitArmedUntil_ || save(now)) {
                quit_ = true;
            } else {
                // Save failed: a second EXIT within a few seconds leaves without saving.
                exitArmedUntil_ = now + 4000;
                setStatus("UNSAVED", now, 4000);
            }
            return;
        case Action::None: return;
    }
    updateToolbar();
}

void Editor::setPage(size_t index) {
    page_ = index;
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
        std::fprintf(stderr, "xscribe: saving %s failed: %s\n", path_.c_str(), error.c_str());
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
    const bool removing = (a.kind == UndoAction::Kind::Erase) == forward;
    if (removing) {
        // Removals replay in their original order.
        for (auto& [index, element] : a.items) {
            element = take(els, index);
            unite(element);
        }
    } else {
        // Re-insertions go in reverse order, restoring the exact original positions.
        for (auto it = a.items.rbegin(); it != a.items.rend(); ++it) {
            unite(it->second);
            put(els, it->first, std::move(it->second));
        }
    }
    return changed;
}

void Editor::undo(uint64_t now) {
    if (undo_.empty() || gesture_ == Gesture::Draw || gesture_ == Gesture::Erase) return;
    UndoAction a = std::move(undo_.back());
    undo_.pop_back();
    const RectF changed = apply(a, false);
    const size_t page = a.kind == UndoAction::Kind::AddPage ? std::min(a.page - 1, doc_.pages.size() - 1) : a.page;
    redo_.push_back(std::move(a));
    afterHistoryChange(page, changed, now);
}

void Editor::redo(uint64_t now) {
    if (redo_.empty() || gesture_ == Gesture::Draw || gesture_ == Gesture::Erase) return;
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

}  // namespace xs
