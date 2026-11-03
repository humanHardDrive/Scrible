#pragma once

// Platform-independent editor: owns the document and the screen canvas, turns
// pen/touch input into document edits, and reports which screen regions changed
// and how they should be refreshed. The Kindle and desktop front ends only move
// input in and pixels out.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Canvas.h"
#include "Document.h"
#include "PaperMenu.h"
#include "Renderer.h"
#include "ToolPanel.h"
#include "Toolbar.h"

namespace scrible {

// How a changed region should be pushed to the e-ink panel.
// DU only draws pure black and white: a pixel whose new value is gray keeps
// showing its old value (and a later partial update won't fix it, since the
// controller believes it's already up to date). So only content that is purely
// black/white may go out as Ink or UI; everything else must be Content or Full.
enum class Refresh {
    Ink,      // stroke being written: fastest black/white waveform (DU)
    UI,       // toolbar feedback, black/white only: fast (DU)
    Content,  // any change, accurate grays; only changed pixels flicker (GC16, partial)
    Full,     // whole screen with a flash, clears ghosting (GC16, full)
};

struct Damage {
    IRect rect;
    Refresh mode;
};

struct PenSample {
    double x = 0, y = 0;    // screen pixels
    double pressure = 0.5;  // 0..1; 0.5 gives the preset width (use it when the pen has no pressure)
    bool eraser = false;    // eraser end of the pen
    bool button = false;    // side button held: highlights whatever the tool, like the native notebook
};

enum class ToolKind { Pen, Highlighter, Eraser, Lasso };

struct EditorOptions {
    RenderStyle render;  // the Kindle turns antialiasing off (see RenderStyle)
    // Opened from the notebook list: the toolbar then has BACK (to the list) and
    // EXIT (leave the app); otherwise just EXIT.
    bool fromList = false;
    bool eraseWholeStrokes = false;  // eraser mode to start in
    bool recognizeShapes = false;    // pen mode to start in (SHAPE rather than PEN)
    std::array<ToolSize, 3> sizes{ToolSize::Medium, ToolSize::Medium, ToolSize::Medium};  // pen, highlighter, eraser
    int toolbarHeight = 120;
    double dpi = 300;  // sizes new notebooks so a page fills the screen
    std::string pageStyle = "lined";
    uint64_t settleDelayMs = 600;  // pen rest time before the grayscale cleanup refresh
    uint64_t autosaveDelayMs = 30000;
};

class Editor {
public:
    Editor(int screenWidth, int screenHeight, Document doc, std::string path, EditorOptions opts = EditorOptions());

    Canvas& canvas() { return canvas_; }
    const Document& document() const { return doc_; }
    const Toolbar& toolbar() const { return toolbar_; }
    const ToolPanel& toolPanel() const { return panel_; }
    const PaperMenu& paperMenu() const { return paperMenu_; }
    const std::string& path() const { return path_; }
    size_t pageIndex() const { return page_; }
    ToolKind tool() const { return tool_; }
    bool modified() const { return modified_; }
    bool eraseWholeStrokes() const { return eraseWhole_; }
    bool recognizeShapes() const { return shapes_; }
    ToolSize toolSize(ToolKind t) const { return t == ToolKind::Lasso ? ToolSize::Medium : sizes_[static_cast<size_t>(t)]; }

    // Lasso selection (strokes of the top layer), and its on-screen frame, resize
    // handle and buttons; empty when nothing is selected.
    const std::vector<size_t>& selection() const { return sel_.indices; }
    IRect selectionFrame() const;
    IRect selectionHandle() const;
    IRect selectionButton(Action a) const;
    // BACK or EXIT was tapped and the notebook is saved (or the user chose to leave
    // it unsaved). quitRequested() tells EXIT (leave the app) from BACK.
    bool closeRequested() const { return close_; }
    bool quitRequested() const { return close_ && quit_; }

    void redrawAll(Refresh mode = Refresh::Full);

    // Input. `now` is a monotonic clock in milliseconds.
    void penDown(const PenSample& s, uint64_t now);
    void penMove(const PenSample& s, uint64_t now);
    void penUp(uint64_t now);
    void touchDown(int x, int y, uint64_t now);  // fingers only operate the toolbar (palm rejection)
    void perform(Action a, uint64_t now);

    bool save(uint64_t now, bool quiet = false);

    // Deferred work: grayscale cleanup after the pen rests, status timeout, autosave.
    void onIdle(uint64_t now);
    int msUntilIdleWork(uint64_t now) const;  // -1 when nothing is scheduled

    std::vector<Damage> takeDamage();

private:
    // One step of a change to a layer. Replaying the steps in order redoes the
    // change; undoing them in reverse order restores the exact previous state.
    // `element` holds whatever is currently out of the document: the removed
    // element of an applied removal, or the element of an undone insertion.
    struct ElementEdit {
        bool insert;
        size_t index;
        Element element;
    };
    struct UndoAction {
        enum class Kind { Edit, AddPage, DeletePage, Background } kind = Kind::Edit;
        size_t page = 0;
        size_t layer = 0;
        std::vector<ElementEdit> edits;  // Edit: drawing a stroke, or an eraser gesture
        std::optional<Page> pageData;    // AddPage: filled while undone; DeletePage: while done
        Background background{};         // Background: the page's other one (swapped in and out)
    };
    enum class Gesture { None, Draw, Erase, Toolbar, Lasso, Drag };
    enum class DragKind { Move, Resize };
    struct OverlayButton {
        Action action;
        std::string label;
        IRect r;
    };
    bool penBusy() const { return gesture_ != Gesture::None && gesture_ != Gesture::Toolbar; }

    Page& currentPage() { return doc_.pages[page_]; }
    size_t currentLayer();
    IRect drawingClip() const;
    double toolWidth() const;
    double eraserWidth() const;
    double pointWidth(const PenSample& s) const;

    void setPage(size_t index);
    void addPage(uint64_t now);
    void eraseAt(const PenSample& s, uint64_t now);
    void previewArea(const IRect& r, uint64_t now);

    void select(std::vector<size_t> indices);
    void clearSelection();
    RectF selectedBounds() const;
    void layoutOverlay();
    IRect overlayArea() const;
    void drawOverlay(const IRect& clip);
    void lassoTo(const StrokePoint& p);
    void finishLasso();
    void startDrag(DragKind kind, const StrokePoint& p);
    void dragTo(const StrokePoint& p);
    void dragFrame(uint64_t now);
    void finishDrag(uint64_t now);
    void deleteSelection(uint64_t now);
    void duplicateSelection(uint64_t now);
    void commitStroke(uint64_t now);
    void recognize(size_t layer);
    void pushUndo(UndoAction a);
    void undo(uint64_t now);
    void redo(uint64_t now);
    RectF apply(UndoAction& a, bool forward);
    void afterHistoryChange(size_t page, const RectF& changed, uint64_t now);

    void repaint(const IRect& r, Refresh mode);
    void drawChrome(const IRect& clip);
    ToolbarState toolbarState() const;
    ToolPanelState panelState() const;
    void refreshPanel(const std::vector<IRect>& before);
    void closePanel();
    void keepPanelOnTop(const IRect& r);
    PaperMenuState paperMenuState() const;
    void openPaperMenu();
    void changePaper(Action a, uint64_t now);
    void closePaperMenu();
    void deletePage(uint64_t now);
    void updateToolbar();
    void markModified(uint64_t now);
    void setStatus(const std::string& text, uint64_t now, uint64_t durationMs = 1500);
    void penActivity(uint64_t now);
    void scheduleSettle(const IRect& r, uint64_t now);

    EditorOptions opts_;
    Canvas canvas_;
    Toolbar toolbar_;
    IRect pageArea_;
    ToolPanel panel_;
    PaperMenu paperMenu_;
    Background paperBefore_;       // the page's paper when the menu opened
    bool deleteArmed_ = false;     // DELETE PAGE tapped once
    bool closePanelOnUp_ = false;  // the pen came down beside the open panel or paper menu
    Document doc_;
    std::string path_;
    size_t page_ = 0;
    PageTransform xf_;

    ToolKind tool_ = ToolKind::Pen;
    ToolKind inkTool_ = ToolKind::Pen;  // what the stroke being written is drawn with
    std::array<ToolSize, 3> sizes_;  // indexed by ToolKind
    bool eraseWhole_ = false;  // eraser removes whole strokes instead of cutting them
    bool shapes_ = false;      // pen strokes are recognized as shapes
    // Lines recognized one after another, which may still close into a polygon or
    // get an arrowhead. They are the last `lines` elements of the layer.
    struct ShapeChain {
        bool valid = false;
        size_t page = 0, layer = 0;
        size_t last = 0;  // element index of the newest line
        size_t lines = 0;
        std::vector<StrokePoint> points;
    };
    ShapeChain chain_;

    struct Selection {
        bool active = false;
        size_t layer = 0;
        std::vector<size_t> indices;  // ascending
        RectF bounds;
    };
    Selection sel_;
    std::vector<OverlayButton> overlayButtons_;
    std::vector<StrokePoint> lasso_;  // the loop being drawn (page coordinates)
    IRect lassoArea_;
    double lassoDots_ = 0;  // distance since the last dot of the dotted outline
    DragKind dragKind_ = DragKind::Move;
    StrokePoint dragStart_{0, 0};
    std::vector<Stroke> dragOriginals_;  // the selection as it was when the drag started
    RectF dragFrom_;
    IRect dragDirty_;
    uint64_t lastDragFrame_ = 0;
    Gesture gesture_ = Gesture::None;
    Stroke live_;
    std::vector<double> livePointWidths_;
    UndoAction pendingErase_;
    std::optional<StrokePoint> eraserLast_;  // previous eraser position in this gesture

    std::vector<UndoAction> undo_, redo_;
    bool modified_ = false;
    uint64_t modifiedSince_ = 0;

    IRect settle_;
    uint64_t settleAt_ = 0;  // 0 = nothing pending
    std::string status_;
    uint64_t statusUntil_ = 0;
    uint64_t exitArmedUntil_ = 0;
    bool close_ = false;
    bool quit_ = false;

    std::vector<Damage> damage_;
};

}  // namespace scrible
