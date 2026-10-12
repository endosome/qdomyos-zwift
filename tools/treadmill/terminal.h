#pragma once
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

inline volatile sig_atomic_t signalWriteFd = -1;
inline void terminationSignal(int) {
    const int savedErrno = errno;
    const char value = 's';
    if (signalWriteFd >= 0) {
        const auto ignored = ::write(signalWriteFd, &value, 1);
        (void)ignored;
    }
    errno = savedErrno;
}
// Own terminal/signal settings so every normal exit restores the caller's state.
class InputGuard {
public:
    explicit InputGuard(bool interactive) {
        if (::pipe2(signalPipe, O_NONBLOCK | O_CLOEXEC) == 0) {
            signalWriteFd = signalPipe[1];
            struct sigaction action {};
            action.sa_handler = terminationSignal;
            sigemptyset(&action.sa_mask);
            for (int i = 0; i < 3; ++i) {
                if (::sigaction(terminationSignals[i], &action, &previous[i]) != 0) break;
                ++installed;
            }
        }
        if (interactive && ::isatty(STDIN_FILENO) && ::tcgetattr(STDIN_FILENO, &terminal) == 0) {
            auto raw = terminal;
            raw.c_lflag &= ~(ICANON | ECHO);
            raw.c_cc[VMIN] = 1;
            raw.c_cc[VTIME] = 0;
            terminalChanged = ::tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
        }
    }
    ~InputGuard() {
        for (int i = 0; i < installed; ++i) ::sigaction(terminationSignals[i], &previous[i], nullptr);
        signalWriteFd = -1;
        if (signalPipe[0] >= 0) ::close(signalPipe[0]);
        if (signalPipe[1] >= 0) ::close(signalPipe[1]);
        if (terminalChanged) ::tcsetattr(STDIN_FILENO, TCSANOW, &terminal);
    }
    bool valid() const { return signalPipe[0] >= 0 && installed == 3; }
    int signalPipe[2] = {-1, -1};
private:
    const int terminationSignals[3] = {SIGINT, SIGTERM, SIGHUP};
    struct sigaction previous[3] {};
    int installed = 0;
    struct termios terminal {};
    bool terminalChanged = false;
};

