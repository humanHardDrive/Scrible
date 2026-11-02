// Renders one page of a .xopp/.xoj file to a grayscale PNG with the same
// renderer the Kindle uses. Handy for checking compatibility with files made
// in desktop Xournal++.
//
//   xopp-render notes.xopp page1.png [--page N] [--dpi 150]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "Png.h"
#include "Renderer.h"
#include "XoppFile.h"

int main(int argc, char** argv) {
    std::string in, out;
    int page = 1;
    double dpi = 150;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--page") && i + 1 < argc) page = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--dpi") && i + 1 < argc) dpi = std::atof(argv[++i]);
        else if (in.empty()) in = argv[i];
        else out = argv[i];
    }
    if (in.empty() || out.empty() || dpi <= 0) {
        std::fprintf(stderr, "usage: %s input.xopp output.png [--page N] [--dpi 150]\n", argv[0]);
        return 2;
    }

    xs::Document doc;
    std::string error;
    std::vector<std::string> warnings;
    if (!xs::loadXopp(in, doc, error, &warnings)) {
        std::fprintf(stderr, "%s: %s\n", in.c_str(), error.c_str());
        return 1;
    }
    for (const auto& w : warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
    if (page < 1 || static_cast<size_t>(page) > doc.pages.size()) {
        std::fprintf(stderr, "%s has %zu page(s)\n", in.c_str(), doc.pages.size());
        return 1;
    }

    const xs::Page& p = doc.pages[page - 1];
    const double scale = dpi / 72.0;
    xs::Canvas canvas(static_cast<int>(p.width * scale + 0.5), static_cast<int>(p.height * scale + 0.5));
    xs::PageTransform t;
    t.scale = scale;
    xs::drawPage(canvas, p, t, canvas.rect());
    if (!xs::writePng(out, canvas, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }

    size_t strokes = 0, other = 0;
    for (const auto& layer : p.layers)
        for (const auto& e : layer.elements) (std::holds_alternative<xs::Stroke>(e) ? strokes : other)++;
    std::printf("page %d/%zu: %.0fx%.0f pt, %zu layer(s), %zu stroke(s), %zu other element(s) -> %s\n", page,
                doc.pages.size(), p.width, p.height, p.layers.size(), strokes, other, out.c_str());
    return 0;
}
