#include "Paper.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace scrible {
namespace {

bool isRuled(const std::string& style) { return style == "ruled" || style == "lined"; }

int presetIndex(const double (&presets)[3], double v) {
    for (int i = 0; i < 3; ++i)
        if (std::fabs(presets[i] - v) < 0.01) return i;
    return -1;
}

}  // namespace

double backgroundSetting(const Background& b, const char* key, double fallback) {
    // Comma-separated key=value pairs, as Xournal++'s BackgroundConfig reads them.
    // strtod follows the C locale, which we never change.
    const std::string& c = b.config;
    const std::string k = key;
    for (size_t start = 0; start <= c.size();) {
        size_t end = c.find(',', start);
        if (end == std::string::npos) end = c.size();
        const size_t eq = c.rfind('=', end);
        if (eq != std::string::npos && eq >= start && c.compare(start, eq - start, k) == 0 && eq - start == k.size()) {
            const std::string value = c.substr(eq + 1, end - eq - 1);
            char* stop = nullptr;
            const double v = std::strtod(value.c_str(), &stop);
            if (stop != value.c_str() && std::isfinite(v)) return v;
        }
        start = end + 1;
    }
    return fallback;
}

std::optional<Paper> paperOf(const Background& b) {
    if (b.type != "solid") return std::nullopt;
    if (b.style == "plain") return Paper{PaperKind::Blank, -1, false};
    if (isRuled(b.style))
        return Paper{PaperKind::Ruled, presetIndex(kRuledSpacings, backgroundSetting(b, "r1", kDefaultRuledSpacing)),
                     b.style == "lined", backgroundSetting(b, "scrible-full", 0) == 0};
    if (b.style == "graph")
        return Paper{PaperKind::Grid, presetIndex(kGridSpacings, backgroundSetting(b, "r1", kDefaultGraphSpacing)),
                     false};
    return std::nullopt;
}

void setPaper(Background& b, const Paper& p) {
    const bool wasRuled = isRuled(b.style), wasGrid = b.style == "graph";
    double spacing = 0;
    if (p.kind == PaperKind::Ruled)
        spacing = p.spacing >= 0 ? kRuledSpacings[p.spacing]
                                 : backgroundSetting(b, "r1", wasRuled ? kDefaultRuledSpacing : kRuledSpacings[1]);
    if (p.kind == PaperKind::Grid)
        spacing = p.spacing >= 0 ? kGridSpacings[p.spacing]
                                 : backgroundSetting(b, "r1", wasGrid ? kDefaultGraphSpacing : kGridSpacings[1]);
    const double defaultSpacing = p.kind == PaperKind::Grid ? kDefaultGraphSpacing : kDefaultRuledSpacing;

    b.type = "solid";
    b.style = p.kind == PaperKind::Blank ? "plain" : p.kind == PaperKind::Grid ? "graph" : p.margin ? "lined" : "ruled";
    b.config.clear();
    if (p.kind != PaperKind::Blank && std::fabs(spacing - defaultSpacing) >= 0.01) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "r1=%g", std::round(spacing * 100) / 100);
        b.config = buf;
    }
    if (p.kind == PaperKind::Ruled && !p.ends) b.config += b.config.empty() ? "scrible-full=1" : ",scrible-full=1";
    b.xml.clear();  // written afresh from the fields
}

}  // namespace scrible
