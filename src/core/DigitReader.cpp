#include "DigitReader.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <numeric>
#include <sstream>

namespace scrible {
namespace {

#include "DigitSamples.inc"

constexpr int kNeighbours = 3;
constexpr double kOwnScale = 0.7;  // the user's own examples count as this much closer

int dist2(const DigitFeatures& a, const DigitFeatures& b) {
    int d = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        const int e = int(a[i]) - int(b[i]);
        d += e * e;
    }
    return d;
}

double width(const RectF& r) { return r.x1 - r.x0; }
double height(const RectF& r) { return r.y1 - r.y0; }
double centerX(const RectF& r) { return (r.x0 + r.x1) / 2; }
double centerY(const RectF& r) { return (r.y0 + r.y1) / 2; }

void unite(RectF& r, const RectF& o) {
    r.add(o.x0, o.y0);
    r.add(o.x1, o.y1);
}

}  // namespace

DigitFeatures digitFeatures(const std::vector<const Stroke*>& strokes) {
    std::vector<StrokePoint> path;
    for (const Stroke* s : strokes) path.insert(path.end(), s->points.begin(), s->points.end());
    DigitFeatures f{};
    if (path.empty()) return f;

    // 8 points evenly spaced along the path (pen-up jumps included, as in the samples).
    std::vector<double> along(path.size(), 0);
    for (size_t i = 1; i < path.size(); ++i)
        along[i] = along[i - 1] + std::hypot(path[i].x - path[i - 1].x, path[i].y - path[i - 1].y);
    const double total = along.back();
    StrokePoint pts[8];
    size_t seg = 0;
    for (int k = 0; k < 8; ++k) {
        const double target = total * k / 7;
        while (seg + 2 < path.size() && along[seg + 1] < target) ++seg;
        const size_t next = std::min(seg + 1, path.size() - 1);
        const double len = along[next] - along[seg];
        const double t = len > 0 ? std::clamp((target - along[seg]) / len, 0.0, 1.0) : 0;
        pts[k] = {path[seg].x + (path[next].x - path[seg].x) * t, path[seg].y + (path[next].y - path[seg].y) * t};
    }

    // Each axis scaled to 0..100 on its own; y up.
    double x0 = pts[0].x, x1 = x0, y0 = pts[0].y, y1 = y0;
    for (const StrokePoint& p : pts) {
        x0 = std::min(x0, p.x), x1 = std::max(x1, p.x);
        y0 = std::min(y0, p.y), y1 = std::max(y1, p.y);
    }
    auto scale = [](double v, double lo, double hi) {
        return static_cast<uint8_t>(hi > lo ? std::lround((v - lo) / (hi - lo) * 100) : 50);
    };
    for (int k = 0; k < 8; ++k) {
        f[2 * k] = scale(pts[k].x, x0, x1);
        f[2 * k + 1] = static_cast<uint8_t>(100 - scale(pts[k].y, y0, y1));
    }
    return f;
}

DigitReader::DigitReader(bool allSamples) {
    const size_t n = allSamples ? std::size(kDigitSamples) : kDigitTrainCount;
    samples_.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        Sample s;
        std::copy_n(kDigitSamples[i], 16, s.f.begin());
        s.digit = kDigitSamples[i][16];
        samples_.push_back(s);
    }
}

int DigitReader::classify(const DigitFeatures& f) const {
    // The nearest few samples vote, the nearer the louder. One of the user's own
    // examples decides instead when it's nearer still: it corrects the shapes it's
    // like, and no others.
    struct Hit {
        double d = std::numeric_limits<double>::infinity();
        int digit = -1;
    };
    std::array<Hit, kNeighbours> best;
    for (const Sample& s : samples_) {
        const double d = std::sqrt(double(dist2(f, s.f)));
        if (d >= best.back().d) continue;
        best.back() = {d, s.digit};
        std::sort(best.begin(), best.end(), [](const Hit& a, const Hit& b) { return a.d < b.d; });
    }
    Hit own;
    for (const Sample& s : examples_) {
        const double d = std::sqrt(double(dist2(f, s.f))) * kOwnScale;
        if (d < own.d) own = {d, s.digit};
    }
    if (own.d < best.front().d) return own.digit;
    double votes[10] = {};
    for (const Hit& h : best)
        if (h.digit >= 0) votes[h.digit] += 1 / (1 + h.d);
    return static_cast<int>(std::max_element(std::begin(votes), std::end(votes)) - std::begin(votes));
}

void DigitReader::addExample(const DigitFeatures& f, int digit) {
    if (digit >= 0 && digit <= 9) examples_.push_back({f, static_cast<uint8_t>(digit)});
}

bool DigitReader::loadExamples(const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        int digit = -1;
        DigitFeatures f{};
        ls >> digit;
        bool ok = digit >= 0 && digit <= 9;
        for (uint8_t& v : f) {
            int x = -1;
            ls >> x;
            ok = ok && x >= 0 && x <= 100;
            v = static_cast<uint8_t>(x);
        }
        if (ok && ls) addExample(f, digit);
    }
    return true;
}

bool DigitReader::appendExample(const std::string& path, const DigitFeatures& f, int digit) const {
    std::ofstream out(path, std::ios::app);
    out << digit;
    for (uint8_t v : f) out << ' ' << int(v);
    out << '\n';
    return bool(out);
}

size_t DigitReader::testCount() { return std::size(kDigitSamples) - kDigitTrainCount; }

DigitFeatures DigitReader::testSample(size_t i, int& digit) {
    const uint8_t* s = kDigitSamples[kDigitTrainCount + i];
    DigitFeatures f;
    std::copy_n(s, 16, f.begin());
    digit = s[16];
    return f;
}

std::string ReadNumber::text() const {
    std::string t;
    for (const ReadChar& c : chars) t += c.c;
    return t;
}

bool ReadNumber::parse(double& value) const {
    const std::string t = text();
    size_t i = 0;
    const bool negative = i < t.size() && t[i] == '-';
    if (negative) ++i;
    std::string number;
    int sinceComma = -1;  // digits since the last comma; -1 before the first
    for (; i < t.size() && t[i] != '.'; ++i) {
        if (std::isdigit(static_cast<unsigned char>(t[i]))) {
            number += t[i];
            if (sinceComma >= 0) ++sinceComma;
        } else if (t[i] == ',') {
            // Thousands: 1 to 3 digits first, then groups of exactly three.
            if (number.empty() || (sinceComma < 0 && number.size() > 3) || (sinceComma >= 0 && sinceComma != 3))
                return false;
            sinceComma = 0;
        } else {
            return false;
        }
    }
    if (number.empty() || (sinceComma >= 0 && sinceComma != 3)) return false;
    if (i < t.size()) {  // the decimal point
        number += '.';
        const size_t digits = number.size();
        for (++i; i < t.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(t[i]))) return false;
            number += t[i];
        }
        if (number.size() == digits) return false;
    }
    value = std::strtod(number.c_str(), nullptr);
    if (negative) value = -value;
    return true;
}

std::string typedNumber(const std::string& line) {
    size_t i = line.find_first_not_of(" \t");
    if (i == std::string::npos) return {};
    std::string sign;
    if (line[i] == '-' || line[i] == '+') sign = line[i++] == '-' ? "-" : "";
    // The longest run of digits, commas and one decimal point, without a comma or point at its end.
    std::string run;
    for (bool point = false; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '.' && point) break;
        if (!std::isdigit(static_cast<unsigned char>(c)) && c != ',' && c != '.') break;
        point = point || c == '.';
        run += c;
    }
    while (!run.empty() && (run.back() == ',' || run.back() == '.')) run.pop_back();
    if (!run.empty() && run[0] == '.') run = "0" + run;
    if (run.empty() || run[0] == ',') return {};
    ReadNumber n;
    for (char c : sign + run) n.chars.push_back({c, {}, {}, {}});
    double v;
    if (n.parse(v)) return sign + run;
    // Commas that aren't thousands: the number ends at the first, as for strtod.
    run.resize(run.find(','));
    return sign + run;
}

std::vector<ReadNumber> readNumbers(const std::vector<Stroke>& strokes, const DigitReader& reader) {
    const size_t n = strokes.size();
    if (n == 0) return {};
    std::vector<RectF> box(n);
    for (size_t i = 0; i < n; ++i) box[i] = strokes[i].bounds();

    // The size of the writing: the height of the taller strokes (digits, not dots or dashes).
    std::vector<double> heights(n);
    for (size_t i = 0; i < n; ++i) heights[i] = height(box[i]);
    std::sort(heights.begin(), heights.end());
    const double size = std::max(1.0, heights[heights.size() * 3 / 4]);
    auto big = [&](size_t i) { return height(box[i]) >= 0.45 * size; };

    // Lines of writing, which needn't be level (blank paper has no rules to follow):
    // tall strokes side by side that overlap vertically are linked, a pair at a time,
    // so a line can drift up or down as long as each digit lines up with the next.
    std::vector<size_t> parent(n);
    std::iota(parent.begin(), parent.end(), 0);
    std::function<size_t(size_t)> root = [&](size_t i) { return parent[i] == i ? i : parent[i] = root(parent[i]); };
    for (size_t i = 0; i < n; ++i) {
        if (!big(i)) continue;
        for (size_t j = i + 1; j < n; ++j) {
            if (!big(j)) continue;
            const double gap = std::max(box[j].x0 - box[i].x1, box[i].x0 - box[j].x1);
            const double overlap = std::min(box[i].y1, box[j].y1) - std::max(box[i].y0, box[j].y0);
            if (gap < 1.5 * size && overlap >= 0.5 * std::min(height(box[i]), height(box[j])))
                parent[root(i)] = root(j);
        }
    }
    // Each line's slant, and where its digits start and end along it (the top and the
    // baseline), from its full-height strokes, and its own size of writing.
    struct Line {
        std::vector<size_t> strokes;
        double h = 1, slope = 0, top0 = 0, base0 = 0, x0 = 0, x1 = 0;
        double topAt(double x) const { return top0 + slope * x; }
        double baseAt(double x) const { return base0 + slope * x; }
    };
    std::vector<Line> lines;
    std::vector<bool> inLine(n, false);
    {
        // A lone stroke that isn't full height (a long comma) is no line: it goes
        // with the small strokes.
        std::vector<int> members(n, 0);
        for (size_t i = 0; i < n; ++i)
            if (big(i)) ++members[root(i)];
        std::vector<int> index(n, -1);
        for (size_t i = 0; i < n; ++i) {
            if (!big(i) || (members[root(i)] == 1 && height(box[i]) < 0.8 * size)) continue;
            inLine[i] = true;
            const size_t r = root(i);
            if (index[r] < 0) {
                index[r] = static_cast<int>(lines.size());
                lines.emplace_back();
            }
            lines[static_cast<size_t>(index[r])].strokes.push_back(i);
        }
    }
    auto median = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    // A short line's slant is guesswork: it takes the page's (the middle of the
    // longer lines'), since a page's writing tends to drift the same way.
    std::vector<std::vector<size_t>> talls(lines.size());
    std::vector<bool> sure(lines.size(), false);
    std::vector<double> sureSlopes;
    for (size_t li = 0; li < lines.size(); ++li) {
        Line& l = lines[li];
        std::vector<double> hs;
        for (size_t i : l.strokes) hs.push_back(height(box[i]));
        std::sort(hs.begin(), hs.end());
        l.h = std::max(1.0, hs[hs.size() * 3 / 4]);  // the taller strokes: not small 0s or commas
        std::vector<size_t>& tall = talls[li];  // not commas, tails or small 0s
        for (size_t i : l.strokes)
            if (height(box[i]) >= 0.75 * l.h) tall.push_back(i);
        if (tall.empty()) tall = l.strokes;
        l.x0 = box[l.strokes[0]].x0;
        l.x1 = box[l.strokes[0]].x1;
        for (size_t i : l.strokes) l.x0 = std::min(l.x0, box[i].x0), l.x1 = std::max(l.x1, box[i].x1);
        // The slant along the bottoms of the tall strokes, which digits share: the
        // middle of the slopes between pairs of them, so a stray stroke doesn't tip it.
        std::vector<double> slopes;
        for (size_t a = 0; a < tall.size(); ++a)
            for (size_t b = a + 1; b < tall.size(); ++b) {
                const double dx = centerX(box[tall[b]]) - centerX(box[tall[a]]);
                if (std::fabs(dx) >= 0.5 * l.h) slopes.push_back((box[tall[b]].y1 - box[tall[a]].y1) / dx);
            }
        if (!slopes.empty()) l.slope = std::clamp(median(slopes), -0.35, 0.35);
        sure[li] = tall.size() >= 5 && l.x1 - l.x0 >= 4 * l.h;
        if (sure[li]) sureSlopes.push_back(l.slope);
    }
    const double pageSlope = sureSlopes.empty() ? 0 : median(sureSlopes);
    for (size_t li = 0; li < lines.size(); ++li) {
        Line& l = lines[li];
        if (!sure[li]) l.slope = pageSlope;
        std::vector<double> tops, bases;
        for (size_t i : talls[li]) {
            tops.push_back(box[i].y0 - l.slope * centerX(box[i]));
            bases.push_back(box[i].y1 - l.slope * centerX(box[i]));
        }
        l.top0 = median(tops);
        l.base0 = median(bases);
    }
    // Small strokes go to the line they're nearest to, measured along its slant.
    for (size_t i = 0; i < n; ++i) {
        if (inLine[i]) continue;
        if (lines.empty()) {
            lines.emplace_back();
            lines[0].h = size;
            lines[0].top0 = box[i].y0 - size / 2;
            lines[0].base0 = box[i].y0 + size / 2;
            lines[0].x0 = box[i].x0;
            lines[0].x1 = box[i].x1;
        }
        Line* nearest = &lines[0];
        double best = std::numeric_limits<double>::infinity();
        for (Line& l : lines) {
            const double cx = centerX(box[i]), cy = centerY(box[i]);
            const double t = l.topAt(cx), b = l.baseAt(cx);
            double d = cy < t ? t - cy : cy > b ? cy - b : 0;
            d += std::max({0.0, l.x0 - cx, cx - l.x1});  // beside the line rather than in it
            if (d < best) best = d, nearest = &l;
        }
        nearest->strokes.push_back(i);
    }
    // Reading order: top to bottom, and left to right along a row, where pieces of a
    // row (numbers far apart) line up with each other along their slant.
    auto middleY = [](const Line& l) { return l.topAt((l.x0 + l.x1) / 2); };
    std::sort(lines.begin(), lines.end(), [&](const Line& a, const Line& b) { return middleY(a) < middleY(b); });
    std::vector<Line> ordered;
    std::vector<bool> placed(lines.size(), false);
    for (size_t i = 0; i < lines.size(); ++i) {
        if (placed[i]) continue;
        std::vector<size_t> row = {i};
        placed[i] = true;
        for (size_t j = i + 1; j < lines.size(); ++j) {
            const double x = (lines[j].x0 + lines[j].x1) / 2;
            if (!placed[j] && std::fabs(lines[i].topAt(x) - lines[j].topAt(x)) < 0.6 * lines[i].h) {
                row.push_back(j);
                placed[j] = true;
            }
        }
        std::sort(row.begin(), row.end(), [&](size_t a, size_t b) { return lines[a].x0 < lines[b].x0; });
        for (size_t k : row) ordered.push_back(std::move(lines[k]));
    }
    lines = std::move(ordered);

    std::vector<ReadNumber> out;
    std::vector<Stroke> level(n);  // each line's strokes, turned so that the line is level
    std::vector<RectF> lbox(n);
    for (const Line& line : lines) {
        const double h = line.h;
        // Read the line level: its strokes turned by its slant, so that everything
        // below works along the line (and digits stand upright, like the samples).
        const double c = std::cos(std::atan(line.slope)), sn = std::sin(std::atan(line.slope));
        std::vector<double> tops, bases;
        for (size_t i : line.strokes) {
            level[i] = strokes[i];
            for (StrokePoint& p : level[i].points) p = {p.x * c + p.y * sn, -p.x * sn + p.y * c};
            level[i].invalidateBounds();
            lbox[i] = level[i].bounds();
            if (height(lbox[i]) >= 0.75 * h) {
                tops.push_back(lbox[i].y0);
                bases.push_back(lbox[i].y1);
            }
        }
        const double lineTop = tops.empty() ? line.top0 : median(tops);
        const double baseline = bases.empty() ? line.base0 : median(bases);
        const std::vector<RectF>& box = lbox;  // from here on, in the line's own frame

        // Where each stroke starts and ends, in digit heights below the line's top:
        // digits start near the top; commas and decimal points start low; minus signs
        // and the bars of 5s and 7s are flat.
        auto top = [&](size_t i) { return (box[i].y0 - lineTop) / h; };
        auto bottom = [&](size_t i) { return (box[i].y1 - lineTop) / h; };
        auto flat = [&](size_t i) { return width(box[i]) >= 1.5 * height(box[i]) && height(box[i]) < 0.3 * h; };
        // A loop goes a long way round for its size (a small 0); a comma doesn't.
        auto loop = [&](size_t i) {
            double length = 0;
            const auto& pts = level[i].points;
            for (size_t k = 1; k < pts.size(); ++k) length += std::hypot(pts[k].x - pts[k - 1].x, pts[k].y - pts[k - 1].y);
            return length > 2.2 * std::max(width(box[i]), height(box[i]));
        };
        auto body = [&](size_t i) {
            if (height(box[i]) < 0.3 * h || flat(i)) return false;
            return top(i) < 0.33 || loop(i);
        };

        // Characters: digit strokes that overlap side to side (a 4 in two strokes),
        // sorted left to right.
        std::vector<ReadChar> chars;
        std::vector<size_t> bodies, others;
        for (size_t i : line.strokes) (body(i) ? bodies : others).push_back(i);
        std::sort(bodies.begin(), bodies.end(), [&](size_t a, size_t b) { return box[a].x0 < box[b].x0; });
        for (size_t i : bodies) {
            ReadChar* c = chars.empty() ? nullptr : &chars.back();
            if (c) {
                const RectF& b = box[i];
                const double overlap = std::min(c->bounds.x1, b.x1) - std::max(c->bounds.x0, b.x0);
                const bool together = (centerX(b) >= c->bounds.x0 && centerX(b) <= c->bounds.x1) ||
                                      (centerX(c->bounds) >= b.x0 && centerX(c->bounds) <= b.x1) ||
                                      overlap >= 0.5 * std::min(width(b), width(c->bounds));
                if (!together) c = nullptr;
            }
            if (!c) {
                chars.push_back({});
                c = &chars.back();
            }
            c->strokes.push_back(i);
            unite(c->bounds, box[i]);
        }
        const size_t digitChars = chars.size();
        for (size_t i : others) {
            const RectF& b = box[i];
            // A flat stroke across a digit is part of it (a 5's flag, a 7's bar or crossbar),
            // and so is a taller one that's mostly over a digit and ends at the baseline
            // (a 9's tail); a comma hangs below it.
            ReadChar* across = nullptr;
            for (size_t k = 0; k < digitChars; ++k) {
                const RectF& c = chars[k].bounds;
                const double overlap = std::min(c.x1, b.x1) - std::max(c.x0, b.x0);
                if (flat(i) ? overlap >= 0.25 * width(b)
                            : height(b) >= 0.3 * h && overlap >= 0.5 * width(b) && b.y1 <= baseline + 0.15 * h)
                    across = &chars[k];
            }
            if (across) {
                across->strokes.push_back(i);
                unite(across->bounds, b);
                continue;
            }
            ReadChar m;
            m.strokes = {i};
            m.bounds = b;
            const double mid = (top(i) + bottom(i)) / 2;
            if (flat(i) && width(b) >= 0.25 * h && mid > 0.2 && mid < 0.8) m.c = '-';
            else if (top(i) >= 0.33 && height(b) >= 0.3 * h) m.c = ',';
            else if (top(i) >= 0.33) m.c = '.';
            chars.push_back(m);
        }
        std::sort(chars.begin(), chars.end(),
                  [](const ReadChar& a, const ReadChar& b) { return centerX(a.bounds) < centerX(b.bounds); });

        // Digits, from the strokes in the order they were written. (Every character
        // gets its features: a mark corrected to a digit teaches the reader too.)
        for (ReadChar& c : chars) {
            std::sort(c.strokes.begin(), c.strokes.end());
            std::vector<const Stroke*> ss;
            for (size_t i : c.strokes) ss.push_back(&level[i]);
            c.features = digitFeatures(ss);
            if (c.c != '?' || height(c.bounds) < 0.3 * h) continue;
            // A plain upright stroke is a 1. (Scaled to fill the box, as the samples are,
            // its little wobbles from side to side would look like anything.)
            if (width(c.bounds) < 0.2 * height(c.bounds)) c.c = '1';
            else c.c = static_cast<char>('0' + reader.classify(c.features));
        }

        // Numbers: split where there's a wide gap. Bounds are given back on the page.
        ReadNumber num;
        double lastX1 = 0;
        for (size_t k = 0; k < chars.size(); ++k) {
            ReadChar& ch = chars[k];
            if (k > 0 && ch.bounds.x0 - lastX1 > 0.9 * h) {
                out.push_back(std::move(num));
                num = {};
            }
            lastX1 = ch.bounds.x1;
            ch.bounds = {};
            for (size_t i : ch.strokes) unite(ch.bounds, strokes[i].bounds());
            num.chars.push_back(std::move(ch));
        }
        if (!num.chars.empty()) out.push_back(std::move(num));
    }
    return out;
}

}  // namespace scrible
