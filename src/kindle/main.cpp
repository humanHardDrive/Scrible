// Kindle front end: e-ink output through FBInk, pen and touch from evdev.
// Meant to be started by kindle/run.sh (from a library scriptlet), which gets
// the Kindle's own UI out of the way first and restores it afterwards.
//
//   scrible [NOTEBOOKS-FOLDER | notebook.xopp] [options]
//     A folder (default /mnt/us/scrible/notebooks) opens the notebook list;
//     a .xopp/.xoj file opens just that notebook.
//     --pen DEV, --touch DEV     input devices (default: auto-detect)
//     --pen-transform SPEC       axis fix-ups, e.g. "swap,flipx" (swap is applied first)
//     --touch-transform SPEC     (default: FBInk's quirks for this device)
//     --no-grab                  don't take exclusive access to the input devices
//     --no-pen-mode              plain DU instead of the MediaTek pen mode
//     --no-a2                    zoom/pan previews with DU only (if A2 misbehaves)
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

#include "App.h"
#include "EvdevInput.h"
#include "FbinkDisplay.h"
#include "PowerEvents.h"

namespace {

volatile sig_atomic_t g_quit = 0;
void onSignal(int) { g_quit = 1; }

uint64_t nowMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000 + static_cast<uint64_t>(ts.tv_nsec) / 1000000;
}

struct Args {
    std::string path = "/mnt/us/scrible/notebooks";
    std::string penDev, touchDev;
    std::string penTransform, touchTransform;
    bool grab = true;
    bool penMode = true;
    bool a2 = true;
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
        else if (arg == "--no-a2") a.a2 = false;
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
    void log(const scrible::PenInput& pen) const {
        std::fprintf(stderr, "scrible: pressure from %s, %u samples:", pen.pressureOrigin(), total);
        for (int i = 0; i <= 10 && total; ++i)
            if (buckets[i]) std::fprintf(stderr, " %.1f=%u%%", i / 10.0, (buckets[i] * 100 + total / 2) / total);
        std::fprintf(stderr, "\n");
    }
};

// Opens the pen, plus a separate pressure source if the device has one.
bool openPen(scrible::PenInput& pen, const std::vector<scrible::InputDeviceInfo>& devices, const std::string& penPath,
             int w, int h, const scrible::InputTransform& t, bool grab, std::string& err) {
    if (penPath.empty() || !pen.open(penPath, w, h, t, grab, err)) return false;
    if (const auto* src = scrible::pickPressureSource(devices, penPath)) {
        std::string srcErr;
        if (pen.openPressureSource(src->path, srcErr))
            std::fprintf(stderr, "scrible: pressure source %s (\"%s\")\n", src->path.c_str(), src->name.c_str());
        else
            std::fprintf(stderr, "scrible: can't use pressure source: %s\n", srcErr.c_str());
    }
    return true;
}

// Full-screen message for errors the user needs to see (there's no terminal on the device).
void showMessage(scrible::FbinkDisplay& display, const std::vector<std::string>& lines) {
    scrible::Canvas c(display.width(), display.height());
    int y = display.height() / 3;
    for (const auto& line : lines) {
        c.drawText(60, y, line, 4, 0, c.rect());
        y += 64;
    }
    display.present(c, {{c.rect(), scrible::Refresh::Full}});
}

int probe(scrible::FbinkDisplay& display, const Args& args, const scrible::InputTransform& penT, const scrible::InputTransform& touchT) {
    std::printf("display: %s\n", display.describe().c_str());
    const auto devices = scrible::scanInputDevices();
    for (const auto& d : devices) {
        std::printf("%s  %-24s %s", d.path.c_str(), ("\"" + d.name + "\"").c_str(),
                    d.isPen ? "pen" : d.isTouch ? "touch" : "-");
        if (d.isPen || d.isTouch) std::printf("  x %d..%d  y %d..%d", d.x.min, d.x.max, d.y.min, d.y.max);
        if (d.isPen) std::printf("  pressure %d..%d", d.pressure.min, d.pressure.max);
        std::printf("\n");
    }
    const auto* penInfo = scrible::pickPen(devices);
    const auto* touchInfo = scrible::pickTouch(devices);
    const std::string penPath = !args.penDev.empty() ? args.penDev : penInfo ? penInfo->path : "";
    const std::string touchPath = !args.touchDev.empty() ? args.touchDev : touchInfo ? touchInfo->path : "";
    std::printf("using pen %s, touch %s\n", penPath.empty() ? "(none)" : penPath.c_str(),
                touchPath.empty() ? "(none)" : touchPath.c_str());
    std::printf("echoing mapped input for %d s; screen is %dx%d. Touch each corner to check orientation.\n",
                args.probeSeconds, display.width(), display.height());
    std::fflush(stdout);

    scrible::PenInput pen;
    scrible::TouchInput touch;
    std::string err;
    std::fflush(stdout);
    if (!openPen(pen, devices, penPath, display.width(), display.height(), penT, args.grab, err))
        std::printf("pen: %s\n", err.c_str());
    if (!touchPath.empty() && !touch.open(touchPath, display.width(), display.height(), touchT, args.grab, err))
        std::printf("touch: %s\n", err.c_str());

    // Draw where we think the pen/finger is: if the marks don't appear under the
    // tip, the axes need a --pen-transform / --touch-transform.
    scrible::Canvas c(display.width(), display.height());
    int y = 200;
    for (const char* line : {"INPUT TEST", "", "Draw near each corner with the pen,", "then tap each corner with a finger.",
                             "Marks should appear under the tip.", "", "Ends by itself; log: scrible.log"}) {
        c.drawText(60, y, line, 4, 0, c.rect());
        y += 64;
    }
    display.present(c, {{c.rect(), scrible::Refresh::Full}});

    const char* kinds[] = {"down", "move", "up"};
    PressureStats stats;
    const uint64_t end = nowMs() + static_cast<uint64_t>(args.probeSeconds) * 1000;
    while (!g_quit && nowMs() < end) {
        pollfd fds[3] = {{pen.fd(), POLLIN, 0}, {touch.fd(), POLLIN, 0}, {pen.pressureFd(), POLLIN, 0}};
        if (poll(fds, 3, 250) <= 0) continue;
        std::vector<scrible::PenEvent> pe;
        std::vector<scrible::TouchEvent> te;
        pen.readPressure();
        if (pen.fd() >= 0) pen.read(pe);
        if (touch.fd() >= 0) touch.read(te);
        std::vector<scrible::Damage> marks;
        for (const auto& e : pe) {
            std::printf("pen   %-4s %7.1f %7.1f  pressure %.2f%s\n", kinds[static_cast<int>(e.type)], e.x, e.y,
                        e.pressure, e.eraser ? "  eraser" : e.button ? "  button" : "");
            if (e.type != scrible::PenEvent::Type::Up) stats.add(e.pressure);
            const int r = 2 + static_cast<int>(e.pressure * 6);
            const scrible::IRect m{int(e.x) - r, int(e.y) - r, int(e.x) + r + 1, int(e.y) + r + 1};
            c.fill(m, 0);
            marks.push_back({m, scrible::Refresh::Ink});
        }
        for (const auto& e : te) {
            std::printf("touch %-4s %7.1f %7.1f\n", kinds[static_cast<int>(e.type)], e.x, e.y);
            if (e.type != scrible::TouchEvent::Type::Down) continue;
            const scrible::IRect m{int(e.x) - 30, int(e.y) - 30, int(e.x) + 30, int(e.y) + 30};
            c.strokeRect(m, 6, 0);
            marks.push_back({m, scrible::Refresh::UI});
        }
        display.present(c, marks);
        std::fflush(stdout);
    }
    stats.log(pen);
    return 0;
}

}  // namespace

// Tells powerd the Kindle is in use: the app grabs the pen and touchscreen, so it can't
// see that for itself. Then the Kindle still sleeps when left idle, and with the power
// button, and not while you write. (KOReader does the same; at most once a minute.)
void stillHere(uint64_t now) {
    static uint64_t last = 0;
    if (last && now < last + 60000) return;
    last = now ? now : 1;
    std::system("lipc-set-prop -i com.lab126.powerd touchScreenSaverTimeout 1 >/dev/null 2>&1 &");
}

int main(int argc, char** argv) {
    Args args;
    if (!parseArgs(argc, argv, args)) {
        std::fprintf(stderr,
                     "usage: %s [FOLDER | notebook.xopp] [--pen DEV] [--touch DEV] [--pen-transform SPEC]\n"
                     "          [--touch-transform SPEC] [--no-grab] [--no-pen-mode] [--no-a2] [--probe [SECONDS]]\n",
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
    scrible::PowerEvents power;
    const char* stopped = std::getenv("SCRIBLE_AWESOME_STOPPED");
    const bool awesomeStopped = stopped && std::string(stopped) == "yes";
    if (args.probeSeconds < 0 && !power.start()) std::fprintf(stderr, "scrible: no sleep/wake notifications\n");

    scrible::FbinkDisplay display;
    scrible::DisplayOptions dopts;
    dopts.penMode = args.penMode;
    dopts.a2 = args.a2;
    if (!display.open(dopts)) return 1;
    std::fprintf(stderr, "scrible: %s\n", display.describe().c_str());

    scrible::InputTransform penT, touchT;
    const scrible::TouchQuirks q = display.touchQuirks();
    touchT.swapAxes = q.swapAxes;
    touchT.mirrorX = q.mirrorX;
    touchT.mirrorY = q.mirrorY;
    if ((!args.penTransform.empty() && !scrible::InputTransform::parse(args.penTransform, penT)) ||
        (!args.touchTransform.empty() && !scrible::InputTransform::parse(args.touchTransform, touchT))) {
        std::fprintf(stderr, "scrible: transforms are comma-separated: swap, flipx, flipy\n");
        return 2;
    }

    if (args.probeSeconds >= 0) return probe(display, args, penT, touchT);

    const auto devices = scrible::scanInputDevices();
    const auto* penInfo = scrible::pickPen(devices);
    const auto* touchInfo = scrible::pickTouch(devices);
    const std::string penPath = !args.penDev.empty() ? args.penDev : penInfo ? penInfo->path : "";
    const std::string touchPath = !args.touchDev.empty() ? args.touchDev : touchInfo ? touchInfo->path : "";

    scrible::PenInput pen;
    scrible::TouchInput touch;
    std::string err;
    if (!openPen(pen, devices, penPath, display.width(), display.height(), penT, args.grab, err)) {
        std::fprintf(stderr, "scrible: no usable pen device %s\n", err.c_str());
        showMessage(display, {"No pen input device found.", "Run scrible --probe over SSH for details."});
        sleep(6);
        return 1;
    }
    if (touchPath.empty() || !touch.open(touchPath, display.width(), display.height(), touchT, args.grab, err))
        std::fprintf(stderr, "scrible: no touch input (%s); the toolbar still works with the pen\n", err.c_str());
    std::fprintf(stderr, "scrible: pen %s, touch %s, opening %s\n", penPath.c_str(),
                 touch.fd() >= 0 ? touchPath.c_str() : "(none)", args.path.c_str());

    scrible::AppOptions aopts;
    aopts.editor.dpi = display.dpi();
    aopts.editor.render.antialias = false;  // what DU shows while writing is then exactly the final stroke
    if (scrible::isNotebookFile(args.path)) aopts.file = args.path;
    else aopts.notebooksDir = args.path;
    aopts.platformInfo = {display.aboutLine()};
    scrible::App app(display.width(), display.height(), aopts);
    if (!app.startupError().empty()) {
        std::fprintf(stderr, "scrible: %s\n", app.startupError().c_str());
        showMessage(display, {"Can't open:", args.path, app.startupError(), "", "Nothing was changed."});
        sleep(6);
        return 1;
    }
    display.present(app.canvas(), app.takeDamage());

    std::vector<scrible::PenEvent> penEvents;
    std::vector<scrible::TouchEvent> touchEvents;
    PressureStats stats;
    // While the Kindle shows its sleep or lock screen, the input is its own and the
    // screen is left alone. Lifts still come through, so a stroke or pinch ends cleanly.
    bool asleep = false;
    while (!g_quit && !app.quitRequested()) {
        pollfd fds[4] = {{pen.fd(), POLLIN, 0}, {touch.fd(), POLLIN, 0}, {power.fd(), POLLIN, 0},
                         {pen.pressureFd(), POLLIN, 0}};
        if (poll(fds, 4, app.msUntilIdleWork(nowMs())) < 0 && errno != EINTR) {
            std::perror("scrible: poll");
            break;
        }
        const uint64_t now = nowMs();

        if (fds[3].revents & POLLIN) pen.readPressure();
        penEvents.clear();
        if (fds[0].revents & POLLIN) pen.read(penEvents);
        if (!asleep && (!penEvents.empty() || (fds[1].revents & POLLIN))) stillHere(now);
        for (const auto& e : penEvents) {
            if (asleep && e.type != scrible::PenEvent::Type::Up) continue;
            if (e.type != scrible::PenEvent::Type::Up) stats.add(e.pressure);
            const scrible::PenSample s{e.x, e.y, e.pressure, e.eraser, e.button};
            switch (e.type) {
                case scrible::PenEvent::Type::Down: app.penDown(s, now); break;
                case scrible::PenEvent::Type::Move: app.penMove(s, now); break;
                case scrible::PenEvent::Type::Up: app.penUp(now); break;
            }
        }

        touchEvents.clear();
        if (fds[1].revents & POLLIN) touch.read(touchEvents);
        // Pinching only works with the pen away from the screen (palm rejection).
        app.setPenNear(pen.near(), now);
        for (const auto& e : touchEvents) {
            if (asleep && e.type != scrible::TouchEvent::Type::Up) continue;
            const int x = static_cast<int>(e.x), y = static_cast<int>(e.y);
            switch (e.type) {
                case scrible::TouchEvent::Type::Down: app.touchDown(e.id, x, y, now); break;
                case scrible::TouchEvent::Type::Move: app.touchMove(e.id, x, y, now); break;
                case scrible::TouchEvent::Type::Up: app.touchUp(e.id, now); break;
            }
        }

        if (fds[2].revents & (POLLIN | POLLHUP)) {
            for (const auto ev : power.read()) {
                if (ev == scrible::PowerEvents::Event::GoingToSleep && !asleep) {
                    asleep = true;
                    app.saveNow(now);
                    if (args.grab) {
                        pen.grab(false);
                        touch.grab(false);
                    }
                    // The Kindle's sleep screen is drawn by the window manager the launcher froze.
                    if (awesomeStopped) std::system("killall -CONT awesome");
                }
                if (ev == scrible::PowerEvents::Event::WokeUp && asleep) {
                    asleep = false;
                    if (args.grab) {
                        pen.grab(true);
                        touch.grab(true);
                    }
                    if (awesomeStopped) std::system("killall -STOP awesome");
                    app.redrawAll(scrible::Refresh::Full);
                }
            }
        }

        app.onIdle(now);
        const auto damage = app.takeDamage();
        if (!asleep) display.present(app.canvas(), damage);
    }

    stats.log(pen);
    std::fprintf(stderr, "scrible: pen hover %s\n", pen.hoverSeen() ? "reported" : "never reported (palm rejection only while writing)");
    return app.saveNow(nowMs()) ? 0 : 1;
}
