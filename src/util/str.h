#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace isolate::str {

std::vector<std::string> split(std::string_view s, char sep);
std::string join(const std::vector<std::string>& parts, std::string_view sep);
std::string_view trim(std::string_view s);
bool starts_with(std::string_view s, std::string_view prefix);
bool ends_with(std::string_view s, std::string_view suffix);
std::string lower(std::string_view s);

// Format bytes to human string (e.g. "1.5K").
std::string human_bytes(uint64_t bytes);

// Format duration (e.g. "3s", "4m12s").
std::string human_duration(double seconds);

// Parse byte count with K/M/G suffix. Throws on error.
uint64_t parse_bytes(std::string_view s);

// Parse duration strings (s/m/h). Throws on error.
double parse_duration(std::string_view s);

// Parse CPU cores or pct (e.g. "200%" -> 2.0).
double parse_cpu(std::string_view s);

// Map signal number to string name.
std::string signal_name(int signum);

// SBPL string literal quoting.
std::string sbpl_quote(std::string_view s);

// Shell quoting for display only.
std::string shell_quote(std::string_view s);

// Glob matching with support for *, ?, and trailing slash dir check.
bool match_glob(std::string_view pattern, std::string_view path, bool is_dir);

// Load ignore patterns from file (.isolateignore / .gitignore).
std::vector<std::string> load_ignore_file(const std::string& path);

} // namespace isolate::str
