#pragma once

#include "core/spec.h"
#include "core/store.h"

#include <string>
#include <sys/types.h>
#include <vector>

namespace isolate::proc {

struct LaunchRequest {
    std::vector<std::string> argv;
    std::vector<std::string> envp; // NAME=value, used verbatim
    std::string cwd;
    std::string profile;

    Limits limits;
    Credentials credentials;

    // Detach session from tty/pgrp
    bool new_session = true;

    // Optional fd redirects (-1 = inherit)
    int stdin_fd = -1;
    int stdout_fd = -1;
    int stderr_fd = -1;
};

// Locate libisolate_shim.dylib
std::string find_shim_library();

// Spawn root proc: setsid, sigs, chdir, creds, rlimits, sb profile, exec
pid_t launch(const LaunchRequest& request);

// Build capsule env
std::vector<std::string> build_environment(const Spec& spec, const Capsule& capsule);

// Resolve prog via PATH
std::string resolve_program(const std::string& name, const std::vector<std::string>& envp);

} // namespace isolate::proc
