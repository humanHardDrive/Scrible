#pragma once

// Pen and touch input straight from evdev (/dev/input/event*).

#include <string>
#include <vector>

namespace scrible {

// Maps raw device axes onto the screen: swap first, then mirror (FBInk's convention).
struct InputTransform {
    bool swapAxes = false, mirrorX = false, mirrorY = false;

    static bool parse(const std::string& spec, InputTransform& t);  // e.g. "swap,flipx"
};

struct AxisRange {
    int min = 0, max = 0;
    double normalize(int v) const { return max > min ? (v - min) / static_cast<double>(max - min) : 0.0; }
};

struct InputDeviceInfo {
    std::string path, name;
    bool isPen = false;    // Wacom-style stylus: BTN_TOOL_PEN + ABS_X/Y + ABS_PRESSURE
    bool isTouch = false;  // multitouch panel
    AxisRange x, y, pressure;
};

std::vector<InputDeviceInfo> scanInputDevices();
const InputDeviceInfo* pickPen(const std::vector<InputDeviceInfo>& devices);
const InputDeviceInfo* pickTouch(const std::vector<InputDeviceInfo>& devices);
// Another pen device to read pressure from: on the Scribe, "stylus-custom" has a
// pressure axis but always reports 0, while the raw "WacomDigitizer" has real pressure.
const InputDeviceInfo* pickPressureSource(const std::vector<InputDeviceInfo>& devices, const std::string& penPath);

struct PenEvent {
    enum class Type { Down, Move, Up } type;
    double x, y;      // screen pixels
    double pressure;  // 0..1
    bool eraser;      // eraser end
    bool button;      // side button held at pen-down
};

struct TouchEvent {
    enum class Type { Down, Move, Up } type;
    int id;  // which finger: its multitouch slot
    double x, y;
};

class EvdevDevice {
public:
    EvdevDevice() = default;
    EvdevDevice(const EvdevDevice&) = delete;
    EvdevDevice& operator=(const EvdevDevice&) = delete;
    ~EvdevDevice();

    // `grab` takes exclusive access, so the Kindle framework doesn't react to our input.
    bool open(const std::string& path, int screenWidth, int screenHeight, const InputTransform& t, bool grab,
              std::string& error);
    int fd() const { return fd_; }

protected:
    void toScreen(int rawX, int rawY, double& x, double& y) const;
    template <typename Handle>
    static void readEvents(int fd, Handle&& handle);

    int fd_ = -1;
    int width_ = 0, height_ = 0;
    InputTransform transform_;
    AxisRange x_, y_;
};

class PenInput : public EvdevDevice {
public:
    ~PenInput();
    bool open(const std::string& path, int screenWidth, int screenHeight, const InputTransform& t, bool grab,
              std::string& error);
    // Optional second device whose ABS_PRESSURE is used when the main one never
    // reports any. Opened read-only and never grabbed: on the Scribe the framework
    // reads it to produce the main device.
    bool openPressureSource(const std::string& path, std::string& error);
    int pressureFd() const { return pressureFd_; }
    void readPressure();  // call before read() when both are ready, so pressure is current

    void read(std::vector<PenEvent>& out);
    // Where pressure has come from so far: "pen", "pressure source", or "none" (fixed at 0.5).
    const char* pressureOrigin() const;
    // The pen is close enough to be sensed (hovering) or on the screen. hoverSeen()
    // says whether the device ever reported hovering, i.e. whether near() is useful.
    bool near() const { return inRange_ || rubber_ || touching_; }
    bool hoverSeen() const { return hoverSeen_; }

private:
    void resync();
    double currentPressure() const;

    AxisRange pressure_;
    int rawX_ = 0, rawY_ = 0, rawP_ = 0;
    bool touching_ = false, rubber_ = false, button_ = false, inRange_ = false, hoverSeen_ = false;
    bool down_ = false, eraser_ = false, buttonAtDown_ = false, dropped_ = false;
    bool ownPressureSeen_ = false;

    int pressureFd_ = -1;
    AxisRange extPressure_;
    int extP_ = 0;
    bool extPressureSeen_ = false;
};

class TouchInput : public EvdevDevice {
public:
    bool open(const std::string& path, int screenWidth, int screenHeight, const InputTransform& t, bool grab,
              std::string& error);
    void read(std::vector<TouchEvent>& out);

private:
    struct Slot {
        int id = -1, x = 0, y = 0;
        bool pen = false;
    };
    static constexpr int kSlots = 16;
    Slot slots_[kSlots];
    bool down_[kSlots] = {};  // a Down was reported for the finger in this slot, and no Up yet
    int cur_ = 0;
    bool dropped_ = false;
};

}  // namespace scrible
