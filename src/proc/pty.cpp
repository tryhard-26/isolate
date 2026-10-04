#include "proc/pty.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <util.h>

#include <cerrno>
#include <cstring>

namespace isolate::proc {

PtySession::PtySession() = default;

PtySession::~PtySession() {
    restore_mode();
    close_all();
}

bool PtySession::open() {
    char name[128] = {0};
    struct winsize ws {};
    if (::isatty(STDIN_FILENO) == 1) {
        ::ioctl(STDIN_FILENO, TIOCGWINSZ, &ws);
    }
    if (::openpty(&master_fd_, &slave_fd_, name, nullptr, ws.ws_col > 0 ? &ws : nullptr) != 0) {
        return false;
    }
    slave_name_ = name;

    int flags = ::fcntl(master_fd_, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(master_fd_, F_SETFL, flags | O_NONBLOCK);
    }
    return true;
}

void PtySession::close_slave() {
    if (slave_fd_ >= 0) {
        ::close(slave_fd_);
        slave_fd_ = -1;
    }
}

void PtySession::close_all() {
    close_slave();
    if (master_fd_ >= 0) {
        ::close(master_fd_);
        master_fd_ = -1;
    }
}

void PtySession::enter_raw_mode() {
    if (::isatty(STDIN_FILENO) != 1) return;
    if (::tcgetattr(STDIN_FILENO, &orig_termios_) != 0) return;

    struct termios raw = orig_termios_;
    ::cfmakeraw(&raw);
    if (::tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) {
        raw_active_ = true;
    }
}

void PtySession::restore_mode() {
    if (raw_active_) {
        ::tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios_);
        raw_active_ = false;
    }
}

void PtySession::update_window_size() {
    if (master_fd_ < 0 || ::isatty(STDIN_FILENO) != 1) return;
    struct winsize ws {};
    if (::ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0) {
        ::ioctl(master_fd_, TIOCSWINSZ, &ws);
    }
}

bool PtySession::pump_to_slave() {
    if (master_fd_ < 0) return false;
    char buf[4096];
    ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0) return false;

    ssize_t written = 0;
    while (written < n) {
        ssize_t w = ::write(master_fd_, buf + written, static_cast<size_t>(n - written));
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return false;
        }
        written += w;
    }
    return true;
}

bool PtySession::pump_from_slave() {
    if (master_fd_ < 0) return false;
    char buf[4096];
    ssize_t n = ::read(master_fd_, buf, sizeof(buf));
    if (n <= 0) return false;

    ssize_t written = 0;
    while (written < n) {
        ssize_t w = ::write(STDOUT_FILENO, buf + written, static_cast<size_t>(n - written));
        if (w < 0) {
            if (errno == EINTR) continue;
            break;
        }
        written += w;
    }
    return true;
}

} // namespace isolate::proc
