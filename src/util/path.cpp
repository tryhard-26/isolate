#include "util/path.h"

#include "util/error.h"
#include "util/str.h"

#include <mach-o/dyld.h>
#include <pwd.h>
#include <unistd.h>

#include <climits>
#include <cstdlib>
#include <vector>

namespace isolate::path {

std::string normalize(std::string_view p) {
    if (p.empty()) return ".";
    bool absolute_path = p.front() == '/';
    std::vector<std::string_view> parts;
    size_t i = 0;
    while (i < p.size()) {
        while (i < p.size() && p[i] == '/') ++i;
        size_t start = i;
        while (i < p.size() && p[i] != '/') ++i;
        if (i == start) break;
        std::string_view comp = p.substr(start, i - start);
        if (comp == ".") continue;
        if (comp == "..") {
            if (!parts.empty() && parts.back() != "..") {
                parts.pop_back();
            } else if (!absolute_path) {
                parts.push_back(comp);
            }
            // ".." above "/" is "/", which is what the kernel does too.
            continue;
        }
        parts.push_back(comp);
    }

    std::string out;
    if (absolute_path) out += '/';
    for (size_t k = 0; k < parts.size(); ++k) {
        if (k) out += '/';
        out += parts[k];
    }
    if (out.empty()) out = absolute_path ? "/" : ".";
    return out;
}

std::string absolute(std::string_view p, std::string_view base) {
    if (!p.empty() && p.front() == '/') return normalize(p);

    std::string prefix;
    if (!base.empty()) {
        prefix = std::string(base);
    } else {
        char buf[PATH_MAX];
        if (!getcwd(buf, sizeof buf)) throw_errno("getcwd");
        prefix = buf;
    }
    if (p.empty()) return normalize(prefix);
    return normalize(prefix + "/" + std::string(p));
}

std::string resolve(std::string_view p) {
    std::string in(p);
    char buf[PATH_MAX];
    if (!realpath(in.c_str(), buf)) throw Error("cannot resolve '" + in + "'", errno);
    return buf;
}

std::string resolve_lenient(std::string_view p) {
    std::string abs = absolute(p);
    std::string remainder;
    std::string probe = abs;
    while (true) {
        char buf[PATH_MAX];
        if (realpath(probe.c_str(), buf)) {
            std::string resolved = buf;
            if (remainder.empty()) return resolved;
            return normalize(resolved + "/" + remainder);
        }
        if (probe == "/" || probe.empty()) return abs;
        std::string leaf = basename(probe);
        remainder = remainder.empty() ? leaf : leaf + "/" + remainder;
        probe = dirname(probe);
    }
}

std::string dirname(std::string_view p) {
    std::string n = normalize(p);
    if (n == "/") return "/";
    size_t slash = n.rfind('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return n.substr(0, slash);
}

std::string basename(std::string_view p) {
    std::string n = normalize(p);
    if (n == "/") return "/";
    size_t slash = n.rfind('/');
    return slash == std::string::npos ? n : n.substr(slash + 1);
}

std::string join(std::string_view a, std::string_view b) {
    if (a.empty()) return normalize(b);
    if (b.empty()) return normalize(a);
    if (b.front() == '/') return normalize(b);
    return normalize(std::string(a) + "/" + std::string(b));
}

bool is_within(std::string_view parent, std::string_view child) {
    if (parent == child) return true;
    if (parent == "/") return !child.empty() && child.front() == '/';
    if (child.size() <= parent.size()) return false;
    if (child.compare(0, parent.size(), parent) != 0) return false;
    return child[parent.size()] == '/';
}

std::string relative_to(std::string_view parent, std::string_view child) {
    if (!is_within(parent, child)) {
        throw Error("'" + std::string(child) + "' is not inside '" + std::string(parent) + "'");
    }
    if (parent == child) return ".";
    size_t skip = parent == "/" ? 1 : parent.size() + 1;
    return std::string(child.substr(skip));
}

std::vector<std::string> ancestors(std::string_view p) {
    std::string n = normalize(p);
    std::vector<std::string> out;
    if (n.empty() || n.front() != '/') return out;
    std::string cur = "/";
    if (n == "/") return out;
    out.push_back("/");
    size_t i = 1;
    while (true) {
        size_t slash = n.find('/', i);
        if (slash == std::string::npos) break;
        cur = n.substr(0, slash);
        out.push_back(cur);
        i = slash + 1;
    }
    return out;
}

std::string home_dir() {
    if (const char* h = std::getenv("HOME"); h && *h) return normalize(h);
    if (const passwd* pw = getpwuid(getuid()); pw && pw->pw_dir) return normalize(pw->pw_dir);
    throw Error("cannot determine the home directory; set HOME");
}

std::string state_dir() {
    if (const char* h = std::getenv("ISOLATE_HOME"); h && *h) return absolute(h);
    return join(home_dir(), ".local/share/isolate");
}

std::string self_executable() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size ? size : PATH_MAX, '\0');
    uint32_t cap = static_cast<uint32_t>(buf.size());
    if (_NSGetExecutablePath(buf.data(), &cap) != 0) {
        throw Error("cannot determine the path of the running executable");
    }
    buf.resize(std::strlen(buf.c_str()));
    return resolve_lenient(buf);
}

} // namespace isolate::path
