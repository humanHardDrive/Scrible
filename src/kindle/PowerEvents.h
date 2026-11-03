#pragma once

#include <sys/types.h>

#include <string>
#include <vector>

namespace scrible {

// Sleep/wake notifications from the Kindle's power daemon, via a
// `lipc-wait-event` child process (avoids linking Amazon's liblipc).
class PowerEvents {
public:
    enum class Event { GoingToSleep, WokeUp };

    ~PowerEvents() { stop(); }
    bool start();  // false if lipc-wait-event can't be run (e.g. not on a Kindle)
    void stop();
    int fd() const { return fd_; }
    std::vector<Event> read();

private:
    pid_t pid_ = -1;
    int fd_ = -1;
    std::string pending_;
};

}  // namespace scrible
