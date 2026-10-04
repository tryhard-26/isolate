#pragma once

#include <string>
#include <sys/types.h>

namespace isolate::sb {

// Synthetic Mach tag in sb profile for unforgeable proc attestation.
// sandbox_check(pid, tag) confirms capsule membership post-exec.

std::string membership_name(const std::string& capsule_id);

enum class Membership {
    Member,
    NotMember,
    Gone,    // Exited during check
    Unknown, // Query failed
};

const char* to_string(Membership m);

// Check if proc has any sb profile active.
bool is_sandboxed(pid_t pid);

Membership check_membership(pid_t pid, const std::string& name);

// Probe membership check on sandbox child (used by doctor).
bool membership_checks_work();

} // namespace isolate::sb
