#include "EvdevInput.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace scrible {
namespace {

constexpr size_t kLongBits = sizeof(unsigned long) * 8;
constexpr size_t nlongs(size_t bits) { return (bits + kLongBits - 1) / kLongBits; }

bool testBit(const unsigned long* bits, int bit) { return (bits[bit / kLongBits] >> (bit % kLongBits)) & 1UL; }

struct Caps {
    unsigned long ev[nlongs(EV_MAX + 1)] = {};
    unsigned long key[nlongs(KEY_MAX + 1)] = {};
    unsigned long abs[nlongs(ABS_MAX + 1)] = {};
};

bool readCaps(int fd, Caps& c) {
    return ioctl(fd, EVIOCGBIT(0, sizeof c.ev), c.ev) >= 0 && ioctl(fd, EVIOCGBIT(EV_KEY, sizeof c.key), c.key) >= 0 &&
           ioctl(fd, EVIOCGBIT(EV_ABS, sizeof c.abs), c.abs) >= 0;
}

AxisRange absRange(int fd, int code) {
    input_absinfo ai{};
    if (ioctl(fd, EVIOCGABS(code), &ai) < 0) return {};
    return {ai.minimum, ai.maximum};
}

}  // namespace

bool InputTransform::parse(const std::string& spec, InputTransform& t) {
    t = {};
    size_t start = 0;
    while (start < spec.size()) {
        size_t end = spec.find(',', start);
        if (end == std::string::npos) end = spec.size();
        const std::string tok = spec.substr(start, end - start);
        if (tok == "swap") t.swapAxes = true;
        else if (tok == "flipx") t.mirrorX = true;
        else if (tok == "flipy") t.mirrorY = true;
        else if (!tok.empty() && tok != "none") return false;
        start = end + 1;
    }
    return true;
}

std::vector<InputDeviceInfo> scanInputDevices() {
    std::vector<InputDeviceInfo> out;
    DIR* dir = opendir("/dev/input");
    if (!dir) return out;
    while (const dirent* e = readdir(dir)) {
        if (std::strncmp(e->d_name, "event", 5) != 0) continue;
        InputDeviceInfo info;
        info.path = std::string("/dev/input/") + e->d_name;
        const int fd = ::open(info.path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        char name[256] = "";
        ioctl(fd, EVIOCGNAME(sizeof name - 1), name);
        info.name = name;
        Caps c;
        if (readCaps(fd, c)) {
            info.isPen = testBit(c.key, BTN_TOOL_PEN) && testBit(c.abs, ABS_X) && testBit(c.abs, ABS_Y) &&
                         testBit(c.abs, ABS_PRESSURE);
            info.isTouch = !info.isPen && testBit(c.abs, ABS_MT_POSITION_X) && testBit(c.abs, ABS_MT_POSITION_Y);
            if (info.isPen) {
                info.x = absRange(fd, ABS_X);
                info.y = absRange(fd, ABS_Y);
                info.pressure = absRange(fd, ABS_PRESSURE);
            } else if (info.isTouch) {
                info.x = absRange(fd, ABS_MT_POSITION_X);
                info.y = absRange(fd, ABS_MT_POSITION_Y);
            }
        }
        ::close(fd);
        out.push_back(std::move(info));
    }
    closedir(dir);
    // event10 after event9
    std::sort(out.begin(), out.end(), [](const InputDeviceInfo& a, const InputDeviceInfo& b) {
        return a.path.size() != b.path.size() ? a.path.size() < b.path.size() : a.path < b.path;
    });
    return out;
}

const InputDeviceInfo* pickPen(const std::vector<InputDeviceInfo>& devices) {
    // On the Scribe the framework re-emits the Wacom digitizer as a virtual
    // "stylus-custom" device; that's the one KOReader found to deliver events.
    for (const auto& d : devices)
        if (d.isPen && d.name == "stylus-custom") return &d;
    for (const auto& d : devices)
        if (d.isPen) return &d;
    return nullptr;
}

const InputDeviceInfo* pickTouch(const std::vector<InputDeviceInfo>& devices) {
    for (const auto& d : devices)
        if (d.isTouch) return &d;
    return nullptr;
}

const InputDeviceInfo* pickPressureSource(const std::vector<InputDeviceInfo>& devices, const std::string& penPath) {
    for (const auto& d : devices)
        if (d.isPen && d.path != penPath && d.name == "WacomDigitizer" && d.pressure.max > d.pressure.min) return &d;
    for (const auto& d : devices)
        if (d.isPen && d.path != penPath && d.pressure.max > d.pressure.min) return &d;
    return nullptr;
}

// ---- EvdevDevice ----

EvdevDevice::~EvdevDevice() {
    if (fd_ >= 0) ::close(fd_);  // also releases a grab
}

bool EvdevDevice::open(const std::string& path, int screenWidth, int screenHeight, const InputTransform& t,
                       bool grab, std::string& error) {
    fd_ = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
        error = path + ": " + std::strerror(errno);
        return false;
    }
    width_ = screenWidth;
    height_ = screenHeight;
    transform_ = t;
    if (grab && ioctl(fd_, EVIOCGRAB, 1) < 0)
        std::fprintf(stderr, "scrible: couldn't grab %s: %s\n", path.c_str(), std::strerror(errno));
    return true;
}

void EvdevDevice::toScreen(int rawX, int rawY, double& x, double& y) const {
    double nx = x_.normalize(rawX), ny = y_.normalize(rawY);
    if (transform_.swapAxes) std::swap(nx, ny);
    if (transform_.mirrorX) nx = 1 - nx;
    if (transform_.mirrorY) ny = 1 - ny;
    x = nx * (width_ - 1);
    y = ny * (height_ - 1);
}

template <typename Handle>
void EvdevDevice::readEvents(int fd, Handle&& handle) {
    input_event evs[64];
    for (;;) {
        const ssize_t n = ::read(fd, evs, sizeof evs);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;  // EAGAIN: drained
        for (size_t i = 0; i < static_cast<size_t>(n) / sizeof(input_event); ++i) handle(evs[i]);
    }
}

// ---- PenInput ----

PenInput::~PenInput() {
    if (pressureFd_ >= 0) ::close(pressureFd_);
}

bool PenInput::openPressureSource(const std::string& path, std::string& error) {
    pressureFd_ = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (pressureFd_ < 0) {
        error = path + ": " + std::strerror(errno);
        return false;
    }
    extPressure_ = absRange(pressureFd_, ABS_PRESSURE);
    return true;
}

void PenInput::readPressure() {
    if (pressureFd_ < 0) return;
    readEvents(pressureFd_, [&](const input_event& e) {
        if (e.type != EV_ABS || e.code != ABS_PRESSURE) return;
        extP_ = e.value;
        if (e.value > 0) extPressureSeen_ = true;
    });
}

double PenInput::currentPressure() const {
    // Until a device has shown it really reports pressure, assume medium pressure,
    // which gives the preset width (a constant 0 would draw everything at the minimum).
    if (ownPressureSeen_) return std::clamp(pressure_.normalize(rawP_), 0.0, 1.0);
    if (extPressureSeen_) return std::clamp(extPressure_.normalize(extP_), 0.0, 1.0);
    return 0.5;
}

const char* PenInput::pressureOrigin() const {
    return ownPressureSeen_ ? "pen" : extPressureSeen_ ? "pressure source" : "none";
}

bool PenInput::open(const std::string& path, int screenWidth, int screenHeight, const InputTransform& t, bool grab,
                    std::string& error) {
    if (!EvdevDevice::open(path, screenWidth, screenHeight, t, grab, error)) return false;
    x_ = absRange(fd_, ABS_X);
    y_ = absRange(fd_, ABS_Y);
    pressure_ = absRange(fd_, ABS_PRESSURE);
    resync();
    return true;
}

// After SYN_DROPPED the kernel's buffered events are gone; read the current state instead.
void PenInput::resync() {
    unsigned long keys[nlongs(KEY_MAX + 1)] = {};
    if (ioctl(fd_, EVIOCGKEY(sizeof keys), keys) >= 0) {
        touching_ = testBit(keys, BTN_TOUCH);
        rubber_ = testBit(keys, BTN_TOOL_RUBBER);
        inRange_ = testBit(keys, BTN_TOOL_PEN);
        button_ = testBit(keys, BTN_STYLUS) || testBit(keys, BTN_STYLUS2);
    }
    input_absinfo ai{};
    if (ioctl(fd_, EVIOCGABS(ABS_X), &ai) >= 0) rawX_ = ai.value;
    if (ioctl(fd_, EVIOCGABS(ABS_Y), &ai) >= 0) rawY_ = ai.value;
    if (ioctl(fd_, EVIOCGABS(ABS_PRESSURE), &ai) >= 0) rawP_ = ai.value;
}

void PenInput::read(std::vector<PenEvent>& out) {
    readEvents(fd_, [&](const input_event& e) {
        if (dropped_) {
            if (e.type == EV_SYN && e.code == SYN_REPORT) {
                dropped_ = false;
                resync();
            }
            return;
        }
        switch (e.type) {
            case EV_KEY:
                if (e.code == BTN_TOUCH) touching_ = e.value != 0;
                else if (e.code == BTN_TOOL_RUBBER) rubber_ = e.value != 0;
                else if (e.code == BTN_TOOL_PEN) {
                    inRange_ = e.value != 0;
                    if (inRange_ && !touching_) hoverSeen_ = true;
                }
                else if (e.code == BTN_STYLUS || e.code == BTN_STYLUS2) button_ = e.value != 0;
                return;
            case EV_ABS:
                if (e.code == ABS_X) rawX_ = e.value;
                else if (e.code == ABS_Y) rawY_ = e.value;
                else if (e.code == ABS_PRESSURE) {
                    rawP_ = e.value;
                    if (e.value > 0) ownPressureSeen_ = true;
                }
                return;
            case EV_SYN:
                if (e.code == SYN_DROPPED) {
                    dropped_ = true;
                    return;
                }
                if (e.code != SYN_REPORT) return;
                break;
            default: return;
        }

        double x, y;
        toScreen(rawX_, rawY_, x, y);
        const double p = currentPressure();
        if (touching_ && !down_) {
            down_ = true;
            eraser_ = rubber_;
            buttonAtDown_ = button_ && !rubber_;
            out.push_back({PenEvent::Type::Down, x, y, p, eraser_, buttonAtDown_});
        } else if (touching_) {
            out.push_back({PenEvent::Type::Move, x, y, p, eraser_, buttonAtDown_});
        } else if (down_) {
            down_ = false;
            out.push_back({PenEvent::Type::Up, x, y, p, eraser_, buttonAtDown_});
        }
    });
}

// ---- TouchInput (multitouch protocol B) ----

bool TouchInput::open(const std::string& path, int screenWidth, int screenHeight, const InputTransform& t,
                      bool grab, std::string& error) {
    if (!EvdevDevice::open(path, screenWidth, screenHeight, t, grab, error)) return false;
    x_ = absRange(fd_, ABS_MT_POSITION_X);
    y_ = absRange(fd_, ABS_MT_POSITION_Y);
    return true;
}

void TouchInput::read(std::vector<TouchEvent>& out) {
    readEvents(fd_, [&](const input_event& e) {
        if (dropped_) {
            // Simplest safe recovery: lift every finger.
            if (e.type == EV_SYN && e.code == SYN_REPORT) {
                dropped_ = false;
                for (int i = 0; i < kSlots; ++i) {
                    slots_[i].id = -1;
                    if (down_[i]) out.push_back({TouchEvent::Type::Up, i, 0, 0});
                    down_[i] = false;
                }
            }
            return;
        }
        Slot& s = slots_[cur_];
        if (e.type == EV_ABS) {
            switch (e.code) {
                case ABS_MT_SLOT: cur_ = std::clamp(e.value, 0, kSlots - 1); break;
                case ABS_MT_TRACKING_ID: s.id = e.value; break;
                case ABS_MT_POSITION_X: s.x = e.value; break;
                case ABS_MT_POSITION_Y: s.y = e.value; break;
                case ABS_MT_TOOL_TYPE: s.pen = e.value == MT_TOOL_PEN; break;
                default: break;
            }
            return;
        }
        if (e.type != EV_SYN) return;
        if (e.code == SYN_DROPPED) {
            dropped_ = true;
            return;
        }
        if (e.code != SYN_REPORT) return;

        // Every finger (not the pen) that went down, moved or lifted since the last report.
        for (int i = 0; i < kSlots; ++i) {
            const bool finger = slots_[i].id >= 0 && !slots_[i].pen;
            double x, y;
            toScreen(slots_[i].x, slots_[i].y, x, y);
            if (finger) out.push_back({down_[i] ? TouchEvent::Type::Move : TouchEvent::Type::Down, i, x, y});
            else if (down_[i]) out.push_back({TouchEvent::Type::Up, i, x, y});
            down_[i] = finger;
        }
    });
}

}  // namespace scrible
