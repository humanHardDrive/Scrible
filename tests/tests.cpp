// Self-contained tests (no framework, so they also build with the Kindle toolchain).

#include <dirent.h>
#include <sys/time.h>
#include <unistd.h>
#include <zlib.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "About.h"
#include "App.h"
#include "DigitReader.h"
#include "Editor.h"
#include "EinkPanel.h"
#include "Eraser.h"
#include "Lasso.h"
#include "Paper.h"
#include "Shapes.h"
#include "Renderer.h"
#include "XoppFile.h"

using namespace scrible;

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
    return d + "scrible-test-" + std::to_string(getpid()) + "-" + name;
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
    for (int width : {1072, 1236, 1860, 1986}) {  // Kindle screen widths
        const Editor e(width, 1200, Document{}, tempPath("toolbar-fit.xopp"));
        const Toolbar& tb = e.toolbar();
        int lastRight = 0;
        for (Action a : {Action::Undo, Action::Redo, Action::PageStyle, Action::PrevPage, Action::NextPage, Action::Save,
                         Action::Exit}) {
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

TEST(eraser_cuts_strokes) {
    Stroke s;
    s.width = 2;
    for (int x = 0; x <= 100; x += 10) s.points.push_back({double(x), 0});
    for (int i = 0; i < 10; ++i) s.segmentWidths.push_back(1 + i * 0.1);
    s.extraAttrs = {{"capStyle", "round"}};
    std::vector<Stroke> pieces;

    // Missing it entirely.
    CHECK(!eraseFromStroke(s, {50, 20}, {50, 20}, 5, pieces));

    // A dot of radius 5 at x=50: cut where the centreline is within 5 + width/2.
    CHECK(eraseFromStroke(s, {50, 0}, {50, 0}, 5, pieces));
    CHECK(pieces.size() == 2);
    if (pieces.size() == 2) {
        const double w = s.segmentWidth(4);  // segment 40..50
        CHECK_NEAR(pieces[0].points.front().x, 0, 1e-9);
        CHECK_NEAR(pieces[0].points.back().x, 50 - (5 + w / 2), 1e-6);
        CHECK_NEAR(pieces[1].points.front().x, 50 + (5 + s.segmentWidth(5) / 2), 1e-6);
        CHECK_NEAR(pieces[1].points.back().x, 100, 1e-9);
        for (const Stroke& p : pieces) {
            CHECK(p.segmentWidths.size() + 1 == p.points.size());  // pressure kept per segment
            CHECK(p.extraAttrs == s.extraAttrs);
            CHECK_NEAR(p.width, 2, 1e-9);
        }
        CHECK_NEAR(pieces[1].segmentWidths.back(), s.segmentWidths.back(), 1e-9);
    }

    // A swipe across the stroke cuts it once, however fast the pen moved.
    CHECK(eraseFromStroke(s, {30, -40}, {70, 40}, 3, pieces));
    CHECK(pieces.size() == 2);
    for (const Stroke& p : pieces) {
        for (size_t i = 0; i + 1 < p.points.size(); ++i) {
            const double reach = 3 + p.segmentWidth(i) / 2;
            const double mid = (p.points[i].x + p.points[i + 1].x) / 2;
            CHECK(distSqToSegment(mid, 0, 30, -40, 70, 40) >= reach * reach - 1e-6);
        }
    }

    // Erasing over the start, the end, or all of it.
    CHECK(eraseFromStroke(s, {0, 0}, {0, 0}, 5, pieces) && pieces.size() == 1);
    CHECK(eraseFromStroke(s, {100, 0}, {100, 0}, 5, pieces) && pieces.size() == 1);
    CHECK(eraseFromStroke(s, {0, 0}, {100, 0}, 5, pieces) && pieces.empty());

    // A dot stroke (two equal points) goes entirely.
    Stroke dot;
    dot.points = {{10, 10}, {10, 10}};
    CHECK(eraseFromStroke(dot, {11, 10}, {11, 10}, 2, pieces) && pieces.empty());
}

TEST(editor_eraser_cuts_strokes_and_undo_restores_them) {
    Editor e(900, 1200, Document{}, tempPath("erase.xopp"));
    uint64_t t = 0;
    drawLine(e, 100, 600, 400, t);
    drawLine(e, 100, 600, 500, t);
    drawLine(e, 100, 600, 600, t);
    const auto& els = e.document().pages[0].layers.back().elements;
    CHECK(els.size() == 3);
    const Stroke middle = std::get<Stroke>(els[1]);

    // Eraser end of the pen, swiped across the middle stroke only: it's cut in two,
    // in place, so the stacking order stays the same.
    e.penDown({300, 470, 0.5, true}, t += 10);
    e.penMove({320, 530, 0.5, true}, t += 10);
    e.penUp(t += 10);
    CHECK(els.size() == 4);
    CHECK(e.tool() == ToolKind::Pen);  // the pen's eraser end doesn't change the selected tool
    if (els.size() == 4) {
        CHECK_NEAR(std::get<Stroke>(els[1]).points.front().y, middle.points.front().y, 1e-9);
        CHECK_NEAR(std::get<Stroke>(els[2]).points.back().y, middle.points.back().y, 1e-9);
        CHECK(std::get<Stroke>(els[1]).points.back().x < std::get<Stroke>(els[2]).points.front().x);
    }
    CHECK(e.canvas().at(310, 500) > 200);  // gone from the screen too
    CHECK(e.canvas().at(200, 500) < 128);

    e.perform(Action::Undo, t += 10);
    CHECK(els.size() == 3);
    if (els.size() == 3) {
        const Stroke& restored = std::get<Stroke>(els[1]);
        CHECK(restored.points.size() == middle.points.size());
        CHECK_NEAR(restored.points.back().x, middle.points.back().x, 1e-9);
    }
    e.perform(Action::Redo, t += 10);
    CHECK(els.size() == 4);
}

TEST(editor_toolbar_and_palm_rejection) {
    Editor e(900, 1200, Document{}, tempPath("toolbar.xopp"));
    const ToolPanel& tools = e.toolPanel();
    const IRect tab = center(tools.buttonRect(Action::Tools));

    e.touchDown(tab.x0, tab.y0, 10);
    const IRect eraser = center(tools.buttonRect(Action::Eraser));
    const IRect pen = center(tools.buttonRect(Action::Pen));
    e.touchDown(eraser.x0, eraser.y0, 15);
    CHECK(e.tool() == ToolKind::Eraser);
    e.penDown({double(pen.x0), double(pen.y0), 0.5, false}, 20);
    e.penUp(30);
    CHECK(e.tool() == ToolKind::Pen);
    CHECK(e.document().pages[0].layers.back().elements.empty());  // taps on the tools don't draw

    e.penDown({500, 500, 0.5, false}, 40);
    e.touchDown(eraser.x0, eraser.y0, 45);  // palm on the tools while writing
    const IRect undo = center(e.toolbar().buttonRect(Action::Undo));
    e.touchDown(undo.x0, undo.y0, 46);  // ...or on the toolbar
    e.penUp(50);
    CHECK(e.tool() == ToolKind::Pen);
    CHECK(e.document().pages[0].layers.back().elements.size() == 1);
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
        CHECK(!e.closeRequested() && e.modified());  // first the button shows it was pressed...
        e.onIdle(t);
        e.onIdle(t += 10);  // ...then it saves and closes
        CHECK(e.closeRequested());
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
    for (uint64_t now : {1000, 2000}) {
        e.perform(Action::Exit, now);
        e.onIdle(now);
        e.onIdle(now + 10);
        CHECK(e.closeRequested() == (now == 2000));
    }
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
    CHECK(e.document().pages[0].layers.back().elements.size() == 6);  // each stroke cut in two
    CHECK_NEAR(panelMismatches(panel, e.canvas()), 0, 0);

    for (int i = 0; i < 4; ++i) {  // undo the erase, then all three strokes
        e.perform(Action::Undo, t += 10);
        flush();
    }
    e.onIdle(t += 1000);
    flush();
    CHECK_NEAR(panelMismatches(panel, e.canvas()), 0, 0);
}

// ---- notebook list and app ----

namespace {

std::string tempDir(const std::string& name) {
    std::string pattern = tempPath(name + "-XXXXXX");
    return mkdtemp(pattern.data()) ? pattern : std::string();
}

void removeDir(const std::string& dir) {
    if (DIR* d = opendir(dir.c_str())) {
        while (const dirent* e = readdir(d)) {
            const std::string name = e->d_name;
            if (name == "." || name == "..") continue;
            const std::string path = dir + "/" + name;
            if (std::remove(path.c_str()) != 0) removeDir(path);
        }
        closedir(d);
    }
    rmdir(dir.c_str());
}

void setMtime(const std::string& path, time_t t) {
    const timeval tv[2] = {{t, 0}, {t, 0}};
    utimes(path.c_str(), tv);
}

Document oneStrokeDocument() {
    Document d;
    d.pages.push_back(makeBlankPage(216, 260, "lined"));
    Stroke s;
    s.points = {{20, 100}, {120, 100}};
    d.pages[0].layers[0].elements.push_back(s);
    return d;
}

void writeIn(App& app, uint64_t& t) {
    app.penDown({100, 500, 0.5, false}, t += 10);
    app.penMove({300, 500, 0.5, false}, t += 10);
    app.penUp(t += 10);
}

void tapButton(App& app, const Toolbar& tb, Action a, uint64_t& t) {
    const IRect c = center(tb.buttonRect(a));
    app.touchDown(c.x0, c.y0, t += 10);
}

// What a front end's main loop does between input events (and BACK and EXIT wait for).
void idle(App& app, uint64_t& t) {
    app.onIdle(t += 10);
    app.onIdle(t += 10);
}

void tapAt(App& app, const IRect& r, uint64_t& t) {
    const IRect c = center(r);
    app.touchDown(c.x0, c.y0, t += 10);
}

void tapTool(App& app, Action a, uint64_t& t) {
    const IRect r = app.editor()->toolPanel().buttonRect(a);
    CHECK(!r.empty());
    tapAt(app, r, t);
}

// Types lowercase letters, digits and spaces on the prompt's keyboard.
void typeIn(App& app, const std::string& text, uint64_t& t) {
    for (char ch : text) {
        const IRect key = ch == ' ' ? app.library()->prompt().keyRect("SPACE")
                                    : app.library()->prompt().keyRect(std::string(1, ch));
        CHECK(!key.empty());
        tapAt(app, key, t);
    }
}

}  // namespace

TEST(lists_notebooks_newest_first) {
    const std::string dir = tempDir("list");
    std::string err;
    const Document doc = oneStrokeDocument();
    for (const char* name : {"old.xopp", "new.xoj", "middle.XOPP", ".hidden.xopp"}) CHECK(saveXopp(dir + "/" + name, doc, err));
    CHECK(writeFile(dir + "/notes.txt", "not a notebook", false));
    CHECK(makeDirs(dir + "/folder.xopp"));
    setMtime(dir + "/old.xopp", 1000);
    setMtime(dir + "/middle.XOPP", 2000);
    setMtime(dir + "/new.xoj", 3000);

    const auto list = listNotebooks(dir);
    CHECK(list.size() == 3);
    if (list.size() == 3) {
        CHECK(list[0].name == "new" && list[1].name == "middle" && list[2].name == "old");
        CHECK(list[2].path == dir + "/old.xopp");
        CHECK(list[0].bytes > 0);
    }
    CHECK(isNotebookFile(dir + "/not-yet-created.xopp"));
    CHECK(!isNotebookFile(dir));

    const time_t when = 1800000000;
    const std::string first = newNotebookPath(dir, when);
    CHECK(first.size() > 5 && first.compare(first.size() - 5, 5, ".xopp") == 0);
    CHECK(saveXopp(first, doc, err));
    const std::string second = newNotebookPath(dir, when);
    CHECK(second != first && second.find(" (2).xopp") != std::string::npos);
    removeDir(dir);
}

TEST(app_opens_notebooks_from_the_list_and_returns) {
    const std::string dir = tempDir("app");
    std::string err;
    CHECK(saveXopp(dir + "/first.xopp", oneStrokeDocument(), err));
    AppOptions o;
    o.notebooksDir = dir;
    App app(900, 1200, o);
    CHECK(app.startupError().empty());
    CHECK(app.library() && !app.editor());
    CHECK(app.library()->notebooks().size() == 1);
    app.takeDamage();

    // Tapping a row highlights it first and opens it on the next idle pass, so the
    // highlight reaches the screen before a large notebook loads.
    uint64_t t = 1000;
    const IRect row = app.library()->rowRect(0);
    CHECK(!row.empty());
    app.touchDown(row.x0 + 100, row.y0 + 20, t);
    CHECK(hasMode(app.takeDamage(), Refresh::UI));
    CHECK(app.msUntilIdleWork(t) == 0);
    app.onIdle(t);
    CHECK(!app.editor());
    app.onIdle(t);
    CHECK(app.editor() != nullptr);
    if (!app.editor()) return removeDir(dir);
    CHECK(app.editor()->document().pages[0].layers.back().elements.size() == 1);
    CHECK(hasMode(app.takeDamage(), Refresh::Full));

    // Write, then BACK: saved, and back on the list.
    writeIn(app, t);
    tapButton(app, app.editor()->toolbar(), Action::Exit, t);
    idle(app, t);
    CHECK(!app.editor() && !app.quitRequested());
    Document saved;
    CHECK(loadXopp(dir + "/first.xopp", saved, err));
    CHECK(saved.pages.size() == 1 && saved.pages[0].layers.back().elements.size() == 2);

    // NEW asks for a name. OK without typing uses the date and time (unique even
    // twice in a minute); the notebook exists as soon as it's named.
    for (size_t expected : {2, 3}) {
        tapButton(app, app.library()->toolbar(), Action::NewNotebook, t);
        CHECK(app.library()->prompting());
        tapButton(app, app.library()->prompt().toolbar(), Action::Confirm, t);
        CHECK(app.editor() && app.editor()->path().compare(0, dir.size(), dir) == 0);
        app.perform(Action::Exit, t += 10);  // BACK without writing (keyboard shortcut path)
        idle(app, t);
        CHECK(app.library()->notebooks().size() == expected);
    }

    // A typed name, through the on-screen keyboard.
    tapButton(app, app.library()->toolbar(), Action::NewNotebook, t);
    typeIn(app, "physics 2", t);
    app.perform(Action::Confirm, t += 10);
    CHECK(app.editor() && app.editor()->path() == dir + "/Physics 2.xopp");
    writeIn(app, t);
    app.perform(Action::Exit, t += 10);
    idle(app, t);
    CHECK(app.library()->notebooks().size() == 4);

    tapButton(app, app.library()->toolbar(), Action::Exit, t);
    CHECK(app.quitRequested());
    removeDir(dir);
}

TEST(app_reports_unreadable_notebooks) {
    const std::string dir = tempDir("bad");
    CHECK(writeFile(dir + "/broken.xopp", "this is not a notebook", false));
    AppOptions o;
    o.notebooksDir = dir;
    App app(900, 1200, o);
    const IRect row = app.library()->rowRect(0);
    app.touchDown(row.x0 + 100, row.y0 + 20, 10);
    app.onIdle(20);
    app.onIdle(30);
    CHECK(!app.editor());
    CHECK(app.library()->message().find("Can't open") != std::string::npos);
    CHECK(hasMode(app.takeDamage(), Refresh::Full));  // the highlight is gone again
    removeDir(dir);
}

TEST(app_single_notebook_mode) {
    AppOptions o;
    o.file = tempPath("single.xopp");
    App app(900, 1200, o);
    CHECK(app.startupError().empty());
    CHECK(app.editor() && !app.library());
    uint64_t t = 0;
    writeIn(app, t);
    app.perform(Action::Exit, t += 10);
    idle(app, t);
    CHECK(app.quitRequested());
    CHECK(access(o.file.c_str(), F_OK) == 0);
    std::remove(o.file.c_str());

    AppOptions bad;
    bad.file = tempPath("unreadable.xopp");
    CHECK(writeFile(bad.file, "garbage", false));
    App broken(900, 1200, bad);
    CHECK(!broken.startupError().empty());
    std::remove(bad.file.c_str());
}

// ---- file manager ----

namespace {

size_t indexOf(Library& lib, const std::string& name) {
    for (size_t i = 0; i < lib.notebooks().size(); ++i)
        if (lib.notebooks()[i].name == name) return i;
    return static_cast<size_t>(-1);
}

bool hasNotebook(Library& lib, const std::string& name) { return indexOf(lib, name) != static_cast<size_t>(-1); }

}  // namespace

TEST(notebook_names) {
    CHECK(naturalLess("Lecture 2", "lecture 10"));
    CHECK(!naturalLess("lecture 10", "Lecture 2"));
    CHECK(naturalLess("file 007", "file 8"));
    CHECK(naturalLess("abc", "ABD"));
    CHECK(naturalLess("a", "ab"));
    CHECK(!naturalLess("same", "same"));

    std::string name, err;
    CHECK(cleanNotebookName("  ..Physics notes.  ", name, err) && name == "Physics notes");
    CHECK(!cleanNotebookName("   ", name, err));
    CHECK(!cleanNotebookName("a/b", name, err));
    CHECK(!cleanNotebookName("what?", name, err));
    CHECK(!cleanNotebookName(std::string(121, 'x'), name, err));
}

TEST(notebook_file_operations_keep_attachments) {
    const std::string dir = tempDir("ops");
    std::string err, path;
    const Document doc = oneStrokeDocument();
    CHECK(saveXopp(dir + "/Lecture.xopp", doc, err));
    CHECK(writeFile(dir + "/Lecture.xopp.bg.pdf", "%PDF attached background", false));
    CHECK(saveXopp(dir + "/Lecturer.xopp", doc, err));  // a different notebook with a similar name
    auto find = [&](const std::string& name) {
        for (const auto& nb : listNotebooks(dir))
            if (nb.name == name) return nb;
        return NotebookInfo{};
    };

    CHECK(renameNotebook(find("Lecture"), "Physics", path, err));
    CHECK(path == dir + "/Physics.xopp");
    CHECK(access((dir + "/Physics.xopp.bg.pdf").c_str(), F_OK) == 0);
    CHECK(access((dir + "/Lecture.xopp.bg.pdf").c_str(), F_OK) != 0);
    CHECK(access((dir + "/Lecturer.xopp").c_str(), F_OK) == 0);
    CHECK(!renameNotebook(find("Physics"), "Lecturer", path, err));  // taken
    CHECK(renameNotebook(find("Physics"), "physics", path, err));    // case only

    CHECK(duplicateNotebook(find("physics"), "Physics copy", path, err));
    CHECK(access((dir + "/Physics copy.xopp").c_str(), F_OK) == 0);
    CHECK(access((dir + "/Physics copy.xopp.bg.pdf").c_str(), F_OK) == 0);
    CHECK(access((dir + "/physics.xopp").c_str(), F_OK) == 0);
    CHECK(!duplicateNotebook(find("physics"), "Physics copy", path, err));

    CHECK(trashNotebook(find("Physics copy"), err));
    CHECK(access((dir + "/Physics copy.xopp").c_str(), F_OK) != 0);
    CHECK(access((dir + "/Trash/Physics copy.xopp").c_str(), F_OK) == 0);
    CHECK(access((dir + "/Trash/Physics copy.xopp.bg.pdf").c_str(), F_OK) == 0);
    CHECK(duplicateNotebook(find("physics"), "Physics copy", path, err));  // trash it again: no clash
    CHECK(trashNotebook(find("Physics copy"), err));
    CHECK(access((dir + "/Trash/Physics copy (2).xopp").c_str(), F_OK) == 0);
    CHECK(listNotebooks(dir).size() == 2);  // the Trash folder isn't listed
    removeDir(dir);
}

TEST(text_prompt_typing) {
    Canvas c(900, 1200);
    TextPrompt p(900, 1200);
    p.open(c, "NEW NOTEBOOK", "", "2026-11-02 14.30");
    CHECK(p.value() == "2026-11-02 14.30");
    auto tap = [&](const std::string& label) {
        const IRect r = center(p.keyRect(label));
        return p.tap(c, r.x0, r.y0);
    };
    tap("h");
    tap("i");
    CHECK(p.text() == "Hi");  // the first letter is capitalized
    tap("SHIFT");
    tap("x");
    tap("x");
    CHECK(p.text() == "HiXx");  // shift is for one letter
    tap("DEL");
    tap("SPACE");
    tap("2");
    CHECK(p.text() == "HiX 2");
    CHECK(p.value() == "HiX 2");
    tap("CLEAR");
    CHECK(p.text().empty() && p.value() == "2026-11-02 14.30");
    const IRect ok = center(p.toolbar().buttonRect(Action::Confirm));
    CHECK(p.tap(c, ok.x0, ok.y0) == TextPrompt::Result::Done);
    CHECK(p.perform(Action::Cancel) == TextPrompt::Result::Cancel);

    int gray = 0;  // it's refreshed with DU, so it must be pure black and white
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x) gray += c.at(x, y) != 0 && c.at(x, y) != 255;
    CHECK(gray == 0);
}

TEST(library_menu_rename_duplicate_delete) {
    const std::string dir = tempDir("menu");
    std::string err;
    CHECK(saveXopp(dir + "/Notes.xopp", oneStrokeDocument(), err));
    CHECK(saveXopp(dir + "/Lecture.xopp", oneStrokeDocument(), err));
    AppOptions o;
    o.notebooksDir = dir;
    App app(900, 1200, o);
    Library& lib = *app.library();
    EinkPanel panel(900, 1200);
    auto flush = [&] {
        for (const Damage& d : app.takeDamage()) panel.refresh(app.canvas(), d);
    };
    flush();
    uint64_t t = 0;

    // The "..." button opens the menu; tapping outside it closes it again.
    tapAt(app, lib.menuRect(indexOf(lib, "Lecture")), t);
    flush();
    CHECK(!lib.dialogButton(Action::Rename).empty());
    CHECK(!app.editor());
    app.touchDown(5, 1195, t += 10);
    flush();
    CHECK(lib.dialogButton(Action::Rename).empty());

    // Rename through the keyboard.
    tapAt(app, lib.menuRect(indexOf(lib, "Lecture")), t);
    tapAt(app, lib.dialogButton(Action::Rename), t);
    flush();
    CHECK(lib.prompting() && lib.prompt().text() == "Lecture");
    tapAt(app, lib.prompt().keyRect("CLEAR"), t);
    typeIn(app, "physics", t);
    tapButton(app, lib.prompt().toolbar(), Action::Confirm, t);
    flush();
    CHECK(!lib.prompting());
    CHECK(hasNotebook(lib, "Physics") && !hasNotebook(lib, "Lecture"));

    // A name that's taken keeps the prompt open.
    tapAt(app, lib.menuRect(indexOf(lib, "Physics")), t);
    tapAt(app, lib.dialogButton(Action::Rename), t);
    tapAt(app, lib.prompt().keyRect("CLEAR"), t);
    typeIn(app, "notes", t);
    tapButton(app, lib.prompt().toolbar(), Action::Confirm, t);
    CHECK(lib.prompting());
    tapButton(app, lib.prompt().toolbar(), Action::Cancel, t);
    flush();
    CHECK(!lib.prompting() && hasNotebook(lib, "Physics"));

    // Duplicate suggests "<name> copy".
    tapAt(app, lib.menuRect(indexOf(lib, "Physics")), t);
    tapAt(app, lib.dialogButton(Action::Duplicate), t);
    CHECK(lib.prompt().text() == "Physics copy");
    tapButton(app, lib.prompt().toolbar(), Action::Confirm, t);
    flush();
    CHECK(lib.notebooks().size() == 3 && hasNotebook(lib, "Physics copy"));

    // Delete asks first, then moves it to the Trash folder.
    tapAt(app, lib.menuRect(indexOf(lib, "Notes")), t);
    tapAt(app, lib.dialogButton(Action::Delete), t);
    flush();
    CHECK(!lib.dialogButton(Action::Confirm).empty());
    CHECK(hasNotebook(lib, "Notes"));
    tapAt(app, lib.dialogButton(Action::Confirm), t);
    flush();
    CHECK(!hasNotebook(lib, "Notes"));
    CHECK(access((dir + "/Trash/Notes.xopp").c_str(), F_OK) == 0);
    CHECK(lib.message().find("Trash") != std::string::npos);

    // All of it is black and white, so the fast refreshes left nothing stale.
    CHECK_NEAR(panelMismatches(panel, app.canvas()), 0, 0);
    removeDir(dir);
}

TEST(library_sort_order_is_remembered) {
    const std::string dir = tempDir("sort");
    std::string err;
    for (const char* n : {"Lecture 10", "Lecture 2", "algebra"}) CHECK(saveXopp(dir + "/" + n + ".xopp", oneStrokeDocument(), err));
    setMtime(dir + "/Lecture 10.xopp", 3000);
    setMtime(dir + "/Lecture 2.xopp", 2000);
    setMtime(dir + "/algebra.xopp", 1000);
    AppOptions o;
    o.notebooksDir = dir;
    {
        App app(900, 1200, o);
        CHECK(app.library()->notebooks()[0].name == "Lecture 10");
        uint64_t t = 0;
        tapButton(app, app.library()->toolbar(), Action::Sort, t);
        const auto& nbs = app.library()->notebooks();
        CHECK(nbs[0].name == "algebra" && nbs[1].name == "Lecture 2" && nbs[2].name == "Lecture 10");
    }
    App again(900, 1200, o);
    CHECK(again.library()->order() == NotebookOrder::Name);
    CHECK(again.library()->notebooks()[0].name == "algebra");
    removeDir(dir);
}

// ---- follow-ups from the device: eraser modes, EXIT from a notebook, menu feedback ----

TEST(editor_toolbar_with_back_and_exit_fits) {
    EditorOptions o;
    o.fromList = true;
    for (int width : {1072, 1236, 1860, 1986}) {  // Kindle screen widths
        const Editor e(width, 2400, Document{}, tempPath("toolbar-list.xopp"), o);
        int lastRight = 0;
        for (Action a : {Action::Undo, Action::Redo, Action::PageStyle, Action::PrevPage, Action::NextPage, Action::Save,
                         Action::Exit, Action::Quit}) {
            const IRect r = e.toolbar().buttonRect(a);
            CHECK(!r.empty() && r.x0 >= lastRight && r.x1 <= width);
            lastRight = r.x1;
        }
    }
}

TEST(eraser_can_remove_whole_strokes) {
    Editor e(900, 1200, Document{}, tempPath("whole.xopp"));
    uint64_t t = 0;
    drawLine(e, 100, 600, 400, t);
    drawLine(e, 100, 600, 500, t);
    const auto& els = e.document().pages[0].layers.back().elements;

    // WHOLE, among the eraser's options, switches to whole strokes.
    e.perform(Action::Eraser, t += 10);
    CHECK(!e.eraseWholeStrokes());
    e.perform(Action::EraseWhole, t += 10);
    CHECK(e.eraseWholeStrokes() && e.tool() == ToolKind::Eraser);

    e.penDown({300, 480, 0.5, false}, t += 10);  // touches the middle of the second stroke only
    e.penMove({310, 520, 0.5, false}, t += 10);
    e.penUp(t += 10);
    CHECK(els.size() == 1);
    CHECK(e.canvas().at(550, 500) > 200);  // all of it is gone, not just the touched part
    e.perform(Action::Undo, t += 10);
    CHECK(els.size() == 2);

    // The pen's eraser end follows the same setting, even with the pen tool selected.
    e.perform(Action::Pen, t += 10);
    e.penDown({300, 380, 0.5, true}, t += 10);
    e.penMove({310, 420, 0.5, true}, t += 10);
    e.penUp(t += 10);
    CHECK(els.size() == 1);

    // And back to cutting.
    e.perform(Action::ErasePartial, t += 10);
    CHECK(!e.eraseWholeStrokes() && e.tool() == ToolKind::Eraser);
}

TEST(app_exit_from_a_notebook_and_remembered_eraser) {
    const std::string dir = tempDir("quit");
    std::string err;
    CHECK(saveXopp(dir + "/One.xopp", oneStrokeDocument(), err));
    CHECK(saveXopp(dir + "/Two.xopp", oneStrokeDocument(), err));
    AppOptions o;
    o.notebooksDir = dir;
    uint64_t t = 0;
    auto open = [&](App& app, const std::string& name) {
        tapAt(app, app.library()->rowRect(indexOf(*app.library(), name)), t);
        app.onIdle(t);
        app.onIdle(t);
        CHECK(app.editor() != nullptr);
    };
    {
        App app(900, 1200, o);
        open(app, "One");
        if (!app.editor()) return removeDir(dir);
        CHECK(!app.editor()->toolbar().buttonRect(Action::Quit).empty());
        // Switch the eraser to whole strokes and the pen to thick; BACK; another notebook starts that way.
        for (Action a : {Action::Tools, Action::Eraser, Action::EraseWhole, Action::Tools, Action::Pen, Action::SizeThick})
            tapTool(app, a, t);
        CHECK(app.editor()->eraseWholeStrokes() && app.editor()->toolSize(ToolKind::Pen) == ToolSize::Thick);
        tapButton(app, app.editor()->toolbar(), Action::Exit, t);
        idle(app, t);
        open(app, "Two");
        if (!app.editor()) return removeDir(dir);
        CHECK(app.editor()->eraseWholeStrokes() && app.editor()->toolSize(ToolKind::Pen) == ToolSize::Thick);
        CHECK(app.editor()->toolSize(ToolKind::Eraser) == ToolSize::Medium);
        // EXIT saves and leaves the app straight from the notebook.
        for (Action a : {Action::Tools, Action::Pen, Action::Pen}) tapTool(app, a, t);  // again: puts the panel away
        writeIn(app, t);
        tapButton(app, app.editor()->toolbar(), Action::Quit, t);
        idle(app, t);
        CHECK(app.quitRequested());
        Document saved;
        CHECK(loadXopp(dir + "/Two.xopp", saved, err) && saved.pages[0].layers.back().elements.size() == 2);
        if (saved.pages[0].layers.back().elements.size() == 2)
            CHECK_NEAR(std::get<Stroke>(saved.pages[0].layers.back().elements[1]).width, 2.26, 1e-6);
    }
    App again(900, 1200, o);  // and after a restart
    open(again, "One");
    if (again.editor())
        CHECK(again.editor()->eraseWholeStrokes() && again.editor()->toolSize(ToolKind::Pen) == ToolSize::Thick);
    removeDir(dir);
}

TEST(library_menu_button_shows_it_was_pressed) {
    const std::string dir = tempDir("pressed");
    std::string err;
    for (int i = 0; i < 13; ++i) CHECK(saveXopp(dir + "/N" + std::to_string(i) + ".xopp", oneStrokeDocument(), err));
    AppOptions o;
    o.notebooksDir = dir;
    App app(1860, 2480, o);  // the Scribe's screen: a full page of 13 rows
    Library& lib = *app.library();
    uint64_t t = 0;
    CHECK(!lib.rowRect(12).empty());
    for (size_t row : {size_t(0), size_t(12)}) {  // the menu goes below the first row, above the last
        const IRect m = lib.menuRect(row);
        CHECK(app.canvas().at(m.x0 + 10, m.y0 + 10) == 255);
        tapAt(app, m, t);
        CHECK(app.canvas().at(m.x0 + 10, m.y0 + 10) == 0);  // inverted while the menu is open
        CHECK(!lib.dialogRect().empty());
        CHECK(lib.dialogRect().intersected(lib.rowRect(row)).empty());  // doesn't cover the tapped row
        tapAt(app, lib.dialogButton(Action::Delete), t);
        CHECK(app.canvas().at(m.x0 + 10, m.y0 + 10) == 0);  // still, while confirming
        tapAt(app, lib.dialogButton(Action::Cancel), t);
        CHECK(app.canvas().at(m.x0 + 10, m.y0 + 10) == 255);
    }
    removeDir(dir);
}

// ---- shape recognition ----

namespace {

// Deterministic "hand" noise in [-1, 1).
struct Wobble {
    uint32_t state;
    double next() {
        state = state * 1664525u + 1013904223u;
        return static_cast<double>(state >> 8) / (1 << 23) - 1;
    }
};

// A path traced point by point every `step`, each point off by up to `jitter`.
std::vector<StrokePoint> handDrawn(const std::vector<StrokePoint>& path, double step, double jitter, uint32_t seed) {
    Wobble w{seed};
    std::vector<StrokePoint> out;
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        const StrokePoint a = path[i], b = path[i + 1];
        const int n = std::max(1, static_cast<int>(std::hypot(b.x - a.x, b.y - a.y) / step));
        for (int k = 0; k < n; ++k) {
            const double t = static_cast<double>(k) / n;
            out.push_back({a.x + (b.x - a.x) * t + w.next() * jitter, a.y + (b.y - a.y) * t + w.next() * jitter});
        }
    }
    out.push_back(path.back());
    return out;
}

std::vector<StrokePoint> ellipsePath(double cx, double cy, double a, double b, double rot, double from, double sweep,
                                     int n) {
    std::vector<StrokePoint> out;
    for (int k = 0; k <= n; ++k) {
        const double t = from + sweep * k / n;
        const double x = a * std::cos(t), y = b * std::sin(t);
        out.push_back({cx + x * std::cos(rot) - y * std::sin(rot), cy + x * std::sin(rot) + y * std::cos(rot)});
    }
    return out;
}

bool near(StrokePoint a, StrokePoint b, double tol) { return std::hypot(a.x - b.x, a.y - b.y) <= tol; }

bool hasCorner(const Shape& s, StrokePoint c, double tol) {
    for (const StrokePoint& p : s.points)
        if (near(p, c, tol)) return true;
    return false;
}

double cosAt(StrokePoint a, StrokePoint b, StrokePoint c) {
    const double ux = a.x - b.x, uy = a.y - b.y, vx = c.x - b.x, vy = c.y - b.y;
    return (ux * vx + uy * vy) / (std::hypot(ux, uy) * std::hypot(vx, vy));
}

}  // namespace

TEST(recognizes_lines) {
    Shape s = recognizeStroke(handDrawn({{10, 20}, {210, 80}}, 2, 1.0, 1));
    CHECK(s.kind == ShapeKind::Line);
    if (s.kind == ShapeKind::Line) CHECK(near(s.points[0], {10, 20}, 3) && near(s.points[1], {210, 80}, 3));

    s = recognizeStroke(handDrawn({{30, 20}, {140, 24}}, 2, 1.2, 15));  // very jittery
    CHECK(s.kind == ShapeKind::Line);
    s = recognizeStroke(handDrawn({{10, 50}, {200, 58}}, 2, 0.8, 2));  // 2.4 degrees off horizontal
    CHECK(s.kind == ShapeKind::Line && s.points[0].y == s.points[1].y);
    s = recognizeStroke(handDrawn({{40, 10}, {44, 150}}, 2, 0.8, 3));  // 1.6 degrees off vertical
    CHECK(s.kind == ShapeKind::Line && s.points[0].x == s.points[1].x);

    // Not lines: an arc, a V, a squiggle, a tiny mark.
    CHECK(recognizeStroke(ellipsePath(100, 100, 60, 60, 0, 0, 3.14, 40)).kind == ShapeKind::None);
    CHECK(recognizeStroke(handDrawn({{0, 0}, {50, 100}, {100, 0}}, 2, 0.5, 4)).kind == ShapeKind::None);
    std::vector<StrokePoint> squiggle;
    for (int x = 0; x <= 200; x += 2) squiggle.push_back({double(x), 50 + 15 * std::sin(x * 0.08)});
    CHECK(recognizeStroke(squiggle).kind == ShapeKind::None);
    CHECK(recognizeStroke(handDrawn({{0, 0}, {4, 3}}, 0.5, 0.2, 5)).kind == ShapeKind::None);
}

TEST(recognizes_closed_shapes) {
    // A rectangle started mid-side, overshooting the start a little at the end.
    Shape s = recognizeStroke(handDrawn({{60, 20}, {150, 20}, {150, 90}, {20, 90}, {20, 20}, {75, 22}}, 2, 1.0, 6));
    CHECK(s.kind == ShapeKind::Rectangle);
    if (s.kind == ShapeKind::Rectangle) {
        CHECK(s.points.size() == 5);
        for (StrokePoint c : {StrokePoint{20, 20}, StrokePoint{150, 20}, StrokePoint{150, 90}, StrokePoint{20, 90}})
            CHECK(hasCorner(s, c, 4));
        CHECK(s.points[0].y == s.points[1].y || s.points[0].x == s.points[1].x);  // exactly axis-aligned
    }

    // Overshooting the starting corner by a lot is still closed.
    s = recognizeStroke(handDrawn({{200, 30}, {290, 28}, {292, 100}, {198, 98}, {200, 30}, {230, 31}}, 2, 1.5, 16));
    CHECK(s.kind == ShapeKind::Rectangle);

    // Rotated 30 degrees: still a rectangle, with exact right angles.
    std::vector<StrokePoint> rotated;
    const double c30 = std::cos(0.5236), s30 = std::sin(0.5236);
    for (StrokePoint p : {StrokePoint{0, 0}, StrokePoint{120, 0}, StrokePoint{120, 60}, StrokePoint{0, 60},
                          StrokePoint{0, 0}})
        rotated.push_back({100 + p.x * c30 - p.y * s30, 100 + p.x * s30 + p.y * c30});
    s = recognizeStroke(handDrawn(rotated, 2, 0.8, 7));
    CHECK(s.kind == ShapeKind::Rectangle);
    if (s.points.size() == 5) {
        for (int k = 0; k < 4; ++k) CHECK(std::fabs(cosAt(s.points[(k + 3) % 4], s.points[k], s.points[k + 1])) < 1e-9);
    }

    s = recognizeStroke(handDrawn({{50, 10}, {100, 100}, {0, 100}, {50, 10}}, 2, 0.8, 8));
    CHECK(s.kind == ShapeKind::Triangle && s.points.size() == 4);
    CHECK(hasCorner(s, {50, 10}, 5) && hasCorner(s, {100, 100}, 5) && hasCorner(s, {0, 100}, 5));

    s = recognizeStroke(handDrawn({{30, 10}, {90, 10}, {120, 80}, {0, 80}, {30, 10}}, 2, 0.8, 9));  // trapezoid
    CHECK(s.kind == ShapeKind::Quadrilateral);

    // A circle, wobbly, overshooting its start.
    std::vector<StrokePoint> circle = ellipsePath(100, 100, 50, 50, 0, 0.3, 2 * 3.14159 + 0.4, 120);
    Wobble w{10};
    for (StrokePoint& p : circle) p = {p.x + w.next() * 1.2, p.y + w.next() * 1.2};
    s = recognizeStroke(circle);
    CHECK(s.kind == ShapeKind::Circle);
    for (const StrokePoint& p : s.points) CHECK(std::fabs(std::hypot(p.x - 100, p.y - 100) - 50) < 3);

    CHECK(recognizeStroke(ellipsePath(100, 100, 80, 35, 0, 0, 2 * 3.14159, 120)).kind == ShapeKind::Ellipse);
    // Going around twice is a scribble, not a circle.
    CHECK(recognizeStroke(ellipsePath(100, 100, 50, 50, 0, 0, 4 * 3.14159, 240)).kind == ShapeKind::None);
}

TEST(polygons_from_separate_lines_and_arrows) {
    std::vector<StrokePoint> chain;
    Shape closed;
    CHECK(extendChain(chain, {0, 0}, {100, 0}, closed) && closed.kind == ShapeKind::None);
    CHECK(extendChain(chain, {101, 1}, {100, 60}, closed) && closed.kind == ShapeKind::None);
    CHECK(extendChain(chain, {1, 60}, {99, 61}, closed) && closed.kind == ShapeKind::None);  // drawn the other way
    CHECK(extendChain(chain, {0, 59}, {1, 2}, closed) && closed.kind == ShapeKind::Rectangle);
    chain = {{0, 0}, {100, 0}};
    CHECK(!extendChain(chain, {300, 300}, {400, 300}, closed));  // doesn't touch

    Shape arrow;
    CHECK(makeArrow({0, 0}, {100, 0}, handDrawn({{88, -9}, {100, 0}, {88, 9}}, 1, 0.3, 11), arrow));
    CHECK(arrow.kind == ShapeKind::Arrow && arrow.points.size() == 5);
    if (arrow.points.size() == 5) {
        CHECK(near(arrow.points[0], {0, 0}, 1e-9) && near(arrow.points[1], {100, 0}, 1e-9));
        CHECK(std::fabs(arrow.points[2].y + arrow.points[4].y) < 1e-9);  // symmetric head
    }
    CHECK(makeArrow({0, 0}, {100, 0}, handDrawn({{12, -9}, {0, 0}, {12, 9}}, 1, 0.3, 12), arrow));  // other end
    CHECK(near(arrow.points[1], {0, 0}, 1e-9));
    CHECK(!makeArrow({0, 0}, {100, 0}, handDrawn({{38, -9}, {50, 0}, {38, 9}}, 1, 0.3, 13), arrow));    // mid-line
    CHECK(!makeArrow({0, 0}, {100, 0}, handDrawn({{112, -9}, {100, 0}, {112, 9}}, 1, 0.3, 14), arrow));  // backwards
}

TEST(editor_shape_mode) {
    auto draw = [](Editor& e, const std::vector<StrokePoint>& pts, uint64_t& t) {
        e.penDown({pts[0].x, pts[0].y, 0.5, false}, t += 10);
        for (size_t i = 1; i < pts.size(); ++i) e.penMove({pts[i].x, pts[i].y, 0.5, false}, t += 3);
        e.penUp(t += 3);
    };
    uint64_t t = 0;
    {
        Editor e(900, 1200, Document{}, tempPath("shapes.xopp"));
        const auto& els = e.document().pages[0].layers.back().elements;
        e.perform(Action::Shapes, t += 10);
        CHECK(e.recognizeShapes());

        draw(e, ellipsePath(450, 600, 150, 150, 0, 0.2, 2 * 3.14159 + 0.3, 150), t);
        CHECK(els.size() == 1);
        const size_t clean = std::get<Stroke>(els.at(0)).points.size();
        e.perform(Action::Undo, t += 10);  // first undo: back to what was drawn
        CHECK(els.size() == 1 && std::get<Stroke>(els[0]).points.size() != clean);
        e.perform(Action::Undo, t += 10);  // second: gone
        CHECK(els.empty());
        e.perform(Action::Redo, t += 10);
        e.perform(Action::Redo, t += 10);
        CHECK(els.size() == 1 && std::get<Stroke>(els[0]).points.size() == clean);

        // Writing still works in shape mode: an unrecognizable stroke stays as drawn.
        std::vector<StrokePoint> squiggle;
        for (int x = 100; x <= 500; x += 4) squiggle.push_back({double(x), 1000 + 40 * std::sin(x * 0.05)});
        draw(e, squiggle, t);
        CHECK(els.size() == 2 && std::get<Stroke>(els[1]).points.size() > 50);
    }
    {
        // Four separate sides become one rectangle; a line plus a V becomes an arrow.
        EditorOptions o;
        o.recognizeShapes = true;
        Editor e(900, 1200, Document{}, tempPath("polygon.xopp"), o);
        const auto& els = e.document().pages[0].layers.back().elements;
        draw(e, handDrawn({{200, 300}, {600, 300}}, 4, 1, 21), t);
        draw(e, handDrawn({{602, 302}, {600, 600}}, 4, 1, 22), t);
        draw(e, handDrawn({{598, 601}, {201, 600}}, 4, 1, 23), t);
        CHECK(els.size() == 3);
        draw(e, handDrawn({{199, 598}, {201, 303}}, 4, 1, 24), t);
        CHECK(els.size() == 1 && std::get<Stroke>(els[0]).points.size() == 5);
        e.perform(Action::Undo, t += 10);
        CHECK(els.size() == 4);  // the four sides again

        draw(e, handDrawn({{200, 900}, {600, 900}}, 4, 1, 25), t);
        draw(e, handDrawn({{560, 870}, {600, 900}, {560, 930}}, 3, 0.8, 26), t);
        CHECK(els.size() == 5 && std::get<Stroke>(els.back()).points.size() == 5);
    }
    {
        // Shape mode off: nothing changes.
        Editor e(900, 1200, Document{}, tempPath("noshapes.xopp"));
        draw(e, ellipsePath(450, 600, 150, 150, 0, 0, 2 * 3.14159, 150), t);
        CHECK(std::get<Stroke>(e.document().pages[0].layers.back().elements.at(0)).points.size() > 100);
    }
}

// ---- lasso ----

TEST(lasso_geometry) {
    const std::vector<StrokePoint> square = {{0, 0}, {100, 0}, {100, 100}, {0, 100}};
    CHECK(pointInPolygon({50, 50}, square));
    CHECK(!pointInPolygon({150, 50}, square));

    std::vector<Element> els;
    Stroke inside, mostly, half;
    inside.points = {{10, 10}, {50, 50}, {90, 20}};
    for (int x = 10; x <= 120; x += 10) mostly.points.push_back({double(x), 50});  // 9 of 12 points inside
    for (int x = 60; x <= 140; x += 10) half.points.push_back({double(x), 70});    // 4 of 9 inside
    els.push_back(inside);
    els.push_back(ForeignElement{});  // text and images aren't selectable (yet)
    els.push_back(mostly);
    els.push_back(half);
    CHECK(strokesInLasso(els, square) == (std::vector<size_t>{0, 2}));

    Stroke s;
    s.width = 2;
    s.points = {{10, 10}, {20, 30}};
    s.segmentWidths = {1.5};
    const Stroke big = scaledStroke(s, {10, 10}, 2);
    CHECK(near(big.points[1], {30, 50}, 1e-9) && big.width == 4 && big.segmentWidths[0] == 3);
    CHECK(near(movedStroke(s, 5, -5).points[0], {15, 5}, 1e-9));
}

TEST(editor_lasso_select_move_resize_delete_duplicate) {
    EditorOptions o;
    o.render.antialias = false;
    Editor e(1860, 2480, Document{}, tempPath("lasso.xopp"), o);
    EinkPanel panel(1860, 2480);
    auto flush = [&] {
        for (const Damage& d : e.takeDamage()) panel.refresh(e.canvas(), d);
    };
    uint64_t t = 1000;
    auto path = [&](const std::vector<StrokePoint>& pts, bool eraser = false) {
        e.penDown({pts[0].x, pts[0].y, 0.5, eraser}, t += 10);
        flush();
        for (size_t i = 1; i < pts.size(); ++i) {
            e.penMove({pts[i].x, pts[i].y, 0.5, eraser}, t += 20);
            e.onIdle(t);
            flush();
        }
        e.penUp(t += 10);
        flush();
    };
    auto tap = [&](const IRect& r) {
        const IRect c = center(r);
        e.touchDown(c.x0, c.y0, t += 10);
        flush();
    };
    const auto& els = e.document().pages[0].layers.back().elements;
    path(handDrawn({{300, 600}, {500, 620}}, 6, 1, 31));
    path(handDrawn({{600, 600}, {800, 640}}, 6, 1, 32));
    path(handDrawn({{300, 1500}, {900, 1500}}, 6, 1, 33));
    e.onIdle(t += 1000);
    flush();
    const Stroke first = std::get<Stroke>(els[0]);

    // A loop around the first two strokes selects them.
    e.perform(Action::Lasso, t += 10);
    path(handDrawn({{250, 520}, {850, 520}, {850, 720}, {250, 720}, {250, 530}}, 8, 1, 34));
    CHECK(e.selection() == (std::vector<size_t>{0, 1}));
    // Its menu: EDIT and MATH, whose options show once one is opened.
    CHECK(!e.selectionFrame().empty() && !e.selectionButton(Action::EditMenu).empty());
    CHECK(!e.selectionButton(Action::MathMenu).empty() && e.selectionButton(Action::Delete).empty());

    // Drag inside the frame: moves them.
    const IRect frame = center(e.selectionFrame());
    path({{double(frame.x0), double(frame.y0)}, {frame.x0 + 100.0, frame.y0 + 150.0}, {frame.x0 + 200.0, frame.y0 + 300.0}});
    const double scale = 1860 / e.document().pages[0].width;
    CHECK_NEAR(std::get<Stroke>(els[0]).points[0].x, first.points[0].x + 200 / scale, 1e-6);
    CHECK_NEAR(std::get<Stroke>(els[0]).points[0].y, first.points[0].y + 300 / scale, 1e-6);
    CHECK(e.selection().size() == 2);  // still selected, ready for more
    e.perform(Action::Undo, t += 10);
    flush();
    CHECK(e.selection().empty());
    CHECK_NEAR(std::get<Stroke>(els[0]).points[0].x, first.points[0].x, 1e-9);
    e.perform(Action::Redo, t += 10);
    flush();

    // Select again and resize with the handle: twice the size, lines twice as wide.
    path(handDrawn({{450, 820}, {1050, 820}, {1050, 1020}, {450, 1020}, {450, 830}}, 8, 1, 35));
    CHECK(e.selection().size() == 2);
    const RectF before = std::get<Stroke>(els[0]).bounds();
    const double widthBefore = std::get<Stroke>(els[0]).width;
    const IRect f = e.selectionFrame(), h = center(e.selectionHandle());
    path({{double(h.x0), double(h.y0)}, {h.x0 + f.width() * 0.5, h.y0 + f.height() * 0.5},
          {double(h.x0 + f.width()), double(h.y0 + f.height())}});
    CHECK(std::get<Stroke>(els[0]).width > 1.7 * widthBefore);
    CHECK(std::get<Stroke>(els[0]).bounds().x1 - std::get<Stroke>(els[0]).bounds().x0 > 1.7 * (before.x1 - before.x0));

    // EDIT: duplicate (with a finger), then delete the copies, then undo that.
    tap(e.selectionButton(Action::EditMenu));
    CHECK(!e.selectionButton(Action::Delete).empty() && e.selectionButton(Action::MathSum).empty());
    tap(e.selectionButton(Action::Duplicate));
    CHECK(els.size() == 5 && e.selection() == (std::vector<size_t>{3, 4}));
    tap(e.selectionButton(Action::Delete));
    CHECK(els.size() == 3 && e.selection().empty());
    e.perform(Action::Undo, t += 10);
    flush();
    CHECK(els.size() == 5);

    // Tapping EDIT again closes its options; a new selection starts closed too.
    path(handDrawn({{250, 1400}, {950, 1400}, {950, 1600}, {250, 1600}, {250, 1410}}, 8, 1, 36));
    CHECK(e.selection() == (std::vector<size_t>{2}) && e.selectionButton(Action::Delete).empty());
    tap(e.selectionButton(Action::EditMenu));
    tap(e.selectionButton(Action::MathMenu));  // straight from one to the other
    CHECK(e.selectionButton(Action::Delete).empty() && !e.selectionButton(Action::MathSum).empty());
    tap(e.selectionButton(Action::MathMenu));
    CHECK(e.selectionButton(Action::MathSum).empty());

    // A tap elsewhere lets go of the selection.
    e.penDown({1500, 2200, 0.5, false}, t += 10);
    e.penUp(t += 10);
    flush();
    CHECK(e.selection().empty());

    // Through all of it, the fast refreshes left nothing stale on the panel.
    e.onIdle(t += 1000);
    flush();
    CHECK_NEAR(panelMismatches(panel, e.canvas()), 0, 0);
}

// ---- the tools panel along the left edge ----

TEST(tool_panel_opens_picks_and_puts_itself_away) {
    EditorOptions o;
    o.render.antialias = false;
    Editor e(1860, 2480, Document{}, tempPath("tool-panel.xopp"), o);
    EinkPanel panel(1860, 2480);
    std::vector<Damage> last;
    auto flush = [&] {
        last = e.takeDamage();
        for (const Damage& d : last) panel.refresh(e.canvas(), d);
    };
    flush();
    uint64_t t = 1000;
    const ToolPanel& tools = e.toolPanel();
    auto tap = [&](Action a) {
        const IRect r = tools.buttonRect(a);
        CHECK(!r.empty());
        const IRect c = center(r);
        e.touchDown(c.x0, c.y0, t += 10);
        flush();
    };
    auto path = [&](const std::vector<StrokePoint>& pts) {
        e.penDown({pts[0].x, pts[0].y, 0.5, false}, t += 10);
        flush();
        for (size_t i = 1; i < pts.size(); ++i) {
            for (int k = 1; k <= 10; ++k) {
                const double f = k / 10.0;
                e.penMove({pts[i - 1].x + (pts[i].x - pts[i - 1].x) * f, pts[i - 1].y + (pts[i].y - pts[i - 1].y) * f,
                           0.5, false},
                          t += 5);
                flush();
            }
        }
        e.penUp(t += 5);
        flush();
    };
    const auto& els = e.document().pages[0].layers.back().elements;

    // Hidden, it's just a tab at the left edge, under the toolbar.
    const IRect tab = tools.buttonRect(Action::Tools);
    CHECK(!tools.open() && tools.rects().size() == 1);
    CHECK(tab.x0 == 0 && tab.y0 >= e.toolbar().rect().y1 && tab.width() < 250);

    // The tab opens the column, quickly: black and white with DU. PEN, SHAPE, LASSO, MARK, ERASE.
    tap(Action::Tools);
    CHECK(tools.open() && tools.buttonRect(Action::Tools).empty());
    CHECK(!last.empty());
    for (const Damage& d : last) CHECK(d.mode == Refresh::UI);
    int lastY = 0;
    for (Action a : {Action::Pen, Action::Shapes, Action::Lasso, Action::Highlighter, Action::Eraser}) {
        const IRect r = tools.buttonRect(a);
        CHECK(!r.empty() && r.y0 >= lastY && r.x0 < 30);
        lastY = r.y1;
    }

    // LASSO is picked, and the panel goes away.
    tap(Action::Lasso);
    CHECK(e.tool() == ToolKind::Lasso && !tools.open());

    // PEN brings up its widths beside the column; THICK changes the pen's alone.
    tap(Action::Tools);
    tap(Action::Pen);
    CHECK(e.tool() == ToolKind::Pen && tools.options() == Action::Pen);
    CHECK(tools.buttonRect(Action::SizeThick).x0 > tools.buttonRect(Action::Pen).x1);
    CHECK(tools.buttonRect(Action::EraseWhole).empty());
    tap(Action::SizeThick);
    CHECK(!tools.open() && e.toolSize(ToolKind::Pen) == ToolSize::Thick);
    CHECK(e.toolSize(ToolKind::Highlighter) == ToolSize::Medium && e.toolSize(ToolKind::Eraser) == ToolSize::Medium);
    path({{600, 900}, {1200, 900}});
    CHECK(els.size() == 1);
    if (els.size() == 1) CHECK_NEAR(std::get<Stroke>(els[0]).width, 2.26, 1e-9);

    // SHAPE switches recognition on, and goes back to the pen.
    tap(Action::Tools);
    tap(Action::Highlighter);
    tap(Action::Shapes);
    CHECK(e.recognizeShapes() && e.tool() == ToolKind::Pen && !tools.open());

    // From the pen's options straight to the eraser's, which also offer whole strokes.
    tap(Action::Tools);
    tap(Action::Pen);
    tap(Action::Eraser);
    CHECK(tools.options() == Action::Eraser && !tools.buttonRect(Action::EraseWhole).empty());
    // The pen's options uncover the margin line; DU over it must not get there first (see takeDamage).
    CHECK_NEAR(panelMismatches(panel, e.canvas()), 0, 0);
    tap(Action::EraseWhole);
    CHECK(e.eraseWholeStrokes() && e.tool() == ToolKind::Eraser && !tools.open());

    // Tapping the tool again puts the panel away, and so does a finger on the page.
    tap(Action::Tools);
    tap(Action::Pen);
    tap(Action::Pen);
    CHECK(!tools.open() && e.tool() == ToolKind::Pen);
    tap(Action::Tools);
    e.touchDown(1000, 1500, t += 10);
    flush();
    CHECK(!tools.open() && els.size() == 1);

    // Writing beside the open panel puts it away when the pen lifts, and the stroke counts.
    tap(Action::Tools);
    path({{600, 1200}, {1200, 1200}});
    CHECK(!tools.open() && els.size() == 2);

    // Ink passing under the tab doesn't cover it, before or after the cleanup refresh.
    const int mid = (tab.y0 + tab.y1) / 2;
    path({{600, double(mid)}, {20, double(mid)}});
    CHECK(els.size() == 3);
    CHECK(e.canvas().at(tab.x0 + 8, mid) == 255);
    e.onIdle(t += 1000);
    flush();
    CHECK(e.canvas().at(tab.x0 + 8, mid) == 255);

    // A selection up by the tab keeps its buttons clear of it.
    path({{300, 320}, {600, 320}});
    tap(Action::Tools);
    tap(Action::Lasso);
    path({{260, 280}, {640, 280}, {640, 360}, {260, 360}, {262, 285}});
    CHECK(e.selection().size() == 1);
    const IRect menu = e.selectionButton(Action::EditMenu);
    CHECK(!menu.empty() && (menu.x0 >= tab.x1 || menu.y0 >= tab.y1 || menu.y1 <= tab.y0));

    // Through all of it, the fast refreshes left nothing stale on the panel.
    e.onIdle(t += 1000);
    flush();
    CHECK_NEAR(panelMismatches(panel, e.canvas()), 0, 0);
}

// ---- paper: per-page backgrounds ----

TEST(paper_settings_and_presets) {
    Background b;
    b.config = "f1=0xbdbdbd,r1=18.5,m1=-72,r10=3";
    CHECK_NEAR(backgroundSetting(b, "r1", 24), 18.5, 1e-9);
    CHECK_NEAR(backgroundSetting(b, "m1", 72), -72, 1e-9);
    CHECK_NEAR(backgroundSetting(b, "r10", 0), 3, 1e-9);
    CHECK_NEAR(backgroundSetting(b, "lw", 0.5), 0.5, 1e-9);  // not there
    b.config = "r1=abc,m1";
    CHECK_NEAR(backgroundSetting(b, "r1", 24), 24, 1e-9);
    CHECK_NEAR(backgroundSetting(b, "m1", 72), 72, 1e-9);

    // Each choice is stored the way Xournal++ stores it, defaults left out.
    struct Case {
        Paper paper;
        const char* style;
        const char* config;
    };
    for (const Case& c : {Case{{PaperKind::Blank, -1, false}, "plain", ""},
                          Case{{PaperKind::Ruled, 0, true}, "lined", "r1=18"},
                          Case{{PaperKind::Ruled, 1, true}, "lined", ""},
                          Case{{PaperKind::Ruled, 2, false}, "ruled", "r1=30"},
                          Case{{PaperKind::Grid, 0, false}, "graph", ""},
                          Case{{PaperKind::Grid, 1, false}, "graph", "r1=21.26"},
                          Case{{PaperKind::Grid, 2, false}, "graph", "r1=28.35"}}) {
        Background bg;
        bg.color = 0xffffc0;
        bg.xml = "<background type=\"solid\" style=\"dotted\"/>";
        setPaper(bg, c.paper);
        CHECK(bg.style == c.style && bg.config == c.config && bg.xml.empty() && bg.color == 0xffffc0);
        const auto back = paperOf(bg);
        CHECK(back && back->kind == c.paper.kind);
        if (back && c.paper.kind != PaperKind::Blank) CHECK(back->spacing == c.paper.spacing);
        if (back && c.paper.kind == PaperKind::Ruled) CHECK(back->margin == c.paper.margin);
    }
    // A spacing that isn't one of ours is kept when only the margin changes.
    Background custom;
    custom.style = "lined";
    custom.config = "r1=20";
    CHECK(paperOf(custom)->spacing == -1);
    setPaper(custom, {PaperKind::Ruled, -1, false});
    CHECK(custom.style == "ruled" && custom.config == "r1=20");
    custom.style = "dotted";
    CHECK(!paperOf(custom));
    custom.type = "pdf";
    custom.style = "plain";
    CHECK(!paperOf(custom));
}

TEST(paper_config_round_trips_through_xopp) {
    Document d;
    d.pages.push_back(makeBlankPage(200, 300, "lined"));
    d.pages.push_back(makeBlankPage(200, 300, "lined"));
    setPaper(d.pages[1].background, {PaperKind::Grid, 2, false});
    std::string err;
    const std::string path = tempPath("paper.xopp");
    CHECK(saveXopp(path, d, err));
    Document back;
    CHECK(loadXopp(path, back, err));
    CHECK(back.pages.size() == 2);
    if (back.pages.size() != 2) return;
    CHECK(back.pages[0].background.style == "lined" && back.pages[0].background.config.empty());
    CHECK(back.pages[1].background.style == "graph" && back.pages[1].background.config == "r1=28.35");
    CHECK(back.pages[1].background.xml.find("config=\"r1=28.35\"") != std::string::npos);
}

TEST(draws_paper_spacing_margin_and_footer) {
    auto page = [](const char* style, const char* config) {
        Page p = makeBlankPage(200, 200, style);
        p.background.config = config;
        return p;
    };
    {
        Canvas c(200, 200);
        drawPage(c, page("ruled", "r1=30"), PageTransform{}, c.rect());
        CHECK(c.at(100, 80) == 200 && c.at(100, 110) == 200 && c.at(100, 140) == 200);
        CHECK(c.at(100, 104) == 255);
        CHECK(c.at(100, 170) == 255);  // in the 60pt footer, like Xournal++
        CHECK(c.at(72, 150) == 255);   // no margin line
    }
    {
        Canvas c(200, 200);
        drawPage(c, page("lined", "m1=-72"), PageTransform{}, c.rect());
        CHECK(c.at(128, 150) == 170 && c.at(72, 150) == 255);  // margin on the right
    }
    {
        Canvas c(200, 200);
        drawPage(c, page("graph", "r1=20"), PageTransform{}, c.rect());
        CHECK(c.at(20, 5) == 200 && c.at(40, 5) == 200 && c.at(5, 60) == 200);
        CHECK(c.at(30, 5) == 255 && c.at(0, 5) == 255);
    }
    {
        Canvas c(200, 200);
        drawPage(c, page("graph", "r1=0"), PageTransform{}, c.rect());  // nonsense: the default
        CHECK(c.at(14, 5) == 200);
    }
}

TEST(paper_menu_changes_this_page_only) {
    EditorOptions o;
    o.render.antialias = false;
    const std::string path = tempPath("paper-menu.xopp");
    Editor e(1860, 2480, Document{}, path, o);
    EinkPanel panel(1860, 2480);
    std::vector<Damage> last;
    auto flush = [&] {
        last = e.takeDamage();
        for (const Damage& d : last) panel.refresh(e.canvas(), d);
    };
    flush();
    uint64_t t = 1000;
    const PaperMenu& menu = e.paperMenu();
    auto tapAt = [&](const IRect& r) {
        CHECK(!r.empty());
        const IRect c = center(r);
        e.touchDown(c.x0, c.y0, t += 10);
        flush();
    };
    auto tap = [&](Action a) { tapAt(menu.buttonRect(a)); };
    auto bg = [&](size_t i) { return e.document().pages[i].background; };

    // PAGE opens the menu below it, quickly.
    tapAt(e.toolbar().buttonRect(Action::PageStyle));
    CHECK(menu.open() && menu.rect().y0 >= e.toolbar().rect().y1);
    for (const Damage& d : last) CHECK(d.mode == Refresh::UI);
    CHECK(!menu.buttonRect(Action::PaperMargin).empty());  // a new notebook is ruled, with a margin

    // GRID, then LARGE: the page changes behind the menu; there's no margin for grids.
    tap(Action::PaperGrid);
    CHECK(bg(0).style == "graph" && bg(0).config == "r1=21.26");  // keeps "medium"
    CHECK(menu.buttonRect(Action::PaperMargin).empty() && !menu.buttonRect(Action::PaperWide).empty());
    tap(Action::PaperWide);
    CHECK(bg(0).style == "graph" && bg(0).config == "r1=28.35");
    CHECK(e.modified());

    // PAGE again closes it; the whole visit is one undo step.
    tapAt(e.toolbar().buttonRect(Action::PageStyle));
    CHECK(!menu.open());
    CHECK_NEAR(panelMismatches(panel, e.canvas()), 0, 0);
    e.perform(Action::Undo, t += 10);
    flush();
    CHECK(bg(0).style == "lined" && bg(0).config.empty());
    e.perform(Action::Redo, t += 10);
    flush();
    CHECK(bg(0).style == "graph" && bg(0).config == "r1=28.35");

    // A new page starts like the one before it, and then has its own paper.
    e.perform(Action::NextPage, t += 10);
    flush();
    CHECK(e.pageIndex() == 1 && bg(1).style == "graph" && bg(1).config == "r1=28.35");
    tapAt(e.toolbar().buttonRect(Action::PageStyle));
    tap(Action::PaperRuled);
    CHECK(bg(1).style == "lined" && bg(1).config == "r1=30");  // "large" becomes "wide"
    tap(Action::PaperMargin);
    tap(Action::PaperNarrow);
    CHECK(bg(1).style == "ruled" && bg(1).config == "r1=18");
    e.touchDown(1000, 2000, t += 10);  // a finger elsewhere closes it
    flush();
    CHECK(!menu.open() && bg(0).style == "graph");

    // Writing beside the open menu closes it when the pen lifts, and the stroke counts.
    tapAt(e.toolbar().buttonRect(Action::PageStyle));
    tap(Action::PaperBlank);
    CHECK(bg(1).style == "plain" && bg(1).config.empty());
    CHECK(menu.buttonRect(Action::PaperNarrow).empty());
    e.penDown({900, 1500, 0.5, false}, t += 10);
    for (int i = 1; i <= 10; ++i) e.penMove({900.0 + 30 * i, 1500, 0.5, false}, t += 5);
    flush();
    e.penUp(t += 5);
    flush();
    CHECK(!menu.open() && e.document().pages[1].layers.back().elements.size() == 1);

    e.onIdle(t += 1000);
    flush();
    CHECK_NEAR(panelMismatches(panel, e.canvas()), 0, 0);

    // And it's all in the file.
    CHECK(e.save(t += 10));
    Document saved;
    std::string err;
    CHECK(loadXopp(path, saved, err) && saved.pages.size() == 2);
    if (saved.pages.size() == 2) {
        CHECK(saved.pages[0].background.style == "graph" && saved.pages[0].background.config == "r1=28.35");
        CHECK(saved.pages[1].background.style == "plain");
    }
}

TEST(paper_menu_leaves_pdf_pages_alone) {
    Document d;
    d.pages.push_back(makeBlankPage(446, 566, "plain"));
    d.pages[0].background.type = "pdf";
    d.pages[0].background.xml = "<background type=\"pdf\" domain=\"absolute\" filename=\"x.pdf\" pageno=\"1\"/>";
    Editor e(1860, 2480, d, tempPath("paper-pdf.xopp"));
    e.perform(Action::PageStyle, 10);
    CHECK(!e.paperMenu().open());
    CHECK(e.document().pages[0].background.type == "pdf" && !e.document().pages[0].background.xml.empty());
}

TEST(delete_page_takes_two_taps_and_undoes) {
    // Three pages, told apart by how many strokes they have.
    Document d;
    for (int k = 0; k < 3; ++k) {
        d.pages.push_back(makeBlankPage(446, 566, "lined"));
        for (int i = 0; i <= k; ++i) {
            Stroke s;
            s.points = {{100, 200.0 + 30 * i}, {300, 200.0 + 30 * i}};
            d.pages[k].layers[0].elements.push_back(s);
        }
    }
    d.pages[2].background.type = "pdf";  // the last one has a PDF background
    EditorOptions o;
    o.render.antialias = false;
    Editor e(1860, 2480, d, tempPath("delete-page.xopp"), o);
    EinkPanel panel(1860, 2480);
    auto flush = [&] {
        for (const Damage& dmg : e.takeDamage()) panel.refresh(e.canvas(), dmg);
    };
    flush();
    uint64_t t = 1000;
    const PaperMenu& menu = e.paperMenu();
    auto tapAt = [&](const IRect& r) {
        CHECK(!r.empty());
        const IRect c = center(r);
        e.touchDown(c.x0, c.y0, t += 10);
        flush();
    };
    auto openMenu = [&] { tapAt(e.toolbar().buttonRect(Action::PageStyle)); };
    auto strokesOn = [&](size_t i) { return e.document().pages[i].layers[0].elements.size(); };

    // On the middle page: the first tap only arms it.
    e.perform(Action::NextPage, t += 10);
    openMenu();
    tapAt(menu.buttonRect(Action::DeletePage));
    CHECK(e.document().pages.size() == 3 && menu.open());
    // Closing the menu disarms it.
    openMenu();
    openMenu();
    tapAt(menu.buttonRect(Action::DeletePage));
    CHECK(e.document().pages.size() == 3);
    tapAt(menu.buttonRect(Action::DeletePage));
    CHECK(e.document().pages.size() == 2 && !menu.open());
    CHECK(e.pageIndex() == 1 && strokesOn(1) == 3);  // the next page takes its place
    CHECK(e.modified());

    // UNDO brings it back, where it was; REDO deletes it again.
    e.perform(Action::Undo, t += 10);
    flush();
    CHECK(e.document().pages.size() == 3 && e.pageIndex() == 1 && strokesOn(1) == 2);
    e.perform(Action::Redo, t += 10);
    flush();
    CHECK(e.document().pages.size() == 2 && strokesOn(1) == 3);
    e.perform(Action::Undo, t += 10);
    flush();

    // The PDF page's menu only offers DELETE PAGE; deleting the last page shows the one before.
    e.perform(Action::NextPage, t += 10);
    flush();
    CHECK(e.pageIndex() == 2);
    openMenu();
    CHECK(menu.buttonRect(Action::PaperBlank).empty() && !menu.buttonRect(Action::DeletePage).empty());
    tapAt(menu.buttonRect(Action::DeletePage));
    tapAt(menu.buttonRect(Action::DeletePage));
    CHECK(e.document().pages.size() == 2 && e.pageIndex() == 1 && strokesOn(1) == 2);

    // A paper change and a delete in one visit are two undo steps.
    openMenu();
    tapAt(menu.buttonRect(Action::PaperGrid));
    tapAt(menu.buttonRect(Action::DeletePage));
    tapAt(menu.buttonRect(Action::DeletePage));
    CHECK(e.document().pages.size() == 1);
    e.perform(Action::Undo, t += 10);
    flush();
    CHECK(e.document().pages.size() == 2 && e.document().pages[1].background.style == "graph");
    e.perform(Action::Undo, t += 10);
    flush();
    CHECK(e.document().pages[1].background.style == "lined");

    // Down to one page, which can't be deleted.
    openMenu();
    tapAt(menu.buttonRect(Action::DeletePage));
    tapAt(menu.buttonRect(Action::DeletePage));
    openMenu();
    CHECK(e.document().pages.size() == 1 && menu.open() && menu.buttonRect(Action::DeletePage).empty());
    openMenu();

    e.onIdle(t += 1000);
    flush();
    CHECK_NEAR(panelMismatches(panel, e.canvas()), 0, 0);
}

TEST(pen_side_button_highlights_with_any_tool) {
    Editor e(900, 1200, Document{}, tempPath("side-button.xopp"));
    const auto& els = e.document().pages[0].layers.back().elements;
    uint64_t t = 0;
    auto stroke = [&](double y, bool eraser, bool button) {
        e.penDown({200, y, 0.5, eraser, button}, t += 10);
        for (int i = 1; i <= 10; ++i) e.penMove({200.0 + 30 * i, y, 0.5, eraser, button}, t += 5);
        e.penUp(t += 5);
    };
    e.perform(Action::Highlighter, t += 10);
    e.perform(Action::SizeThick, t += 10);  // the highlighter's size is the one used
    for (Action tool : {Action::Pen, Action::Eraser, Action::Lasso}) {
        e.perform(tool, t += 10);
        const size_t before = els.size();
        stroke(400.0 + 50 * before, false, true);
        CHECK(els.size() == before + 1);
        if (els.size() != before + 1) continue;
        const Stroke& s = std::get<Stroke>(els.back());
        CHECK(s.tool == StrokeTool::Highlighter && s.width > 19);
    }
    CHECK(e.tool() == ToolKind::Lasso);  // the button doesn't change the tool
    // With the pen, a stroke without the button is ink again; the eraser end still erases.
    e.perform(Action::Pen, t += 10);
    stroke(700, false, false);
    CHECK(els.size() == 4 && std::get<Stroke>(els.back()).tool == StrokeTool::Pen);
    e.penDown({150, 700, 0.5, true, false}, t += 10);
    e.penMove({600, 700, 0.5, true, false}, t += 10);
    e.penUp(t += 10);
    CHECK(els.size() == 3);
}

TEST(exit_buttons_show_they_were_pressed) {
    const std::string dir = tempDir("pressed-exit");
    std::string err;
    CHECK(saveXopp(dir + "/One.xopp", oneStrokeDocument(), err));
    AppOptions o;
    o.notebooksDir = dir;
    uint64_t t = 0;
    App app(1860, 2480, o);
    app.takeDamage();
    auto pressed = [&](const Toolbar& tb, Action a) {
        const IRect r = tb.buttonRect(a);
        return app.canvas().at(r.x0 + 6, r.y0 + 6) == 0;  // inverted: black inside
    };
    tapAt(app, app.library()->rowRect(0), t);
    idle(app, t);
    if (!app.editor()) return removeDir(dir);
    for (Action a : {Action::Exit, Action::Quit}) {  // BACK, then EXIT
        if (a == Action::Quit) {
            tapAt(app, app.library()->rowRect(0), t);
            idle(app, t);
        }
        writeIn(app, t);
        CHECK(!pressed(app.editor()->toolbar(), a));
        app.takeDamage();  // what opening and writing drew
        tapButton(app, app.editor()->toolbar(), a, t);
        // Before saving or leaving, the pressed button goes to the screen, quickly.
        CHECK(app.editor() && app.editor()->modified() && pressed(app.editor()->toolbar(), a));
        const auto dmg = app.takeDamage();
        CHECK(!dmg.empty() && hasMode(dmg, Refresh::UI));
        app.onIdle(t += 10);  // the pass that presents it
        CHECK(app.editor() != nullptr);
        app.onIdle(t += 10);
        CHECK(!app.editor() || app.quitRequested());
        CHECK(app.quitRequested() == (a == Action::Quit));
    }
    // The notebook list's EXIT too.
    App again(1860, 2480, o);
    again.takeDamage();
    tapButton(again, again.library()->toolbar(), Action::Exit, t);
    CHECK(again.quitRequested());
    const IRect r = again.library()->toolbar().buttonRect(Action::Exit);
    CHECK(again.canvas().at(r.x0 + 6, r.y0 + 6) == 0 && hasMode(again.takeDamage(), Refresh::UI));
    removeDir(dir);
}

// ---- about ----

TEST(about_text_has_the_version_build_and_license) {
    const BuildInfo b = buildInfo();
    CHECK(!b.version.empty() && b.version != "dev");
    const std::vector<std::string> t = aboutText({"Kindle Test (Codename), FBInk vX"}, "/mnt/us/scrible/notebooks");
    auto has = [&](const std::string& needle) {
        for (const std::string& line : t)
            if (line.find(needle) != std::string::npos) return true;
        return false;
    };
    CHECK(has("Kindle Test (Codename), FBInk vX") && has("Notebooks: /mnt/us/scrible/notebooks"));
    CHECK(has("GNU GPL, version 3 or later") && has("Copyright (C) 2026 Sam Stratter"));
    CHECK(has("Build ") == !b.commit.empty());
}

TEST(library_about_dialog_opens_and_closes) {
    for (auto [w, h] : {std::pair{1860, 2480}, std::pair{1072, 1448}}) {  // the Scribe, and a narrow Kindle
        const std::string dir = tempDir("about");
        AppOptions o;
        o.notebooksDir = dir;
        o.platformInfo = {"Kindle Test (Codename), FBInk v1.25.0-543-g886f25f1"};
        uint64_t t = 0;
        App app(w, h, o);
        Library& lib = *app.library();
        app.takeDamage();
        CHECK(!lib.toolbar().buttonRect(Action::About).empty());
        tapButton(app, lib.toolbar(), Action::About, t);
        const IRect d = lib.dialogRect();
        CHECK(!d.empty() && d.y0 >= lib.toolbar().rect().y1 && d.y1 <= h && d.x1 <= w);  // fits, wrapped
        CHECK(!lib.dialogButton(Action::Cancel).empty());
        for (const Damage& dmg : app.takeDamage()) CHECK(dmg.mode == Refresh::UI);
        tapAt(app, lib.dialogButton(Action::Cancel), t);
        CHECK(lib.dialogRect().empty());
        tapButton(app, lib.toolbar(), Action::About, t);
        app.touchDown(5, h - 5, t += 10);  // outside it
        CHECK(lib.dialogRect().empty() && !app.quitRequested());
        removeDir(dir);
    }
}

// ---- reading handwritten numbers ----

namespace {

using Path = std::vector<StrokePoint>;

// Digits drawn the way people often write them, in a unit box (y down).
const std::vector<std::vector<Path>>& digitShapes() {
    static const std::vector<std::vector<Path>> shapes = {
        {{{0.5, 0}, {0.15, 0.2}, {0.1, 0.5}, {0.2, 0.85}, {0.5, 1}, {0.8, 0.85}, {0.9, 0.5}, {0.8, 0.15}, {0.5, 0}}},
        {{{0.5, 0}, {0.5, 1}}},
        {{{0.15, 0.25}, {0.35, 0.02}, {0.7, 0.02}, {0.85, 0.25}, {0.6, 0.55}, {0.1, 1}, {0.9, 1}}},
        {{{0.15, 0.1}, {0.5, 0}, {0.85, 0.15}, {0.75, 0.4}, {0.4, 0.5}, {0.8, 0.6}, {0.85, 0.85}, {0.5, 1}, {0.15, 0.9}}},
        {{{0.6, 0}, {0.05, 0.65}, {0.95, 0.65}}, {{0.7, 0.3}, {0.7, 1}}},
        {{{0.25, 0}, {0.2, 0.45}, {0.6, 0.4}, {0.85, 0.6}, {0.8, 0.9}, {0.5, 1}, {0.15, 0.9}}, {{0.25, 0}, {0.85, 0}}},
        {{{0.8, 0}, {0.4, 0.2}, {0.15, 0.6}, {0.25, 0.95}, {0.55, 1}, {0.8, 0.8}, {0.65, 0.55}, {0.3, 0.6}, {0.18, 0.7}}},
        {{{0.1, 0}, {0.9, 0}, {0.4, 1}}},
        {{{0.8, 0.12}, {0.5, 0}, {0.2, 0.15}, {0.3, 0.4}, {0.75, 0.6}, {0.8, 0.88}, {0.5, 1}, {0.2, 0.88}, {0.25, 0.6},
          {0.7, 0.4}, {0.8, 0.15}}},
        {{{0.8, 0.2}, {0.5, 0}, {0.2, 0.15}, {0.25, 0.4}, {0.55, 0.45}, {0.8, 0.25}, {0.8, 1}}},
    };
    return shapes;
}

// The paths of `text` ("0".."9", ".", ",", "-", " ") written from (x, y), each
// character w by h: as many points as a pen would give.
std::vector<Path> writePaths(const std::string& text, double x, double y, double w, double h) {
    std::vector<Path> out;
    auto add = [&](const Path& unit, double cx) {
        Path p;
        for (size_t i = 0; i + 1 < unit.size(); ++i)
            for (int k = 0; k < 10; ++k) {
                const double t = k / 10.0;
                p.push_back({cx + w * (unit[i].x + (unit[i + 1].x - unit[i].x) * t),
                             y + h * (unit[i].y + (unit[i + 1].y - unit[i].y) * t)});
            }
        p.push_back({cx + w * unit.back().x, y + h * unit.back().y});
        out.push_back(p);
    };
    for (char c : text) {
        if (c >= '0' && c <= '9') {
            for (const Path& unit : digitShapes()[static_cast<size_t>(c - '0')]) add(unit, x);
            x += w * 1.4;
        } else if (c == '.') {
            add({{0.1, 0.95}, {0.14, 0.98}}, x);
            x += w * 0.5;
        } else if (c == ',') {
            add({{0.15, 0.92}, {0.05, 1.18}}, x);
            x += w * 0.5;
        } else if (c == '-') {
            add({{0, 0.5}, {0.7, 0.5}}, x);
            x += w * 1.1;
        } else if (c == 'C') {  // a long comma, hanging below the line
            add({{0.12, 0.6}, {0.02, 1.25}}, x);
            x += w * 0.5;
        } else if (c == 'N') {  // a 9 as a loop and then its tail
            add({{0.8, 0.2}, {0.5, 0}, {0.2, 0.15}, {0.25, 0.4}, {0.55, 0.45}, {0.8, 0.25}}, x);
            add({{0.8, 0.35}, {0.78, 1}}, x);
            x += w * 1.4;
        } else if (c == 'S') {  // a 7 with its bar drawn first
            add({{0.05, 0}, {0.9, 0}}, x);
            add({{0.9, 0.02}, {0.4, 1}}, x);
            x += w * 1.4;
        } else if (c == 'o') {  // a small 0, sitting low
            Path zero;
            for (const StrokePoint& p : digitShapes()[0][0]) zero.push_back({0.1 + 0.8 * p.x, 0.35 + 0.65 * p.y});
            add(zero, x);
            x += w * 1.2;
        } else {
            x += w * 2;
        }
    }
    return out;
}

std::vector<Stroke> asStrokes(const std::vector<Path>& paths) {
    std::vector<Stroke> out;
    for (const Path& p : paths) {
        Stroke s;
        s.width = 1.41;
        s.points = p;
        out.push_back(s);
    }
    return out;
}

}  // namespace

TEST(digit_reader_reads_other_peoples_handwriting) {
    // Samples of 30 writers, tested on the 14 others: the published figure is ~97-98%.
    const DigitReader r(false);
    size_t ok = 0;
    for (size_t i = 0; i < DigitReader::testCount(); ++i) {
        int d;
        const DigitFeatures f = DigitReader::testSample(i, d);
        ok += r.classify(f) == d;
    }
    CHECK(ok * 100 >= DigitReader::testCount() * 97);
}

TEST(digit_reader_reads_strokes_and_learns) {
    const DigitReader shared;
    for (int d = 0; d < 10; ++d) {
        const std::vector<Stroke> ss = asStrokes(writePaths(std::string(1, char('0' + d)), 100, 100, 12, 20));
        std::vector<const Stroke*> ptrs;
        for (const Stroke& s : ss) ptrs.push_back(&s);
        CHECK(shared.classify(digitFeatures(ptrs)) == d);
    }
    // A writer's own examples win over everyone else's: teach it that a 7 is a 1.
    DigitReader r;
    const std::vector<Stroke> seven = asStrokes(writePaths("7", 100, 100, 12, 20));
    const DigitFeatures f = digitFeatures({&seven[0]});
    CHECK(r.classify(f) == 7);
    for (int i = 0; i < 3; ++i) r.addExample(f, 1);
    CHECK(r.classify(f) == 1);
    // Kept in a file, and read back.
    const std::string path = tempPath("digits.txt");
    std::remove(path.c_str());
    CHECK(r.appendExample(path, f, 1) && r.appendExample(path, f, 1));
    DigitReader again;
    CHECK(again.loadExamples(path) && again.exampleCount() == 2);
    std::remove(path.c_str());
}

TEST(reads_a_written_list_of_numbers) {
    std::vector<Path> paths;
    const char* lines[] = {"12", "7.5", "-30", "1,250", "4  96"};
    for (int i = 0; i < 5; ++i)
        for (const Path& p : writePaths(lines[i], 50, 50 + 40.0 * i, 12, 20)) paths.push_back(p);
    const std::vector<ReadNumber> nums = readNumbers(asStrokes(paths), DigitReader());
    std::vector<std::string> texts;
    for (const ReadNumber& n : nums) texts.push_back(n.text());
    CHECK(texts == (std::vector<std::string>{"12", "7.5", "-30", "1,250", "4", "96"}));
    double v = 0;
    const double expected[] = {12, 7.5, -30, 1250, 4, 96};
    for (size_t i = 0; i < nums.size() && i < 6; ++i) CHECK(nums[i].parse(v) && v == expected[i]);
}

TEST(reads_commas_tails_and_bars_as_people_write_them) {
    // Habits from a real handwriting test page: long commas that hang below the line,
    // a 9 written as a loop and then a tail, a 7 with its bar drawn first, small 0s.
    std::vector<Path> paths;
    const char* lines[] = {"1C2N9", "S5", "1CoooCoo1", "NNNCNNNC999.9"};
    for (int i = 0; i < 4; ++i)
        for (const Path& p : writePaths(lines[i], 50, 50 + 40.0 * i, 12, 20)) paths.push_back(p);
    std::vector<std::string> texts;
    for (const ReadNumber& n : readNumbers(asStrokes(paths), DigitReader())) texts.push_back(n.text());
    CHECK(texts == (std::vector<std::string>{"1,299", "75", "1,000,001", "999,999,999.9"}));
}

TEST(reads_lines_that_run_uphill_or_downhill) {
    // On blank paper nothing keeps the writing level: tilt it both ways.
    const char* lines[] = {"12", "7.5", "-30", "1C2N9", "1CoooCoo1", "4  96"};
    const std::vector<std::string> expected = {"12", "7.5", "-30", "1,299", "1,000,001", "4", "96"};
    for (double degrees : {-6.0, 6.0}) {
        const double a = degrees * 3.14159265 / 180, c = std::cos(a), sn = std::sin(a);
        std::vector<Path> paths;
        for (int i = 0; i < 6; ++i)
            for (Path p : writePaths(lines[i], 50, 50 + 40.0 * i, 12, 20)) {
                for (StrokePoint& q : p) q = {100 + (q.x - 100) * c - (q.y - 150) * sn, 150 + (q.x - 100) * sn + (q.y - 150) * c};
                paths.push_back(p);
            }
        std::vector<std::string> texts;
        for (const ReadNumber& r : readNumbers(asStrokes(paths), DigitReader())) texts.push_back(r.text());
        CHECK(texts == expected);
    }
}

TEST(numbers_follow_the_writing_rules) {
    auto parses = [](const std::string& t, double& v) {
        ReadNumber n;
        for (char c : t) n.chars.push_back({c, {}, {}, {}});
        return n.parse(v);
    };
    double v = 0;
    CHECK(parses("1,250,000.5", v) && v == 1250000.5);
    CHECK(parses("-0.25", v) && v == -0.25);
    CHECK(parses("42", v) && v == 42);
    for (const char* bad : {"1,25", "1250,000", ",5", "1.", ".", "-", "1.2.3", "4?", "1,2345"}) CHECK(!parses(bad, v));
}

TEST(selection_math_reads_and_can_be_corrected) {
    const std::string dir = tempDir("math");
    EditorOptions o;
    o.render.antialias = false;
    o.digitExamplesPath = dir + "/.scrible-digits";
    Editor e(1860, 2480, Document{}, dir + "/math.xopp", o);
    EinkPanel panel(1860, 2480);
    auto flush = [&] {
        for (const Damage& d : e.takeDamage()) panel.refresh(e.canvas(), d);
    };
    uint64_t t = 1000;
    auto pen = [&](const Path& p) {
        e.penDown({p[0].x, p[0].y, 0.5, false}, t += 10);
        for (size_t i = 1; i < p.size(); ++i) e.penMove({p[i].x, p[i].y, 0.5, false}, t += 5);
        e.penUp(t += 5);
        flush();
    };
    auto tap = [&](const IRect& r) {
        CHECK(!r.empty());
        const IRect c = center(r);
        e.touchDown(c.x0, c.y0, t += 10);
        flush();
    };
    // A list, written at about the Scribe's handwriting size.
    const char* lines[] = {"12", "7.5", "-30"};
    for (int i = 0; i < 3; ++i)
        for (const Path& p : writePaths(lines[i], 700, 900 + 160.0 * i, 60, 100)) pen(p);
    e.onIdle(t += 1000);
    flush();

    // Select it; SUM is under MATH in the selection's menu, with the other math.
    e.perform(Action::Lasso, t += 10);
    pen({{600, 850}, {1200, 850}, {1200, 1360}, {600, 1360}, {600, 860}});
    CHECK(e.selection().size() == 9);  // a 5 takes two strokes
    tap(e.selectionButton(Action::MathMenu));
    for (Action a : {Action::MathSum, Action::MathAverage, Action::MathMin, Action::MathMax, Action::MathCount})
        CHECK(!e.selectionButton(a).empty());
    tap(e.selectionButton(Action::MathSum));
    const MathPopup& m = e.mathPopup();
    CHECK(m.open() && m.numbers().size() == 3);
    CHECK(m.result() == "SUM OF 3 = -10.5");

    // Tap the 2 of "12", make it a 7: the answer follows, and the reader learns.
    tap(m.cellRect(0, 1));
    tap(m.keyRect('7'));
    CHECK(m.numbers()[0].text() == "17" && m.result() == "SUM OF 3 = -5.5");
    std::ifstream kept(o.digitExamplesPath);
    std::string line;
    CHECK(std::getline(kept, line) && line[0] == '7');

    // DONE closes it; the selection stays. Other math works the same way.
    tap(m.doneRect());
    CHECK(!m.open() && !e.selection().empty());
    tap(e.selectionButton(Action::MathMax));
    CHECK(m.result() == "MAX OF 3 = 17");  // read afresh: that 2 now reads as the 7 it was taught to be
    e.touchDown(100, 2400, t += 10);  // a tap elsewhere closes it
    flush();
    CHECK(!m.open());

    // A restarted editor reads with the example it was taught.
    Editor again(1860, 2480, Document{}, dir + "/other.xopp", o);
    (void)again;
    DigitReader reader;
    CHECK(reader.loadExamples(o.digitExamplesPath) && reader.exampleCount() == 1);

    e.onIdle(t += 1000);
    flush();
    CHECK_NEAR(panelMismatches(panel, e.canvas()), 0, 0);
    removeDir(dir);
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
