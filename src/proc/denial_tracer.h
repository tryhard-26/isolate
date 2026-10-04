#pragma once

#include <atomic>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace isolate::proc {

class DenialTracer {
public:
    DenialTracer();
    ~DenialTracer();

    DenialTracer(const DenialTracer&) = delete;
    DenialTracer& operator=(const DenialTracer&) = delete;

    void start();
    void stop();

private:
    std::atomic<bool> running_{false};
    pid_t log_pid_ = -1;
    int pipe_fd_ = -1;
    std::thread worker_;
    std::set<std::string> reported_services_;

    void run_loop();
};

} // namespace isolate::proc
