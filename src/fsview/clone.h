#pragma once

#include <string>

namespace isolate::fsview {

// VOL_CAP_INT_CLONE. Volume format capability, so a read-only
// volume still says yes; clone_tree falls back on the real error.
bool supports_cloning(const std::string& p);

// "apfs", "hfs", ... For diagnostics and doctor.
std::string filesystem_type(const std::string& p);

enum class CloneMethod {
    Clone, // clonefile(2); blocks shared until written
    Copy,  // copyfile(3); real copy, when cloning is unavailable
};

struct CloneResult {
    CloneMethod method = CloneMethod::Clone;
    uint64_t bytes_logical = 0;
    uint64_t bytes_on_disk = 0;
    uint64_t entries = 0;
};

// Clones to `dst`, which must not exist. clonefile does dirs
// recursively in one call, sharing blocks. Falls back to a full
// copy on ENOTSUP/EXDEV; result says which, so slowness is
// explainable.
CloneResult clone_tree(const std::string& src, const std::string& dst);

// Replaces `dst` if present. Writes a sibling, then renames.
void clone_file_over(const std::string& src, const std::string& dst);

} // namespace isolate::fsview
