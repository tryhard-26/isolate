#pragma once

#include "core/store.h"
#include "fsview/diff.h"
#include "proc/tracker.h"
#include "util/json.h"

#include <string>
#include <vector>

namespace isolate::cli {

// Every cmd supports --json, so each builds a json::Value and
// either prints it or renders a table from the same data.

std::string format_timestamp(int64_t unix_seconds);

// "4 minutes ago", "just now".
std::string format_relative(int64_t unix_seconds);

// Left-aligned, 2 spaces between cols, sized to contents.
void print_table(const std::vector<std::string>& headers,
                 const std::vector<std::vector<std::string>>& rows);

void print_fields(const std::vector<std::pair<std::string, std::string>>& fields);
void print_heading(const std::string& text);
void print_json(const json::Value& value);

// One line per change, prefixed w/ its marker.
void print_diff(const std::string& label, const fsview::Diff& diff, bool show_stats);

json::Value diff_to_json(const fsview::Diff& diff);
json::Value capsule_to_json(const Capsule& capsule, bool include_policy);
json::Value process_to_json(const proc::ProcInfo& info);

} // namespace isolate::cli
