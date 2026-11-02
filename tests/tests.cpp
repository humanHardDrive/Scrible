// Self-contained tests (no framework, so they also build with the Kindle toolchain).

#include <dirent.h>
#include <sys/time.h>
#include <unistd.h>
#include <zlib.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "App.h"
#include "Editor.h"
#include "EinkPanel.h"
#include "Eraser.h"
#include "Shapes.h"
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
    for (int width : {1072, 1236, 1860, 1986}) {  // Kindle screen widths
        const Editor e(width, 1200, Document{}, tempPath("toolbar-fit.xopp"));
        const Toolbar& tb = e.toolbar();
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
    e.perform(Action::Exit, 1000);
    CHECK(!e.closeRequested());
    e.perform(Action::Exit, 2000);
    CHECK(e.closeRequested());
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

void tapAt(App& app, const IRect& r, uint64_t& t) {
    const IRect c = center(r);
    app.touchDown(c.x0, c.y0, t += 10);
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
        CHECK(app.library()->notebooks().size() == expected);
    }

    // A typed name, through the on-screen keyboard.
    tapButton(app, app.library()->toolbar(), Action::NewNotebook, t);
    typeIn(app, "physics 2", t);
    app.perform(Action::Confirm, t += 10);
    CHECK(app.editor() && app.editor()->path() == dir + "/Physics 2.xopp");
    writeIn(app, t);
    app.perform(Action::Exit, t += 10);
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
        for (Action a : {Action::Pen, Action::Highlighter, Action::Eraser, Action::SizeFine, Action::SizeMedium,
                         Action::SizeThick, Action::Undo, Action::Redo, Action::PrevPage, Action::NextPage,
                         Action::Save, Action::Exit, Action::Quit}) {
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

    // Selecting ERASE, then tapping it again, switches to whole strokes.
    e.perform(Action::Eraser, t += 10);
    CHECK(!e.eraseWholeStrokes());
    e.perform(Action::Eraser, t += 10);
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
    e.perform(Action::Eraser, t += 10);
    e.perform(Action::Eraser, t += 10);
    CHECK(!e.eraseWholeStrokes());
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
        // Switch the eraser to whole strokes; BACK; another notebook starts that way.
        tapButton(app, app.editor()->toolbar(), Action::Eraser, t);
        tapButton(app, app.editor()->toolbar(), Action::Eraser, t);
        CHECK(app.editor()->eraseWholeStrokes());
        tapButton(app, app.editor()->toolbar(), Action::Exit, t);
        open(app, "Two");
        if (!app.editor()) return removeDir(dir);
        CHECK(app.editor()->eraseWholeStrokes());
        // EXIT saves and leaves the app straight from the notebook.
        tapButton(app, app.editor()->toolbar(), Action::Pen, t);
        writeIn(app, t);
        tapButton(app, app.editor()->toolbar(), Action::Quit, t);
        CHECK(app.quitRequested());
        Document saved;
        CHECK(loadXopp(dir + "/Two.xopp", saved, err) && saved.pages[0].layers.back().elements.size() == 2);
    }
    App again(900, 1200, o);  // and after a restart
    open(again, "One");
    if (again.editor()) CHECK(again.editor()->eraseWholeStrokes());
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
        e.perform(Action::Pen, t += 10);  // the pen is already selected: tapping it again turns shapes on
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

int main() {
    for (const auto& [name, fn] : registry()) {
        const int before = g_failures;
        fn();
        std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", name);
    }
    std::printf("%zu tests, %d failed checks\n", registry().size(), g_failures);
    return g_failures ? 1 : 0;
}
