// Self-contained tests (no framework, so they also build with the Kindle toolchain).

#include <unistd.h>
#include <zlib.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "Editor.h"
#include "EinkPanel.h"
#include "Renderer.h"
#include "XoppFile.h"

using namespace xs;

namespace {

int g_failures = 0;

std::vector<std::pair<const char*, void (*)()>>& registry() {
    static std::vector<std::pair<const char*, void (*)()>> r;
    return r;
}

struct Register {
    Register(const char* name, void (*fn)()) { registry().emplace_back(name, fn); }
};

}  // namespace

#define TEST(name)                                \
    static void name();                           \
    static Register register_##name(#name, name); \
    static void name()

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                              \
        }                                                                              \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                       \
    do {                                                                                            \
        const double a_ = (a), b_ = (b);                                                            \
        if (std::fabs(a_ - b_) > (eps)) {                                                           \
            std::fprintf(stderr, "  %s:%d: %s is %g, expected %g\n", __FILE__, __LINE__, #a, a_, b_); \
            ++g_failures;                                                                           \
        }                                                                                           \
    } while (0)

namespace {

std::string tempPath(const std::string& name) {
    const char* dir = std::getenv("TMPDIR");
    std::string d = dir && *dir ? dir : "/tmp";
    if (d.back() != '/') d += '/';
    return d + "xscribe-test-" + std::to_string(getpid()) + "-" + name;
}

bool writeFile(const std::string& path, const std::string& data, bool gzip) {
    if (gzip) {
        gzFile f = gzopen(path.c_str(), "wb");
        if (!f) return false;
        const bool ok = gzwrite(f, data.data(), static_cast<unsigned>(data.size())) == static_cast<int>(data.size());
        return gzclose(f) == Z_OK && ok;
    }
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    return std::fclose(f) == 0 && ok;
}

const Stroke& strokeAt(const Document& d, size_t page, size_t layer, size_t i) {
    return std::get<Stroke>(d.pages[page].layers[layer].elements[i]);
}

bool hasMode(const std::vector<Damage>& ds, Refresh m) {
    for (const auto& d : ds)
        if (d.mode == m) return true;
    return false;
}

// Laid out the way Xournal++ 1.2/1.3 writes files.
const char* kXournalppFile = R"(<?xml version="1.0" standalone="no"?>
<xournal creator="xournalpp 1.2.8" fileversion="4">
<title>Xournal++ document - see https://xournalpp.github.io/</title>
<preview>iVBORw0KGgo=</preview>
<page width="595.27559055" height="841.88976378">
<background type="solid" color="#ffffffff" style="graph"/>
<layer name="Ink &amp; notes">
<stroke tool="pen" color="#3333ccff" width="1.41 1.2 1.5" capStyle="round">10 20 30 40 50 60</stroke>
<stroke tool="highlighter" color="#ffff007f" width="8.5" capStyle="round">100 100 200 100</stroke>
<text font="Sans" size="12" x="72" y="300" color="#000000ff">Hello
world</text>
<image left="300" top="400" right="400" bottom="450">iVBORw0KGgo=</image>
</layer>
<layer/>
</page>
<page width="595.27559055" height="841.88976378">
<background type="pdf" domain="absolute" filename="/home/me/paper.pdf" pageno="1"/>
<layer/>
</page>
</xournal>
)";

// The original Xournal's format: named colors, no fileversion.
const char* kXournalFile = R"(<?xml version="1.0" standalone="no"?>
<xournal version="0.4.8">
<title>Xournal document - see http://math.mit.edu/~auroux/software/xournal/</title>
<page width="612.00" height="792.00">
<background type="solid" color="yellow" style="lined" />
<layer>
<stroke tool="pen" color="blue" width="1.41">1 2 3 4</stroke>
<stroke tool="eraser" color="white" width="8.50">5 5 6 6</stroke>
</layer>
</page>
</xournal>
)";

// Draws a horizontal stroke in screen coordinates.
void drawLine(Editor& e, double x0, double x1, double y, uint64_t& t, bool eraser = false) {
    e.penDown({x0, y, 0.5, eraser}, t);
    for (double x = x0 + 5; x <= x1; x += 5) e.penMove({x, y, 0.5, eraser}, t += 4);
    e.penUp(t += 4);
}

IRect center(const IRect& r) { return {(r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2, 0, 0}; }

}  // namespace

// ---- file format ----

TEST(parses_xournalpp_file) {
    Document d;
    std::string err;
    std::vector<std::string> warnings;
    CHECK(parseXopp(kXournalppFile, d, err, &warnings));
    CHECK(warnings.empty());
    CHECK(d.pages.size() == 2);
    const Page& p = d.pages[0];
    CHECK_NEAR(p.width, 595.27559055, 1e-9);
    CHECK(p.background.style == "graph");
    CHECK(p.layers.size() == 2);
    CHECK(p.layers[0].name == "Ink & notes");
    CHECK(p.layers[0].elements.size() == 4);

    const Stroke& s = strokeAt(d, 0, 0, 0);
    CHECK(s.tool == StrokeTool::Pen);
    CHECK(s.color == 0x3333cc);
    CHECK_NEAR(s.width, 1.41, 1e-9);
    CHECK(s.points.size() == 3);
    CHECK(s.segmentWidths.size() == 2);
    CHECK_NEAR(s.segmentWidths[1], 1.5, 1e-9);
    CHECK(s.extraAttrs.size() == 1 && s.extraAttrs[0].first == "capStyle");

    const Stroke& h = strokeAt(d, 0, 0, 1);
    CHECK(h.tool == StrokeTool::Highlighter);
    CHECK(h.color == 0xffff00);
    CHECK(h.segmentWidths.empty());

    const auto& text = std::get<ForeignElement>(p.layers[0].elements[2]);
    CHECK(text.tag == "text");
    CHECK(text.text == "Hello\nworld");
    CHECK(text.bounds.valid);
    const auto& img = std::get<ForeignElement>(p.layers[0].elements[3]);
    CHECK(img.tag == "image");
    CHECK_NEAR(img.bounds.x1, 400, 1e-9);
    CHECK(d.pages[1].background.type == "pdf");
}

TEST(parses_fileversion5_text) {
    const char* xml = R"(<xournal fileversion="5"><page width="200" height="200"><layer>
<text font="Sans" size="10" matrix="2 0 0 2 30 40" color="#ff0000ff" wrap="45">one two three</text>
</layer></page></xournal>)";
    Document d;
    std::string err;
    CHECK(parseXopp(xml, d, err));
    const auto& t = std::get<ForeignElement>(d.pages[0].layers[0].elements.at(0));
    CHECK_NEAR(t.textX, 30, 1e-9);
    CHECK_NEAR(t.textSize, 20, 1e-9);
    CHECK(t.color == 0xff0000);
    CHECK(textLines(t) == (std::vector<std::string>{"one", "two", "three"}));
    CHECK_NEAR(t.bounds.x1, 30 + 5 * 20, 1e-9);
    CHECK(serializeXopp(d).find("fileversion=\"5\"") != std::string::npos);  // never downgraded
}

TEST(roundtrip_preserves_everything) {
    Document d;
    std::string err;
    CHECK(parseXopp(kXournalppFile, d, err));
    const std::string xml = serializeXopp(d);
    CHECK(xml.find("fileversion=\"4\"") != std::string::npos);
    CHECK(xml.find("name=\"Ink &amp; notes\"") != std::string::npos);
    CHECK(xml.find("capStyle=\"round\"") != std::string::npos);
    CHECK(xml.find("color=\"#ffff007f\"") != std::string::npos);  // highlighter alpha, like Xournal++
    CHECK(xml.find("width=\"1.41 1.2 1.5\"") != std::string::npos);
    CHECK(xml.find("filename=\"/home/me/paper.pdf\"") != std::string::npos);
    CHECK(xml.find("<image left=\"300\"") != std::string::npos);
    CHECK(xml.find("<preview") == std::string::npos);  // stale once we edit; Xournal++ regenerates it

    Document d2;
    CHECK(parseXopp(xml, d2, err));
    CHECK(d2.pages.size() == 2);
    CHECK(d2.pages[0].layers[0].elements.size() == 4);
    CHECK(strokeAt(d2, 0, 0, 0).points.size() == 3);
    CHECK(std::get<ForeignElement>(d2.pages[0].layers[0].elements[2]).text == "Hello\nworld");
    CHECK(serializeXopp(d2) == xml);  // stable across repeated saves
}

TEST(save_and_load_gzip) {
    Document d;
    std::string err;
    CHECK(parseXopp(kXournalppFile, d, err));
    const std::string path = tempPath("roundtrip.xopp");
    CHECK(saveXopp(path, d, err));

    FILE* f = std::fopen(path.c_str(), "rb");
    unsigned char magic[2] = {};
    CHECK(f && std::fread(magic, 1, 2, f) == 2);
    if (f) std::fclose(f);
    CHECK(magic[0] == 0x1f && magic[1] == 0x8b);
    CHECK(access((path + ".tmp").c_str(), F_OK) != 0);

    Document d2;
    CHECK(loadXopp(path, d2, err));
    CHECK(serializeXopp(d2) == serializeXopp(d));
    std::remove(path.c_str());
}

TEST(loads_original_xournal_files) {
    for (bool gzip : {true, false}) {
        const std::string path = tempPath(gzip ? "old.xoj" : "old.xml");
        CHECK(writeFile(path, kXournalFile, gzip));
        Document d;
        std::string err;
        CHECK(loadXopp(path, d, err));
        std::remove(path.c_str());
        if (d.pages.empty()) continue;
        CHECK(d.fileVersion == 1);
        CHECK(d.pages[0].background.color == 0xffff80);
        CHECK(strokeAt(d, 0, 0, 0).color == 0x3333cc);
        CHECK(strokeAt(d, 0, 0, 1).tool == StrokeTool::Eraser);
        // Written back as a Xournal++ file.
        CHECK(serializeXopp(d).find("fileversion=\"4\"") != std::string::npos);
    }
}

TEST(repairs_inconsistent_strokes) {
    const char* xml = R"(<xournal fileversion="4"><page width="100" height="100"><layer>
<stroke tool="pen" color="#000000ff" width="1 0.5 0.6 0.7 0.8 0.9">0 0 1 1 2 2</stroke>
<stroke tool="pen" color="#000000ff" width="1 0.5">0 0 1 1 2 2 3 3</stroke>
<stroke tool="pen" color="#000000ff" width="2">7 7</stroke>
<stroke tool="pen" color="#000000ff" width="2">1 2 3</stroke>
<stroke tool="pen" color="#000000ff" width="2"></stroke>
</layer></page></xournal>)";
    Document d;
    std::string err;
    std::vector<std::string> warnings;
    CHECK(parseXopp(xml, d, err, &warnings));
    const auto& els = d.pages[0].layers[0].elements;
    CHECK(els.size() == 4);  // the empty stroke is dropped
    CHECK(strokeAt(d, 0, 0, 0).points.size() == 3);
    CHECK(strokeAt(d, 0, 0, 0).segmentWidths.size() == 2);  // extra pressures discarded
    CHECK(strokeAt(d, 0, 0, 1).points.size() == 2);         // points without pressure discarded
    CHECK(strokeAt(d, 0, 0, 2).points.size() == 2);         // a dot becomes two equal points
    CHECK(strokeAt(d, 0, 0, 3).points.size() == 2);
    CHECK(warnings.size() == 2);
}

TEST(rejects_unsupported_files) {
    const std::string path = tempPath("packaged.xopp");
    CHECK(writeFile(path, std::string("PK\x03\x04rest-of-zip", 16), false));
    Document d;
    std::string err;
    CHECK(!loadXopp(path, d, err));
    CHECK(err.find("zip") != std::string::npos);
    std::remove(path.c_str());

    CHECK(!parseXopp("<html/>", d, err));
    CHECK(!parseXopp("<xournal><page", d, err));
    CHECK(!loadXopp(tempPath("does-not-exist.xopp"), d, err));
}

// ---- rendering ----

TEST(renders_antialiased_pen_stroke) {
    Canvas c(200, 100);
    Page p = makeBlankPage(200, 100, "plain");
    Stroke s;
    s.width = 10;
    s.points = {{20, 50.25}, {180, 50.25}};
    p.layers[0].elements.push_back(s);
    drawPage(c, p, PageTransform{}, c.rect());
    CHECK(c.at(100, 50) == 0);
    CHECK(c.at(100, 54) == 0);
    CHECK(c.at(100, 55) > 0 && c.at(100, 55) < 255);  // partial coverage at the edge
    CHECK(c.at(100, 30) == 255);
    CHECK(c.at(5, 50) == 255);
}

TEST(hard_edged_ink_matches_live_rendering) {
    // On e-ink the stroke drawn while writing (refreshed with black/white DU) must
    // already be the final stroke, or it visibly changes when the pen lifts.
    const RenderStyle hard{false};
    PageTransform t;
    t.scale = 300.0 / 72.0;  // the Scribe's scale for screen-sized pages
    Page p = makeBlankPage(100, 60, "plain");
    Stroke s;
    s.width = 1.41;
    for (int i = 0; i <= 40; ++i) {
        s.points.push_back({10 + i * 2.0, 30 + 12 * std::sin(i * 0.4)});
        if (i) s.segmentWidths.push_back(0.85 + 0.03 * i);
    }
    const int w = static_cast<int>(100 * t.scale), h = static_cast<int>(60 * t.scale);
    Canvas live(w, h), final(w, h);
    for (size_t i = 0; i + 1 < s.points.size(); ++i) drawLiveSegment(live, s, i, t, live.rect(), hard);
    p.layers[0].elements.push_back(s);
    drawPage(final, p, t, final.rect(), hard);

    int differing = 0, gray = 0, ink = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            differing += live.at(x, y) != final.at(x, y);
            gray += final.at(x, y) != 0 && final.at(x, y) != 255;
            ink += final.at(x, y) == 0;
        }
    }
    CHECK(differing == 0);
    CHECK(gray == 0);
    CHECK(ink > 1000);
}

TEST(highlighter_darkens_without_hiding_ink) {
    Canvas c(200, 200);
    Page p = makeBlankPage(200, 200, "plain");
    Stroke ink;
    ink.width = 4;
    ink.points = {{100, 20}, {100, 180}};
    Stroke hl;
    hl.tool = StrokeTool::Highlighter;
    hl.color = 0xffff00;
    hl.width = 20;
    hl.points = {{20, 100}, {60, 100}, {180, 100}};
    p.layers[0].elements = {ink, hl};
    drawPage(c, p, PageTransform{}, c.rect());
    CHECK(c.at(100, 100) == 0);  // ink under the highlighter stays black
    const uint8_t g = c.at(40, 100);
    CHECK(g > 100 && g <= 200);
    CHECK(c.at(60, 100) == g);  // no darker blob where segments join
}

TEST(draws_ruled_background) {
    Canvas c(200, 200);
    const Page p = makeBlankPage(200, 200, "lined");
    drawPage(c, p, PageTransform{}, c.rect());
    CHECK(c.at(100, 80) == 200);  // first ruling line at 80pt
    CHECK(c.at(100, 104) == 200);
    CHECK(c.at(100, 90) == 255);
    CHECK(c.at(72, 150) == 170);  // margin line
}

TEST(toolbar_layout_fits) {
    for (int width : {600, 1236, 1860}) {
        Toolbar tb(width, 120);
        int lastRight = 0;
        for (Action a : {Action::Pen, Action::Highlighter, Action::Eraser, Action::SizeFine, Action::SizeMedium,
                         Action::SizeThick, Action::Undo, Action::Redo, Action::PrevPage, Action::NextPage,
                         Action::Save, Action::Exit}) {
            const IRect r = tb.buttonRect(a);
            CHECK(!r.empty());
            CHECK(r.x0 >= lastRight && r.x1 <= width);
            CHECK(tb.hit(center(r).x0, 5) == a);
            lastRight = r.x1;
        }
    }
}

// ---- editor ----

TEST(editor_draws_pressure_strokes_and_settles) {
    Editor e(900, 1200, Document{}, tempPath("draw.xopp"));
    CHECK(e.document().pages.size() == 1);
    auto dmg = e.takeDamage();
    CHECK(dmg.size() == 1 && dmg[0].mode == Refresh::Full);

    uint64_t t = 1000;
    e.penDown({100, 400, 0.2, false}, t);
    for (int i = 1; i <= 20; ++i) e.penMove({100.0 + i * 10, 400, 0.2 + i * 0.03, false}, t += 5);
    e.penUp(t += 5);

    const auto& els = e.document().pages[0].layers.back().elements;
    CHECK(els.size() == 1);
    const Stroke& s = std::get<Stroke>(els[0]);
    CHECK(s.points.size() == 21);
    CHECK(s.segmentWidths.size() == 20);
    CHECK(s.segmentWidths.back() > s.segmentWidths.front());
    CHECK(e.modified());
    CHECK(e.canvas().at(200, 400) < 128);
    dmg = e.takeDamage();
    CHECK(hasMode(dmg, Refresh::Ink));

    CHECK(e.msUntilIdleWork(t) > 0);
    e.onIdle(t + 600);
    CHECK(hasMode(e.takeDamage(), Refresh::Content));
}

TEST(editor_pressure_curve) {
    // 0.5 (also what the Kindle reports when the pen has no pressure) is the preset
    // width; no pressure at all still leaves a clearly visible line.
    Editor e(900, 1200, Document{}, tempPath("curve.xopp"));
    uint64_t t = 0;
    for (double p : {0.5, 0.0, 1.0}) {
        e.penDown({100, 300 + p * 400, p, false}, t += 10);
        e.penMove({300, 300 + p * 400, p, false}, t += 10);
        e.penUp(t += 10);
    }
    const auto& els = e.document().pages[0].layers.back().elements;
    CHECK(els.size() == 3);
    CHECK_NEAR(std::get<Stroke>(els[0]).segmentWidths.at(0), 1.41, 1e-9);
    CHECK_NEAR(std::get<Stroke>(els[1]).segmentWidths.at(0), 1.41 * 0.6, 1e-9);
    CHECK_NEAR(std::get<Stroke>(els[2]).segmentWidths.at(0), 1.41 * 1.4, 1e-9);
}

TEST(editor_tap_makes_a_dot) {
    Editor e(900, 1200, Document{}, tempPath("dot.xopp"));
    e.penDown({300, 500, 0.5, false}, 10);
    e.penUp(20);
    const Stroke& s = std::get<Stroke>(e.document().pages[0].layers.back().elements.at(0));
    CHECK(s.points.size() == 2);
    CHECK(s.segmentWidths.size() == 1);
    CHECK(e.canvas().at(300, 500) < 128);
}

TEST(editor_undo_redo) {
    Editor e(900, 1200, Document{}, tempPath("undo.xopp"));
    uint64_t t = 0;
    drawLine(e, 100, 400, 500, t);
    const auto& els = e.document().pages[0].layers.back().elements;
    CHECK(els.size() == 1);
    e.perform(Action::Undo, t += 10);
    CHECK(els.empty());
    CHECK(e.canvas().at(250, 500) > 200);
    e.perform(Action::Redo, t += 10);
    CHECK(els.size() == 1);
    CHECK(e.canvas().at(250, 500) < 128);
}

TEST(editor_erases_whole_strokes_and_undo_restores_order) {
    Editor e(900, 1200, Document{}, tempPath("erase.xopp"));
    uint64_t t = 0;
    drawLine(e, 100, 600, 400, t);
    drawLine(e, 100, 600, 500, t);
    drawLine(e, 100, 600, 600, t);
    const auto& els = e.document().pages[0].layers.back().elements;
    CHECK(els.size() == 3);
    const double middleY = std::get<Stroke>(els[1]).points[0].y;

    // Eraser end of the pen, swiped across the middle stroke only.
    e.penDown({300, 495, 0.5, true}, t += 10);
    e.penMove({350, 505, 0.5, true}, t += 10);
    e.penUp(t += 10);
    CHECK(els.size() == 2);
    CHECK(e.tool() == ToolKind::Pen);  // the pen's eraser end doesn't change the selected tool

    e.perform(Action::Undo, t += 10);
    CHECK(els.size() == 3);
    CHECK_NEAR(std::get<Stroke>(els[1]).points[0].y, middleY, 1e-9);
    e.perform(Action::Redo, t += 10);
    CHECK(els.size() == 2);
}

TEST(editor_toolbar_and_palm_rejection) {
    Editor e(900, 1200, Document{}, tempPath("toolbar.xopp"));
    const IRect eraser = center(e.toolbar().buttonRect(Action::Eraser));
    const IRect pen = center(e.toolbar().buttonRect(Action::Pen));

    e.touchDown(eraser.x0, eraser.y0, 10);
    CHECK(e.tool() == ToolKind::Eraser);
    e.penDown({double(pen.x0), double(pen.y0), 0.5, false}, 20);
    e.penUp(30);
    CHECK(e.tool() == ToolKind::Pen);
    CHECK(e.document().pages[0].layers.back().elements.empty());  // toolbar taps don't draw

    e.penDown({300, 500, 0.5, false}, 40);
    e.touchDown(eraser.x0, eraser.y0, 45);  // palm on the toolbar while writing
    e.penUp(50);
    CHECK(e.tool() == ToolKind::Pen);
}

TEST(editor_pages) {
    Editor e(900, 1200, Document{}, tempPath("pages.xopp"));
    e.takeDamage();
    e.perform(Action::NextPage, 10);  // past the last page: appends one
    CHECK(e.document().pages.size() == 2);
    CHECK(e.pageIndex() == 1);
    CHECK(hasMode(e.takeDamage(), Refresh::Full));
    e.perform(Action::PrevPage, 20);
    CHECK(e.pageIndex() == 0);
    e.perform(Action::NextPage, 30);  // existing page: just turns
    CHECK(e.document().pages.size() == 2);
    e.perform(Action::Undo, 40);
    CHECK(e.document().pages.size() == 1);
    CHECK(e.pageIndex() == 0);
    e.perform(Action::Redo, 50);
    CHECK(e.document().pages.size() == 2);
    CHECK(e.pageIndex() == 1);
}

TEST(editor_saves_on_exit) {
    const std::string path = tempPath("exit.xopp");
    {
        Editor e(900, 1200, Document{}, path);
        uint64_t t = 0;
        drawLine(e, 100, 300, 400, t);
        e.perform(Action::Exit, t += 10);
        CHECK(e.quitRequested());
        CHECK(!e.modified());
    }
    Document d;
    std::string err;
    CHECK(loadXopp(path, d, err));
    CHECK(d.pages.size() == 1 && d.pages[0].layers.back().elements.size() == 1);
    std::remove(path.c_str());
}

TEST(editor_exit_after_failed_save_needs_second_tap) {
    Editor e(900, 1200, Document{}, "/nonexistent-dir/notes.xopp");
    uint64_t t = 0;
    drawLine(e, 100, 300, 400, t);
    e.perform(Action::Exit, 1000);
    CHECK(!e.quitRequested());
    e.perform(Action::Exit, 2000);
    CHECK(e.quitRequested());
}

TEST(editor_autosaves_when_idle) {
    const std::string path = tempPath("autosave.xopp");
    EditorOptions o;
    o.autosaveDelayMs = 1000;
    Editor e(900, 1200, Document{}, path, o);
    uint64_t t = 1000;
    drawLine(e, 100, 300, 400, t);
    e.onIdle(t + o.settleDelayMs);  // settle first
    CHECK(e.modified());
    const int wait = e.msUntilIdleWork(t + o.settleDelayMs);
    CHECK(wait > 0 && wait <= 1000);
    e.onIdle(t + 2000);
    CHECK(!e.modified());
    CHECK(access(path.c_str(), F_OK) == 0);
    std::remove(path.c_str());
}

TEST(editor_opens_existing_document) {
    Document d;
    std::string err;
    CHECK(parseXopp(kXournalppFile, d, err));
    Editor e(900, 1200, std::move(d), tempPath("existing.xopp"));
    CHECK(e.document().pages.size() == 2);
    uint64_t t = 0;
    drawLine(e, 100, 300, 600, t);
    CHECK(e.document().pages[0].layers.back().elements.size() == 1);  // drawn on the top (empty) layer
    CHECK(e.document().pages[0].layers[0].elements.size() == 4);
}

// ---- what the e-ink panel ends up showing ----

namespace {

int panelMismatches(const EinkPanel& panel, const Canvas& c) {
    int n = 0;
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x) n += panel.shown().at(x, y) != c.at(x, y);
    return n;
}

}  // namespace

TEST(eink_panel_model) {
    Canvas c(4, 1);
    EinkPanel panel(4, 1);
    c.row(0)[0] = 0;
    c.row(0)[1] = 128;
    panel.refresh(c, {c.rect(), Refresh::Ink});
    CHECK(panel.shown().at(0, 0) == 0);
    CHECK(panel.shown().at(1, 0) == 255);  // DU leaves non-B&W targets as they were
    panel.refresh(c, {c.rect(), Refresh::Content});
    CHECK(panel.shown().at(1, 0) == 255);  // ...and the controller now thinks it's done
    panel.refresh(c, {c.rect(), Refresh::Full});
    CHECK(panel.shown().at(1, 0) == 128);  // only a flashing update forces it
}

TEST(eink_panel_shows_what_the_editor_drew) {
    // Writing across rule lines, erasing, the cleanup refresh, and undo must leave
    // the panel showing exactly the canvas, with device settings (no antialiasing).
    EditorOptions o;
    o.render.antialias = false;
    Editor e(900, 1200, Document{}, tempPath("panel.xopp"), o);
    EinkPanel panel(900, 1200);
    auto flush = [&] {
        for (const Damage& d : e.takeDamage()) panel.refresh(e.canvas(), d);
    };
    flush();

    uint64_t t = 1000;
    for (double x : {300.0, 400.0, 500.0}) {  // vertical strokes across several rule lines
        e.penDown({x, 400, 0.5, false}, t += 10);
        flush();
        for (double y = 410; y <= 900; y += 10) {
            e.penMove({x, y, 0.5, false}, t += 4);
            flush();
        }
        e.penUp(t += 4);
        flush();
    }
    e.onIdle(t += 1000);
    flush();
    CHECK_NEAR(panelMismatches(panel, e.canvas()), 0, 0);

    e.penDown({250, 650, 0.5, true}, t += 10);  // eraser end, across all three strokes
    flush();
    for (double x = 260; x <= 550; x += 10) {
        e.penMove({x, 650, 0.5, true}, t += 4);
        flush();
    }
    e.penUp(t += 4);
    flush();
    e.onIdle(t += 1000);
    flush();
    CHECK(e.document().pages[0].layers.back().elements.empty());
    CHECK_NEAR(panelMismatches(panel, e.canvas()), 0, 0);

    for (int i = 0; i < 4; ++i) {  // undo the erase, then all three strokes
        e.perform(Action::Undo, t += 10);
        flush();
    }
    e.onIdle(t += 1000);
    flush();
    CHECK_NEAR(panelMismatches(panel, e.canvas()), 0, 0);
}

int main() {
    for (const auto& [name, fn] : registry()) {
        const int before = g_failures;
        fn();
        std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", name);
    }
    std::printf("%zu tests, %d failed checks\n", registry().size(), g_failures);
    return g_failures ? 1 : 0;
}
