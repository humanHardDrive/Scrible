#include "PowerEvents.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>

namespace scrible {

bool PowerEvents::start() {
    int p[2];
    if (pipe(p) < 0) return false;
    pid_ = fork();
    if (pid_ < 0) {
        ::close(p[0]);
        ::close(p[1]);
        return false;
    }
    if (pid_ == 0) {
        dup2(p[1], STDOUT_FILENO);
        ::close(p[0]);
        ::close(p[1]);
        const int null = ::open("/dev/null", O_WRONLY);
        if (null >= 0) dup2(null, STDERR_FILENO);
        // -m: keep listening after the first event; -s 0: no timeout.
        execlp("lipc-wait-event", "lipc-wait-event", "-m", "-s", "0", "com.lab126.powerd",
               "goingToScreenSaver,outOfScreenSaver,exitingScreenSaver", static_cast<char*>(nullptr));
        _exit(127);
    }
    ::close(p[1]);
    fd_ = p[0];
    fcntl(fd_, F_SETFL, O_NONBLOCK);
    fcntl(fd_, F_SETFD, FD_CLOEXEC);
    return true;
}

void PowerEvents::stop() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    if (pid_ > 0) {
        kill(pid_, SIGTERM);
        waitpid(pid_, nullptr, 0);
    }
    pid_ = -1;
}

std::vector<PowerEvents::Event> PowerEvents::read() {
    std::vector<Event> out;
    char buf[256];
    for (;;) {
        const ssize_t n = ::read(fd_, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n == 0) {
            stop();  // child exited (lipc-wait-event missing or unsupported flags)
            break;
        }
        if (n < 0) break;
        pending_.append(buf, static_cast<size_t>(n));
    }
    size_t nl;
    while ((nl = pending_.find('\n')) != std::string::npos) {
        const std::string line = pending_.substr(0, nl);
        pending_.erase(0, nl + 1);
        std::fprintf(stderr, "scrible: powerd: %s\n", line.c_str());  // e.g. "goingToScreenSaver 2" (the button); exitingScreenSaver is only logged
        if (line.find("goingToScreenSaver") != std::string::npos) out.push_back(Event::GoingToSleep);
        else if (line.find("outOfScreenSaver") != std::string::npos) out.push_back(Event::WokeUp);
    }
    return out;
}

}  // namespace scrible
