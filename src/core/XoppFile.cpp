#include "XoppFile.h"

#include <unistd.h>
#include <zlib.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include <pugixml.hpp>

#ifndef SCRIBLE_VERSION
#define SCRIBLE_VERSION "dev"
#endif

namespace scrible {
namespace {

constexpr const char* kCreator = "Scrible " SCRIBLE_VERSION;

void warn(std::vector<std::string>* warnings, std::string msg) {
    if (warnings) warnings->push_back(std::move(msg));
}

// Whitespace-separated numbers. Stops at the first token that isn't a finite
// number, like Xournal++ does. strtod follows the C locale, which we never change.
std::vector<double> parseNumbers(const char* s) {
    std::vector<double> out;
    for (;;) {
        char* end = nullptr;
        const double v = std::strtod(s, &end);
        if (end == s || !std::isfinite(v)) break;
        out.push_back(v);
        s = end;
    }
    return out;
}

struct NamedColor {
    const char* name;
    uint32_t rgb;
};

// Predefined colors of the original Xournal, still found in old .xoj files.
constexpr NamedColor kStrokeColors[] = {
    {"black", 0x000000},     {"blue", 0x3333cc},       {"red", 0xff0000},     {"green", 0x008000},
    {"gray", 0x808080},      {"lightblue", 0x00c0ff},  {"lightgreen", 0x00ff00}, {"magenta", 0xff00ff},
    {"orange", 0xff8000},    {"yellow", 0xffff00},     {"white", 0xffffff},
};
constexpr NamedColor kBackgroundColors[] = {
    {"white", 0xffffff}, {"yellow", 0xffff80}, {"pink", 0xffc0d4},
    {"orange", 0xffc080}, {"blue", 0xa0e8ff},  {"green", 0x80ffc0},
};

// Accepts "#rrggbbaa", "#rrggbb" or a named color. Alpha is dropped (see Stroke::color).
template <size_t N>
bool parseColor(const char* s, uint32_t& rgb, const NamedColor (&names)[N]) {
    if (s[0] == '#') {
        char* end = nullptr;
        const unsigned long v = std::strtoul(s + 1, &end, 16);
        const size_t digits = static_cast<size_t>(end - (s + 1));
        if (*end != '\0') return false;
        if (digits == 8) rgb = static_cast<uint32_t>(v >> 8);
        else if (digits == 6) rgb = static_cast<uint32_t>(v);
        else return false;
        return true;
    }
    for (const auto& c : names) {
        if (std::strcmp(s, c.name) == 0) {
            rgb = c.rgb;
            return true;
        }
    }
    return false;
}

std::string nodeXml(const pugi::xml_node& n) {
    std::ostringstream ss;
    n.print(ss, "", pugi::format_raw);
    return ss.str();
}

bool parseStroke(const pugi::xml_node& n, Stroke& s, std::vector<std::string>* warnings) {
    const std::string tool = n.attribute("tool").as_string("pen");
    s.tool = tool == "highlighter" ? StrokeTool::Highlighter
             : tool == "eraser"    ? StrokeTool::Eraser
                                   : StrokeTool::Pen;
    if (!parseColor(n.attribute("color").as_string("black"), s.color, kStrokeColors)) s.color = 0x000000;

    // width="W p0 p1 ..." : nominal width, then one pressure-adjusted width per segment.
    // MrWriter puts the per-segment values in a separate "pressures" attribute.
    const std::vector<double> widths = parseNumbers(n.attribute("width").as_string("1"));
    s.width = widths.empty() ? 1.0 : widths[0];
    std::vector<double> pressures;
    if (auto p = n.attribute("pressures")) pressures = parseNumbers(p.as_string());
    else if (widths.size() > 1) pressures.assign(widths.begin() + 1, widths.end());

    const std::vector<double> coords = parseNumbers(n.child_value());
    if (coords.size() % 2) warn(warnings, "stroke with an odd number of coordinates; dropped the last one");
    for (size_t i = 0; i + 1 < coords.size(); i += 2) s.points.push_back({coords[i], coords[i + 1]});
    if (s.points.empty()) return false;

    if (!pressures.empty()) {
        // Same repairs Xournal++ applies to inconsistent files.
        if (pressures.size() + 1 < s.points.size()) {
            warn(warnings, "stroke with fewer pressure values than segments; truncated");
            s.points.resize(pressures.size() + 1);
        }
        if (pressures.size() >= s.points.size()) pressures.resize(s.points.size() - 1);
        s.segmentWidths = std::move(pressures);
    }
    // Xournal++ ignores strokes with fewer than two points, so dots are stored as two equal points.
    if (s.points.size() == 1) s.points.push_back(s.points[0]);

    for (const auto& a : n.attributes()) {
        const char* name = a.name();
        if (!std::strcmp(name, "tool") || !std::strcmp(name, "color") || !std::strcmp(name, "width") ||
            !std::strcmp(name, "pressures"))
            continue;
        s.extraAttrs.emplace_back(name, a.value());
    }
    return true;
}

ForeignElement parseForeign(const pugi::xml_node& n) {
    ForeignElement f;
    f.tag = n.name();
    f.xml = nodeXml(n);
    if (n.attribute("left") && n.attribute("bottom")) {  // <image>, <teximage> (file version 4 layout)
        f.bounds.add(n.attribute("left").as_double(), n.attribute("top").as_double());
        f.bounds.add(n.attribute("right").as_double(), n.attribute("bottom").as_double());
    } else if (f.tag == "text") {
        f.textSize = n.attribute("size").as_double(12);
        if (const auto m = n.attribute("matrix")) {
            // File version 5: "xx yx xy yy x0 y0". Rotation/shear is ignored; the scale goes into the size.
            const std::vector<double> v = parseNumbers(m.as_string());
            if (v.size() != 6) return f;
            f.textX = v[4];
            f.textY = v[5];
            f.textSize *= std::sqrt(std::fabs(v[0] * v[3] - v[1] * v[2]));
        } else if (n.attribute("x")) {
            f.textX = n.attribute("x").as_double();
            f.textY = n.attribute("y").as_double();
        } else {
            return f;
        }
        f.text = n.child_value();
        f.textWrap = n.attribute("wrap").as_double(0);
        if (!parseColor(n.attribute("color").as_string("black"), f.color, kStrokeColors)) f.color = 0;
        // The box the bitmap-font rendering occupies (not the real font's metrics).
        const std::vector<std::string> lines = textLines(f);
        size_t longest = 0;
        for (const auto& l : lines) longest = std::max(longest, codePoints(l));
        f.bounds.add(f.textX, f.textY);
        f.bounds.add(f.textX + longest * f.textSize, f.textY + lines.size() * f.textSize * 1.25);
    }
    return f;
}

// ---- writing ----

void appendEscaped(std::string& out, const std::string& s) {
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
}

// Fixed decimals with trailing zeros trimmed: 1.50 -> "1.5", 2.00 -> "2".
void appendNumber(std::string& out, double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    char* end = buf + std::strlen(buf);
    if (std::strchr(buf, '.')) {
        while (end[-1] == '0') --end;
        if (end[-1] == '.') --end;
    }
    *end = '\0';
    if (std::strcmp(buf, "-0") == 0) std::strcpy(buf, "0");
    out += buf;
}

void appendColor(std::string& out, uint32_t rgb, uint8_t alpha) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "#%06x%02x", rgb & 0xffffff, alpha);
    out += buf;
}

void appendStroke(std::string& out, const Stroke& s) {
    out += "<stroke tool=\"";
    out += s.tool == StrokeTool::Highlighter ? "highlighter" : s.tool == StrokeTool::Eraser ? "eraser" : "pen";
    out += "\" color=\"";
    appendColor(out, s.color, s.tool == StrokeTool::Highlighter ? 0x7f : 0xff);
    out += "\" width=\"";
    appendNumber(out, s.width, 2);
    for (double w : s.segmentWidths) {
        out += ' ';
        appendNumber(out, w, 2);
    }
    out += '"';
    for (const auto& [name, value] : s.extraAttrs) {
        out += ' ';
        out += name;
        out += "=\"";
        appendEscaped(out, value);
        out += '"';
    }
    out += '>';
    for (size_t i = 0; i < s.points.size(); ++i) {
        if (i) out += ' ';
        appendNumber(out, s.points[i].x, 2);
        out += ' ';
        appendNumber(out, s.points[i].y, 2);
    }
    out += "</stroke>\n";
}

}  // namespace

bool parseXopp(const std::string& xml, Document& doc, std::string& error, std::vector<std::string>* warnings) {
    pugi::xml_document x;
    const pugi::xml_parse_result res = x.load_buffer(xml.data(), xml.size());
    if (!res) {
        error = std::string("XML parse error: ") + res.description() + " (offset " + std::to_string(res.offset) + ")";
        return false;
    }
    const pugi::xml_node root = x.child("xournal");
    if (!root) {
        error = x.child("MrWriter") ? "MrWriter files aren't supported" : "not a Xournal document";
        return false;
    }

    doc = Document{};
    doc.fileVersion = root.attribute("fileversion").as_int(1);
    for (const pugi::xml_node pn : root.children("page")) {
        Page page;
        page.width = pn.attribute("width").as_double();
        page.height = pn.attribute("height").as_double();
        if (!(page.width > 0 && page.height > 0)) {
            warn(warnings, "page without a valid size; using A4");
            page.width = 595.27;
            page.height = 841.89;
        }
        if (const pugi::xml_node bg = pn.child("background")) {
            page.background.type = bg.attribute("type").as_string("solid");
            if (!parseColor(bg.attribute("color").as_string("white"), page.background.color, kBackgroundColors))
                page.background.color = 0xffffff;
            page.background.style = bg.attribute("style").as_string("plain");
            page.background.config = bg.attribute("config").as_string();
            page.background.xml = nodeXml(bg);
        }
        for (const pugi::xml_node ln : pn.children("layer")) {
            Layer layer;
            layer.name = ln.attribute("name").as_string();
            for (const pugi::xml_node en : ln.children()) {
                if (en.type() != pugi::node_element) continue;
                if (std::strcmp(en.name(), "stroke") == 0) {
                    Stroke s;
                    if (parseStroke(en, s, warnings)) layer.elements.emplace_back(std::move(s));
                } else {
                    layer.elements.emplace_back(parseForeign(en));
                }
            }
            page.layers.push_back(std::move(layer));
        }
        if (page.layers.empty()) page.layers.emplace_back();
        doc.pages.push_back(std::move(page));
    }
    return true;
}

bool loadXopp(const std::string& path, Document& doc, std::string& error, std::vector<std::string>* warnings) {
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        unsigned char magic[4] = {};
        const size_t n = std::fread(magic, 1, sizeof magic, f);
        std::fclose(f);
        if (n == 4 && !std::memcmp(magic, "PK\x03\x04", 4)) {
            error = "zip-packaged .xopp files aren't supported yet";
            return false;
        }
    }

    // gzread reads gzip and passes uncompressed files through unchanged.
    gzFile gz = gzopen(path.c_str(), "rb");
    if (!gz) {
        error = "can't open " + path + ": " + std::strerror(errno);
        return false;
    }
    std::string xml;
    char buf[1 << 16];
    int n;
    while ((n = gzread(gz, buf, sizeof buf)) > 0) xml.append(buf, n);
    if (n < 0) {
        int code = 0;
        error = std::string("can't read ") + path + ": " + gzerror(gz, &code);
        gzclose(gz);
        return false;
    }
    gzclose(gz);
    return parseXopp(xml, doc, error, warnings);
}

std::string serializeXopp(const Document& doc) {
    std::string out;
    out.reserve(1 << 16);
    out += "<?xml version=\"1.0\" standalone=\"no\"?>\n<xournal creator=\"";
    appendEscaped(out, kCreator);
    out += "\" fileversion=\"" + std::to_string(std::max(doc.fileVersion, 4)) + "\">\n";
    out += "<title>Xournal++ document - see https://xournalpp.github.io/</title>\n";
    for (const Page& page : doc.pages) {
        out += "<page width=\"";
        appendNumber(out, page.width, 4);
        out += "\" height=\"";
        appendNumber(out, page.height, 4);
        out += "\">\n";
        if (!page.background.xml.empty()) {
            out += page.background.xml;
            out += '\n';
        } else {
            out += "<background type=\"solid\" color=\"";
            appendColor(out, page.background.color, 0xff);
            out += "\" style=\"";
            appendEscaped(out, page.background.style);
            if (!page.background.config.empty()) {  // as Xournal++ writes it; the original Xournal ignores it
                out += "\" config=\"";
                appendEscaped(out, page.background.config);
            }
            out += "\"/>\n";
        }
        if (page.layers.empty()) out += "<layer/>\n";  // the original Xournal needs at least one layer
        for (const Layer& layer : page.layers) {
            out += "<layer";
            if (!layer.name.empty()) {
                out += " name=\"";
                appendEscaped(out, layer.name);
                out += '"';
            }
            out += ">\n";
            for (const Element& e : layer.elements) {
                if (const auto* s = std::get_if<Stroke>(&e)) {
                    appendStroke(out, *s);
                } else {
                    out += std::get<ForeignElement>(e).xml;
                    out += '\n';
                }
            }
            out += "</layer>\n";
        }
        out += "</page>\n";
    }
    out += "</xournal>\n";
    return out;
}

bool saveXopp(const std::string& path, const Document& doc, std::string& error) {
    const std::string xml = serializeXopp(doc);

    z_stream zs{};
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16 /* gzip wrapper */, 8, Z_DEFAULT_STRATEGY) !=
        Z_OK) {
        error = "can't initialize zlib";
        return false;
    }
    std::string gz(deflateBound(&zs, static_cast<uLong>(xml.size())) + 64, '\0');
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(xml.data()));
    zs.avail_in = static_cast<uInt>(xml.size());
    zs.next_out = reinterpret_cast<Bytef*>(gz.data());
    zs.avail_out = static_cast<uInt>(gz.size());
    const int rc = deflate(&zs, Z_FINISH);
    gz.resize(zs.total_out);
    deflateEnd(&zs);
    if (rc != Z_STREAM_END) {
        error = "compression failed";
        return false;
    }

    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        error = "can't write " + tmp + ": " + std::strerror(errno);
        return false;
    }
    bool ok = std::fwrite(gz.data(), 1, gz.size(), f) == gz.size() && std::fflush(f) == 0 && fsync(fileno(f)) == 0;
    const int writeErrno = errno;
    if (std::fclose(f) != 0) ok = false;
    if (!ok) {
        error = "can't write " + tmp + ": " + std::strerror(writeErrno);
        std::remove(tmp.c_str());
        return false;
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        error = "can't replace " + path + ": " + std::strerror(errno);
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

}  // namespace scrible
