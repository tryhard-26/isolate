#pragma once

#include <string>

namespace isolate::sb {

// Applies a profile to self, irreversibly. nullptr on success,
// else a msg. No throw/alloc: called between fork and exec.
const char* apply_profile_nothrow(const char* profile);

struct ValidationResult {
    bool ok = false;
    std::string error;
};

// Compiles + applies in a throwaway child. Catches a bad profile
// here instead of post-fork, where reporting is harder.
ValidationResult validate_profile(const std::string& profile);

// Nesting would intersect two profiles; run refuses if true.
bool self_is_sandboxed();

} // namespace isolate::sb
