// Kindle front end: e-ink output through FBInk, pen and touch from evdev.
// Meant to be started by kindle/run.sh (from a library scriptlet), which gets
// the Kindle's own UI out of the way first and restores it afterwards.
//
//   xscribe [notebook.xopp] [options]
//     --pen DEV, --touch DEV     input devices (default: auto-detect)
//     --pen-transform SPEC       axis fix-ups, e.g. "swap,flipx" (swap is applied first)
//     --touch-transform SPEC     (default: FBInk's quirks for this device)
//     --no-grab                  don't take exclusive access to the input devices
//     --no-pen-mode              plain DU instead of the MediaTek pen mode
//     --probe [SECONDS]          print display/input info, echo input events, exit

#include <poll.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "Editor.h"
#include "EvdevInput.h"
#include "FbinkDisplay.h"
#include "PowerEvents.h"
#include "XoppFile.h"

namespace {

volatile sig_atomic_t g_quit = 0;
void onSignal(int) { g_quit = 1; }

uint64_t nowMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000 + static_cast<uint64_t>(ts.tv_nsec) / 1000000;
}

struct Args {
    std::string path = "/mnt/us/xournal-scribe/notebook.xopp";
    std::string penDev, touchDev;
    std::string penTransform, touchTransform;
    bool grab = true;
    bool penMode = true;
    int probeSeconds = -1;
};

bool parseArgs(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](std::string& out) {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };
        if (arg == "--pen") { if (!value(a.penDev)) return false; }
        else if (arg == "--touch") { if (!value(a.touchDev)) return false; }
        else if (arg == "--pen-transform") { if (!value(a.penTransform)) return false; }
        else if (arg == "--touch-transform") { if (!value(a.touchTransform)) return false; }
        else if (arg == "--no-grab") a.grab = false;
        else if (arg == "--no-pen-mode") a.penMode = false;
        else if (arg == "--probe") {
            a.probeSeconds = 30;
            if (i + 1 < argc && argv[i + 1][0] != '-') a.probeSeconds = std::atoi(argv[++i]);
        } else if (arg[0] == '-') return false;
        else a.path = arg;
    }
    return true;
}

// Pen pressure seen in a session, logged at exit to help tune the pressure curve.
struct PressureStats {
    unsigned buckets[11] = {};  // 0.0, 0.1, ... 1.0
    unsigned total = 0;

    void add(double p) {
        ++buckets[std::clamp(static_cast<int>(p * 10 + 0.5), 0, 10)];
        ++total;
    }
    void log(const xs::PenInput& pen) const {
        std::fprintf(stderr, "xscribe: pressure from %s, %u samples:", pen.pressureOrigin(), total);
        for (int i = 0; i <= 10 && total; ++i)
            if (buckets[i]) std::fprintf(stderr, " %.1f=%u%%", i / 10.0, (buckets[i] * 100 + total / 2) / total);
        std::fprintf(stderr, "\n");
    }
};

// Opens the pen, plus a separate pressure source if the device has one.
bool openPen(xs::PenInput& pen, const std::vector<xs::InputDeviceInfo>& devices, const std::string& penPath,
             int w, int h, const xs::InputTransform& t, bool grab, std::string& err) {
    if (penPath.empty() || !pen.open(penPath, w, h, t, grab, err)) return false;
    if (const auto* src = xs::pickPressureSource(devices, penPath)) {
        std::string srcErr;
        if (pen.openPressureSource(src->path, srcErr))
            std::fprintf(stderr, "xscribe: pressure source %s (\"%s\")\n", src->path.c_str(), src->name.c_str());
        else
            std::fprintf(stderr, "xscribe: can't use pressure source: %s\n", srcErr.c_str());
    }
    return true;
}

// Full-screen message for errors the user needs to see (there's no terminal on the device).
void showMessage(xs::FbinkDisplay& display, const std::vector<std::string>& lines) {
    xs::Canvas c(display.width(), display.height());
    int y = display.height() / 3;
    for (const auto& line : lines) {
        c.drawText(60, y, line, 4, 0, c.rect());
        y += 64;
    }
    display.present(c, {{c.rect(), xs::Refresh::Full}});
}

int probe(xs::FbinkDisplay& display, const Args& args, const xs::InputTransform& penT, const xs::InputTransform& touchT) {
    std::printf("display: %s\n", display.describe().c_str());
    const auto devices = xs::scanInputDevices();
    for (const auto& d : devices) {
        std::printf("%s  %-24s %s", d.path.c_str(), ("\"" + d.name + "\"").c_str(),
                    d.isPen ? "pen" : d.isTouch ? "touch" : "-");
        if (d.isPen || d.isTouch) std::printf("  x %d..%d  y %d..%d", d.x.min, d.x.max, d.y.min, d.y.max);
        if (d.isPen) std::printf("  pressure %d..%d", d.pressure.min, d.pressure.max);
        std::printf("\n");
    }
    const auto* penInfo = xs::pickPen(devices);
    const auto* touchInfo = xs::pickTouch(devices);
    const std::string penPath = !args.penDev.empty() ? args.penDev : penInfo ? penInfo->path : "";
    const std::string touchPath = !args.touchDev.empty() ? args.touchDev : touchInfo ? touchInfo->path : "";
    std::printf("using pen %s, touch %s\n", penPath.empty() ? "(none)" : penPath.c_str(),
                touchPath.empty() ? "(none)" : touchPath.c_str());
    std::printf("echoing mapped input for %d s; screen is %dx%d. Touch each corner to check orientation.\n",
                args.probeSeconds, display.width(), display.height());
    std::fflush(stdout);

    xs::PenInput pen;
    xs::TouchInput touch;
    std::string err;
    std::fflush(stdout);
    if (!openPen(pen, devices, penPath, display.width(), display.height(), penT, args.grab, err))
        std::printf("pen: %s\n", err.c_str());
    if (!touchPath.empty() && !touch.open(touchPath, display.width(), display.height(), touchT, args.grab, err))
        std::printf("touch: %s\n", err.c_str());

    // Draw where we think the pen/finger is: if the marks don't appear under the
    // tip, the axes need a --pen-transform / --touch-transform.
    xs::Canvas c(display.width(), display.height());
    int y = 200;
    for (const char* line : {"INPUT TEST", "", "Draw near each corner with the pen,", "then tap each corner with a finger.",
                             "Marks should appear under the tip.", "", "Ends by itself; log: xscribe.log"}) {
        c.drawText(60, y, line, 4, 0, c.rect());
        y += 64;
    }
    display.present(c, {{c.rect(), xs::Refresh::Full}});

    const char* kinds[] = {"down", "move", "up"};
    PressureStats stats;
    const uint64_t end = nowMs() + static_cast<uint64_t>(args.probeSeconds) * 1000;
    while (!g_quit && nowMs() < end) {
        pollfd fds[3] = {{pen.fd(), POLLIN, 0}, {touch.fd(), POLLIN, 0}, {pen.pressureFd(), POLLIN, 0}};
        if (poll(fds, 3, 250) <= 0) continue;
        std::vector<xs::PenEvent> pe;
        std::vector<xs::TouchEvent> te;
        pen.readPressure();
        if (pen.fd() >= 0) pen.read(pe);
        if (touch.fd() >= 0) touch.read(te);
        std::vector<xs::Damage> marks;
        for (const auto& e : pe) {
            std::printf("pen   %-4s %7.1f %7.1f  pressure %.2f%s\n", kinds[static_cast<int>(e.type)], e.x, e.y,
                        e.pressure, e.eraser ? "  eraser" : "");
            if (e.type != xs::PenEvent::Type::Up) stats.add(e.pressure);
            const int r = 2 + static_cast<int>(e.pressure * 6);
            const xs::IRect m{int(e.x) - r, int(e.y) - r, int(e.x) + r + 1, int(e.y) + r + 1};
            c.fill(m, 0);
            marks.push_back({m, xs::Refresh::Ink});
        }
        for (const auto& e : te) {
            std::printf("touch %-4s %7.1f %7.1f\n", kinds[static_cast<int>(e.type)], e.x, e.y);
            if (e.type != xs::TouchEvent::Type::Down) continue;
            const xs::IRect m{int(e.x) - 30, int(e.y) - 30, int(e.x) + 30, int(e.y) + 30};
            c.strokeRect(m, 6, 0);
            marks.push_back({m, xs::Refresh::UI});
        }
        display.present(c, marks);
        std::fflush(stdout);
    }
    stats.log(pen);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parseArgs(argc, argv, args)) {
        std::fprintf(stderr,
                     "usage: %s [notebook.xopp] [--pen DEV] [--touch DEV] [--pen-transform SPEC]\n"
                     "          [--touch-transform SPEC] [--no-grab] [--no-pen-mode] [--probe [SECONDS]]\n",
                     argv[0]);
        return 2;
    }

    struct sigaction sa {};
    sa.sa_handler = onSignal;  // no SA_RESTART: poll() returns EINTR so we can exit cleanly
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);

    // Before opening anything else, so the child doesn't inherit our descriptors.
    xs::PowerEvents power;
    if (args.probeSeconds < 0 && !power.start()) std::fprintf(stderr, "xscribe: no sleep/wake notifications\n");

    xs::FbinkDisplay display;
    xs::DisplayOptions dopts;
    dopts.penMode = args.penMode;
    if (!display.open(dopts)) return 1;
    std::fprintf(stderr, "xscribe: %s\n", display.describe().c_str());

    xs::InputTransform penT, touchT;
    const xs::TouchQuirks q = display.touchQuirks();
    touchT.swapAxes = q.swapAxes;
    touchT.mirrorX = q.mirrorX;
    touchT.mirrorY = q.mirrorY;
    if ((!args.penTransform.empty() && !xs::InputTransform::parse(args.penTransform, penT)) ||
        (!args.touchTransform.empty() && !xs::InputTransform::parse(args.touchTransform, touchT))) {
        std::fprintf(stderr, "xscribe: transforms are comma-separated: swap, flipx, flipy\n");
        return 2;
    }

    if (args.probeSeconds >= 0) return probe(display, args, penT, touchT);

    // Load the notebook, or start a new one if the file doesn't exist yet.
    xs::Document doc;
    if (access(args.path.c_str(), F_OK) == 0) {
        std::string error;
        std::vector<std::string> warnings;
        if (!xs::loadXopp(args.path, doc, error, &warnings)) {
            std::fprintf(stderr, "xscribe: %s: %s\n", args.path.c_str(), error.c_str());
            showMessage(display, {"Can't open notebook:", args.path, error, "", "The file was not changed."});
            sleep(6);
            return 1;
        }
        for (const auto& w : warnings) std::fprintf(stderr, "xscribe: %s: %s\n", args.path.c_str(), w.c_str());
    }

    const auto devices = xs::scanInputDevices();
    const auto* penInfo = xs::pickPen(devices);
    const auto* touchInfo = xs::pickTouch(devices);
    const std::string penPath = !args.penDev.empty() ? args.penDev : penInfo ? penInfo->path : "";
    const std::string touchPath = !args.touchDev.empty() ? args.touchDev : touchInfo ? touchInfo->path : "";

    xs::PenInput pen;
    xs::TouchInput touch;
    std::string err;
    if (!openPen(pen, devices, penPath, display.width(), display.height(), penT, args.grab, err)) {
        std::fprintf(stderr, "xscribe: no usable pen device %s\n", err.c_str());
        showMessage(display, {"No pen input device found.", "Run xscribe --probe over SSH for details."});
        sleep(6);
        return 1;
    }
    if (touchPath.empty() || !touch.open(touchPath, display.width(), display.height(), touchT, args.grab, err))
        std::fprintf(stderr, "xscribe: no touch input (%s); the toolbar still works with the pen\n", err.c_str());
    std::fprintf(stderr, "xscribe: pen %s, touch %s, notebook %s\n", penPath.c_str(),
                 touch.fd() >= 0 ? touchPath.c_str() : "(none)", args.path.c_str());

    xs::EditorOptions eopts;
    eopts.dpi = display.dpi();
    eopts.render.antialias = false;  // what DU shows while writing is then exactly the final stroke
    xs::Editor editor(display.width(), display.height(), std::move(doc), args.path, eopts);
    display.present(editor.canvas(), editor.takeDamage());

    std::vector<xs::PenEvent> penEvents;
    std::vector<xs::TouchEvent> touchEvents;
    PressureStats stats;
    while (!g_quit && !editor.quitRequested()) {
        pollfd fds[4] = {{pen.fd(), POLLIN, 0}, {touch.fd(), POLLIN, 0}, {power.fd(), POLLIN, 0},
                         {pen.pressureFd(), POLLIN, 0}};
        if (poll(fds, 4, editor.msUntilIdleWork(nowMs())) < 0 && errno != EINTR) {
            std::perror("xscribe: poll");
            break;
        }
        const uint64_t now = nowMs();

        if (fds[3].revents & POLLIN) pen.readPressure();
        penEvents.clear();
        if (fds[0].revents & POLLIN) pen.read(penEvents);
        for (const auto& e : penEvents) {
            if (e.type != xs::PenEvent::Type::Up) stats.add(e.pressure);
            const xs::PenSample s{e.x, e.y, e.pressure, e.eraser};
            switch (e.type) {
                case xs::PenEvent::Type::Down: editor.penDown(s, now); break;
                case xs::PenEvent::Type::Move: editor.penMove(s, now); break;
                case xs::PenEvent::Type::Up: editor.penUp(now); break;
            }
        }

        touchEvents.clear();
        if (fds[1].revents & POLLIN) touch.read(touchEvents);
        for (const auto& e : touchEvents)
            if (e.type == xs::TouchEvent::Type::Down)
                editor.touchDown(static_cast<int>(e.x), static_cast<int>(e.y), now);

        if (fds[2].revents & (POLLIN | POLLHUP)) {
            for (const auto ev : power.read()) {
                if (ev == xs::PowerEvents::Event::GoingToSleep && editor.modified()) editor.save(now, true);
                if (ev == xs::PowerEvents::Event::WokeUp) editor.redrawAll(xs::Refresh::Full);
            }
        }

        editor.onIdle(now);
        display.present(editor.canvas(), editor.takeDamage());
    }

    stats.log(pen);
    if (editor.modified() && !editor.save(nowMs(), true)) return 1;
    return 0;
}
