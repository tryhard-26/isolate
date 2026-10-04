#pragma once

#include "core/store.h"
#include "proc/tracker.h"

#include <chrono>
#include <string>

namespace isolate::proc {

enum class Termination {
    Exited,
    Signalled,
    MemoryLimit,
    ProcessLimit,
    TimeLimit,
    Interrupted,
    LaunchFailed,
};

const char* to_string(Termination t);

struct RunReport {
    Termination termination = Termination::Exited;
    int exit_code = 0;
    int signal_number = 0;

    double wall_seconds = 0;
    double cpu_seconds = 0;
    uint64_t peak_footprint = 0;
    size_t peak_process_count = 0;

    // Surviving procs post-exit; forced_kill = SIGKILL used
    size_t leftover_processes = 0;
    bool forced_kill = false;

    // Degraded attestation flag
    bool attestation_degraded = false;

    // Overshoot delta past mem limit
    uint64_t memory_overshoot = 0;

    std::string detail;

    int exit_status_for_isolate() const;
};

class PtySession;

struct SupervisorOptions {
    // Default sample interval
    std::chrono::milliseconds sample_interval{250};

    // Tighter sample window for --memory
    std::chrono::milliseconds memory_sample_interval{50};

    std::chrono::milliseconds grace_period{2000}; // SIGTERM -> SIGKILL
    bool announce = true;
    bool use_pty = false;
    bool trace_denials = false;
};

// Supervise proc lifecycle via kqueue (mem/proc limits, timeout, sigs, cleanup)
class Supervisor {
public:
    Supervisor(Capsule& capsule, SupervisorOptions options);

    // Throws on launch failure
    RunReport run(const std::vector<std::string>& argv, const std::string& profile,
                  PtySession* pty = nullptr);

private:
    Capsule& capsule_;
    SupervisorOptions options_;
};

} // namespace isolate::proc
