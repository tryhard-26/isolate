#include "util/str.h"

#include "util/error.h"
#include "util/fs.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <fnmatch.h>

namespace isolate::str {

std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> out;
    if (s.empty()) return out;
    size_t start = 0;
    while (true) {
        size_t pos = s.find(sep, start);
        if (pos == std::string_view::npos) {
            out.emplace_back(s.substr(start));
            return out;
        }
        out.emplace_back(s.substr(start, pos - start));
        start = pos + 1;
    }
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

std::string_view trim(std::string_view s) {
    auto is_space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
    return s;
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return out;
}

std::string human_bytes(uint64_t bytes) {
    static const char* units[] = {"B", "K", "M", "G", "T", "P"};
    if (bytes < 1024) return std::to_string(bytes) + "B";
    double v = static_cast<double>(bytes);
    size_t u = 0;
    while (v >= 1024.0 && u + 1 < sizeof(units) / sizeof(units[0])) {
        v /= 1024.0;
        ++u;
    }
    char buf[32];
    // 1 decimal below 10, none above, as ls -lh.
    std::snprintf(buf, sizeof buf, v < 10.0 ? "%.1f%s" : "%.0f%s", v, units[u]);
    return buf;
}

std::string human_duration(double seconds) {
    if (seconds < 0) seconds = 0;
    auto total = static_cast<uint64_t>(seconds);
    char buf[64];
    if (total < 60) {
        std::snprintf(buf, sizeof buf, "%llus", static_cast<unsigned long long>(total));
    } else if (total < 3600) {
        std::snprintf(buf, sizeof buf, "%llum%02llus",
                      static_cast<unsigned long long>(total / 60),
                      static_cast<unsigned long long>(total % 60));
    } else {
        std::snprintf(buf, sizeof buf, "%lluh%02llum",
                      static_cast<unsigned long long>(total / 3600),
                      static_cast<unsigned long long>((total % 3600) / 60));
    }
    return buf;
}

uint64_t parse_bytes(std::string_view s) {
    std::string_view t = trim(s);
    if (t.empty()) throw Error("empty size value");

    size_t digits = 0;
    while (digits < t.size() &&
           (std::isdigit(static_cast<unsigned char>(t[digits])) || t[digits] == '.')) {
        ++digits;
    }
    if (digits == 0) throw Error("size must start with a number: '" + std::string(s) + "'");

    double value = 0;
    try {
        value = std::stod(std::string(t.substr(0, digits)));
    } catch (const std::exception&) {
        throw Error("bad size value: '" + std::string(s) + "'");
    }
    if (value < 0) throw Error("size must not be negative: '" + std::string(s) + "'");

    std::string suffix = lower(trim(t.substr(digits)));
    if (suffix == "b" || suffix.empty()) {
        // bytes
    } else if (suffix == "k" || suffix == "kb" || suffix == "kib") {
        value *= 1024.0;
    } else if (suffix == "m" || suffix == "mb" || suffix == "mib") {
        value *= 1024.0 * 1024.0;
    } else if (suffix == "g" || suffix == "gb" || suffix == "gib") {
        value *= 1024.0 * 1024.0 * 1024.0;
    } else if (suffix == "t" || suffix == "tb" || suffix == "tib") {
        value *= 1024.0 * 1024.0 * 1024.0 * 1024.0;
    } else {
        throw Error("unknown size suffix '" + suffix + "' in '" + std::string(s) + "'");
    }
    return static_cast<uint64_t>(value);
}

double parse_duration(std::string_view s) {
    std::string_view t = trim(s);
    if (t.empty()) throw Error("empty duration value");

    double total = 0;
    size_t i = 0;
    bool any = false;
    while (i < t.size()) {
        size_t start = i;
        while (i < t.size() &&
               (std::isdigit(static_cast<unsigned char>(t[i])) || t[i] == '.')) {
            ++i;
        }
        if (i == start) throw Error("bad duration: '" + std::string(s) + "'");
        double value = 0;
        try {
            value = std::stod(std::string(t.substr(start, i - start)));
        } catch (const std::exception&) {
            throw Error("bad duration: '" + std::string(s) + "'");
        }
        char unit = i < t.size() ? static_cast<char>(std::tolower(static_cast<unsigned char>(t[i]))) : 's';
        if (i < t.size()) ++i;
        switch (unit) {
            case 's': total += value; break;
            case 'm': total += value * 60; break;
            case 'h': total += value * 3600; break;
            case 'd': total += value * 86400; break;
            default:
                throw Error(std::string("unknown duration unit '") + unit + "' in '" +
                            std::string(s) + "'");
        }
        any = true;
    }
    if (!any) throw Error("bad duration: '" + std::string(s) + "'");
    return total;
}

double parse_cpu(std::string_view s) {
    std::string_view t = trim(s);
    if (t.empty()) throw Error("empty cpu value");
    bool percent = t.back() == '%';
    if (percent) t.remove_suffix(1);
    double value = 0;
    try {
        value = std::stod(std::string(t));
    } catch (const std::exception&) {
        throw Error("bad cpu value: '" + std::string(s) + "'");
    }
    if (percent) value /= 100.0;
    if (value <= 0) throw Error("cpu value must be positive: '" + std::string(s) + "'");
    return value;
}

std::string signal_name(int signum) {
    struct Entry {
        int number;
        const char* name;
    };
    static const Entry kSignals[] = {
        {SIGHUP, "SIGHUP"},   {SIGINT, "SIGINT"},   {SIGQUIT, "SIGQUIT"},
        {SIGILL, "SIGILL"},   {SIGTRAP, "SIGTRAP"}, {SIGABRT, "SIGABRT"},
        {SIGFPE, "SIGFPE"},   {SIGKILL, "SIGKILL"}, {SIGBUS, "SIGBUS"},
        {SIGSEGV, "SIGSEGV"}, {SIGSYS, "SIGSYS"},   {SIGPIPE, "SIGPIPE"},
        {SIGALRM, "SIGALRM"}, {SIGTERM, "SIGTERM"}, {SIGURG, "SIGURG"},
        {SIGSTOP, "SIGSTOP"}, {SIGTSTP, "SIGTSTP"}, {SIGCONT, "SIGCONT"},
        {SIGCHLD, "SIGCHLD"}, {SIGTTIN, "SIGTTIN"}, {SIGTTOU, "SIGTTOU"},
        {SIGXCPU, "SIGXCPU"}, {SIGXFSZ, "SIGXFSZ"}, {SIGVTALRM, "SIGVTALRM"},
        {SIGPROF, "SIGPROF"}, {SIGUSR1, "SIGUSR1"}, {SIGUSR2, "SIGUSR2"},
    };
    for (const Entry& e : kSignals) {
        if (e.number == signum) return e.name;
    }
    return "signal " + std::to_string(signum);
}

std::string sbpl_quote(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        // No SBPL escape for these; a path w/ one would change
        // the profile's meaning, so refuse.
        if (static_cast<unsigned char>(c) < 0x20) {
            throw Error("path contains a control character and cannot be expressed "
                        "in a sandbox profile");
        }
        out += c;
    }
    out += '"';
    return out;
}

std::string shell_quote(std::string_view s) {
    bool safe = !s.empty();
    for (char c : s) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '/' || c == '.' ||
              c == '_' || c == '-' || c == '=' || c == ':' || c == '+' || c == ',')) {
            safe = false;
            break;
        }
    }
    if (safe) return std::string(s);
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += '\'';
    return out;
}

bool match_glob(std::string_view pattern, std::string_view path, bool is_dir) {
    if (pattern.empty()) return false;

    if (pattern.front() == '/') pattern.remove_prefix(1);

    bool dir_only = false;
    if (pattern.back() == '/') {
        dir_only = true;
        pattern.remove_suffix(1);
    }
    if (dir_only && !is_dir) return false;

    std::string pat(pattern);
    std::string target(path);

    if (target == pat) return true;
    if (starts_with(target, pat + "/")) return true;

    if (pat.find('/') != std::string::npos) {
        if (fnmatch(pat.c_str(), target.c_str(), FNM_PATHNAME) == 0) return true;
        if (fnmatch((pat + "/*").c_str(), target.c_str(), FNM_PATHNAME) == 0) return true;
    } else {
        size_t last_slash = target.rfind('/');
        std::string base =
            (last_slash == std::string::npos) ? target : target.substr(last_slash + 1);
        if (fnmatch(pat.c_str(), base.c_str(), 0) == 0) return true;
        if (fnmatch(pat.c_str(), target.c_str(), 0) == 0) return true;
    }
    return false;
}

std::vector<std::string> load_ignore_file(const std::string& path) {
    std::vector<std::string> patterns;
    if (!fs::exists(path)) return patterns;
    try {
        std::string content = fs::read_file(path);
        for (std::string_view line : split(content, '\n')) {
            std::string_view trimmed = trim(line);
            if (trimmed.empty() || trimmed.front() == '#') continue;
            patterns.emplace_back(trimmed);
        }
    } catch (...) {
    }
    return patterns;
}

} // namespace isolate::str
