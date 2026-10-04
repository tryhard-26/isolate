#pragma once

#include "core/spec.h"

#include <cstddef>
#include <string>
#include <vector>

namespace isolate::proc {

// Kernel rlimits: CPU, FSIZE, NOFILE, CORE. AS/DATA/NPROC unsupp on Darwin.
// Mem/proc ceilings enforced via supervisor sampling.

// Apply rlimits post-fork (no throw/alloc; returns err msg or null)
const char* apply_rlimits_nothrow(const Limits& limits);

// Apply umask and uid drop
const char* apply_credentials_nothrow(const Credentials& credentials);

// QoS prio clamp (0 = none)
int qos_clamp_for(const Limits& limits);

const char* qos_clamp_name(int clamp);

struct LimitNote {
    std::string name;
    std::string value;
    bool kernel_enforced = false;
    std::string mechanism;
};

// Limit notes for inspect / banner
std::vector<LimitNote> describe(const Limits& limits);

struct MemoryLimitProbe {
    bool footprint_limit_available = false;
    std::string footprint_limit_error;
    bool memorystatus_available = false;
    std::string memorystatus_error;
};

// Probe kernel mem limits for doctor
MemoryLimitProbe probe_kernel_memory_limits();

} // namespace isolate::proc
