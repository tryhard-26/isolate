#pragma once

#include <termios.h>
#include <string>

namespace isolate::proc {

class PtySession {
public:
    PtySession();
    ~PtySession();

    PtySession(const PtySession&) = delete;
    PtySession& operator=(const PtySession&) = delete;

    bool open();
    void close_slave();
    void close_all();

    int master_fd() const { return master_fd_; }
    int slave_fd() const { return slave_fd_; }
    const std::string& slave_name() const { return slave_name_; }
    bool is_open() const { return master_fd_ >= 0; }

    void enter_raw_mode();
    void restore_mode();
    void update_window_size();

    bool pump_to_slave();
    bool pump_from_slave();

private:
    int master_fd_ = -1;
    int slave_fd_ = -1;
    std::string slave_name_;
    struct termios orig_termios_ {};
    bool raw_active_ = false;
};

} // namespace isolate::proc
