#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace isolate::path {

// Lexical path normalisation without fs access.
std::string normalize(std::string_view p);

// Absolute path relative to base or cwd.
std::string absolute(std::string_view p, std::string_view base = {});

// realpath(3) resolution; throws if missing.
std::string resolve(std::string_view p);

// Resolves longest existing prefix.
std::string resolve_lenient(std::string_view p);

std::string dirname(std::string_view p);
std::string basename(std::string_view p);
std::string join(std::string_view a, std::string_view b);

// Check if child path is within parent dir.
bool is_within(std::string_view parent, std::string_view child);

// Relative path computation.
std::string relative_to(std::string_view parent, std::string_view child);

// Path ancestor list from root downward.
std::vector<std::string> ancestors(std::string_view p);

// Resolved user home dir ($HOME or passwd).
std::string home_dir();

// Capsule storage state dir ($ISOLATE_HOME or ~/.local/share/isolate).
std::string state_dir();

std::string self_executable();

} // namespace isolate::path
