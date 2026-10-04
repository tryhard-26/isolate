#pragma once

#include "core/spec.h"

#include <string>
#include <vector>

namespace isolate::sb {

// Dynamic runtime inputs for sb profile gen.
struct ProfileInputs {
    const Spec* spec = nullptr;

    std::string capsule_tmp;  // Empty if no private tmp
    std::string capsule_home; // Empty if no private home
    std::string capsule_dir;  // Capsule store path

    std::string tty_path;     // Controlling tty path (host or pty proxy)
    std::string program_path; // Resolved exec target
    std::string shim_path;    // Interception dylib path
    std::string membership_name; // Mach tag for proc membership
    bool log_denials = false;    // Log denials to system log
};

// Generate SBPL profile string from inputs.
std::string build_profile(const ProfileInputs& inputs);

// Split SBPL on section markers (test helper).
std::vector<std::string> profile_sections(const std::string& profile);

} // namespace isolate::sb
