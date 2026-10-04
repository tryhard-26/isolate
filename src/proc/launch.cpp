#include "proc/launch.h"

#include "proc/limits.h"
#include "sb/apply.h"
#include "util/error.h"
#include "util/fs.h"
#include "util/log.h"
#include "util/path.h"
#include "util/str.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <crt_externs.h>
#include <mach-o/dyld.h>
#include <sys/ioctl.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

// QoS clamp API (spawn_private.h)
extern "C" int posix_spawnattr_set_qos_clamp_np(posix_spawnattr_t* attr, uint64_t clamp);

namespace isolate::proc {
namespace {

// Async-sig-safe child err report & exit
[[noreturn]] void child_fail(int report_fd, const char* stage, const char* detail) {
    char buf[512];
    size_t len = 0;
    auto append = [&](const char* s) {
        while (*s && len + 1 < sizeof buf) buf[len++] = *s++;
    };
    append(stage);
    if (detail && *detail) {
        append(": ");
        append(detail);
    }
    ssize_t ignored = ::write(report_fd, buf, len);
    (void)ignored;
    ::_exit(127);
}

// Spawn attr wrapper
class SpawnAttr {
public:
    SpawnAttr() {
        if (::posix_spawnattr_init(&attr_) != 0) {
            throw Error("cannot initialise spawn attributes", errno);
        }
        valid_ = true;
    }
    SpawnAttr(const SpawnAttr&) = delete;
    SpawnAttr& operator=(const SpawnAttr&) = delete;
    ~SpawnAttr() {
        if (valid_) ::posix_spawnattr_destroy(&attr_);
    }

    posix_spawnattr_t* get() { return &attr_; }

private:
    posix_spawnattr_t attr_ {};
    bool valid_ = false;
};

class ArgvBlock {
public:
    explicit ArgvBlock(const std::vector<std::string>& values) {
        pointers_.reserve(values.size() + 1);
        for (const std::string& v : values) pointers_.push_back(const_cast<char*>(v.c_str()));
        pointers_.push_back(nullptr);
    }

    char* const* data() const { return pointers_.data(); }

private:
    std::vector<char*> pointers_;
};

} // namespace

std::string find_shim_library() {
    if (const char* env = std::getenv("ISOLATE_SHIM_PATH")) {
        if (*env && fs::exists(env)) return path::resolve_lenient(env);
    }
#ifdef ISOLATE_SHIM_PATH
    if (fs::exists(ISOLATE_SHIM_PATH)) {
        return path::resolve_lenient(ISOLATE_SHIM_PATH);
    }
#endif
    char exe_buf[1024];
    uint32_t buf_size = sizeof(exe_buf);
    if (_NSGetExecutablePath(exe_buf, &buf_size) == 0) {
        std::string exe_path = path::resolve_lenient(exe_buf);
        std::string dir = path::dirname(exe_path);

        std::string candidate1 = path::join(dir, "libisolate_shim.dylib");
        if (fs::exists(candidate1)) return candidate1;

        std::string candidate2 = path::join(dir, "../lib/libisolate_shim.dylib");
        if (fs::exists(candidate2)) return path::resolve_lenient(candidate2);
    }

    if (fs::exists("/usr/local/lib/libisolate_shim.dylib")) {
        return "/usr/local/lib/libisolate_shim.dylib";
    }

    return {};
}

std::vector<std::string> build_environment(const Spec& spec, const Capsule& capsule) {
    std::vector<std::string> out;
    auto set = [&out](const std::string& name, const std::string& value) {
        std::string prefix = name + "=";
        for (std::string& existing : out) {
            if (str::starts_with(existing, prefix)) {
                existing = prefix + value;
                return;
            }
        }
        out.push_back(prefix + value);
    };

    if (spec.env.inherit) {
        // _NSGetEnviron is how a dylib-safe program reaches environ on Darwin.
        char** env = *_NSGetEnviron();
        for (char** e = env; e && *e; ++e) out.emplace_back(*e);
    } else {
        for (const std::string& name : spec.env.passthrough) {
            if (const char* value = std::getenv(name.c_str())) set(name, value);
        }
    }

    // A capsule gets a usable baseline even with an empty inherited
    // environment, or nothing that reads PATH will start.
    set("PATH", "/usr/bin:/bin:/usr/sbin:/sbin");

    if (spec.private_home) {
        set("HOME", capsule.home_dir());
    } else if (!spec.env.inherit) {
        if (const char* home = std::getenv("HOME")) set("HOME", home);
    }

    if (spec.private_tmp) {
        // The trailing slash matches what the system sets, and some tools
        // concatenate without checking.
        set("TMPDIR", capsule.tmp_dir() + "/");
    }

    set("ISOLATE_CAPSULE", spec.id);
    if (!spec.name.empty()) set("ISOLATE_CAPSULE_NAME", spec.name);

    if (spec.redirect_paths) {
        std::string shim = find_shim_library();
        if (!shim.empty()) {
            set("DYLD_INSERT_LIBRARIES", shim);
            set("DYLD_FORCE_FLAT_NAMESPACE", "1");
        } else {
            log::warn("path virtualization requested (--redirect-paths), but libisolate_shim.dylib was not found");
        }

        std::string map_str;
        std::string first_from;
        std::string first_to;
        for (const Mount& m : spec.mounts) {
            if (m.storage == Storage::Clone && !m.host.empty() && !m.guest.empty() && m.host != m.guest) {
                if (!map_str.empty()) map_str += ":";
                map_str += m.host + "=" + m.guest;
                if (first_from.empty()) {
                    first_from = m.host;
                    first_to = m.guest;
                }
            }
        }
        if (!map_str.empty()) {
            set("ISOLATE_REDIRECT_MAP", map_str);
            set("ISOLATE_REDIRECT_FROM", first_from);
            set("ISOLATE_REDIRECT_TO", first_to);
        }
    }

    // Overrides come last so the user's --env always wins.
    for (const auto& [name, value] : spec.env.set) set(name, value);

    return out;
}

std::string resolve_program(const std::string& name, const std::vector<std::string>& envp) {
    if (name.find('/') != std::string::npos) {
        std::string resolved = path::resolve_lenient(name);
        if (!fs::exists(resolved)) {
            throw Error("cannot run '" + name + "': no such file");
        }
        return resolved;
    }

    std::string search = "/usr/bin:/bin:/usr/sbin:/sbin";
    for (const std::string& entry : envp) {
        if (str::starts_with(entry, "PATH=")) {
            search = entry.substr(5);
            break;
        }
    }

    for (const std::string& dir : str::split(search, ':')) {
        if (dir.empty()) continue;
        std::string candidate = path::join(dir, name);
        struct stat st {};
        if (::stat(candidate.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
            ::access(candidate.c_str(), X_OK) == 0) {
            return path::resolve_lenient(candidate);
        }
    }
    throw Error("cannot run '" + name + "': not found in the capsule's PATH (" + search + ")");
}

pid_t launch(const LaunchRequest& request) {
    if (request.argv.empty()) throw Error("nothing to run");

    // Everything below must be ready before fork: no allocation in the child.
    ArgvBlock argv(request.argv);
    ArgvBlock envp(request.envp);
    const char* program = request.argv.front().c_str();
    const char* cwd = request.cwd.empty() ? nullptr : request.cwd.c_str();
    const char* profile = request.profile.c_str();
    const Limits limits = request.limits;
    const Credentials credentials = request.credentials;
    const bool new_session = request.new_session;

    // SETEXEC replaces proc w/ QoS clamp applied
    SpawnAttr attr;
    if (::posix_spawnattr_setflags(attr.get(), POSIX_SPAWN_SETEXEC) != 0) {
        throw Error("cannot set spawn flags", errno);
    }
    const int clamp = qos_clamp_for(limits);
    if (clamp != 0) {
        if (int rc = posix_spawnattr_set_qos_clamp_np(attr.get(),
                                                      static_cast<uint64_t>(clamp));
            rc != 0) {
            // Non-fatal fallback
            log::warn(std::string("this system rejected the scheduling clamp (") +
                      std::strerror(rc) + "); the capsule will run at normal priority");
            if (::posix_spawnattr_setflags(attr.get(), POSIX_SPAWN_SETEXEC) != 0) {
                throw Error("cannot set spawn flags", errno);
            }
        }
    }

    int report[2];
    if (::pipe(report) != 0) throw_errno("cannot create the status pipe");
    if (::fcntl(report[1], F_SETFD, FD_CLOEXEC) != 0) {
        int saved = errno;
        ::close(report[0]);
        ::close(report[1]);
        throw Error("cannot configure the status pipe", saved);
    }

    pid_t child = ::fork();
    if (child < 0) {
        int saved = errno;
        ::close(report[0]);
        ::close(report[1]);
        throw Error("cannot fork", saved);
    }

    if (child == 0) {
        ::close(report[0]);

        if (request.stdin_fd >= 0) {
            if (::dup2(request.stdin_fd, STDIN_FILENO) < 0) {
                child_fail(report[1], "cannot redirect stdin", std::strerror(errno));
            }
        }
        if (request.stdout_fd >= 0) {
            if (::dup2(request.stdout_fd, STDOUT_FILENO) < 0) {
                child_fail(report[1], "cannot redirect stdout", std::strerror(errno));
            }
        }
        if (request.stderr_fd >= 0) {
            if (::dup2(request.stderr_fd, STDERR_FILENO) < 0) {
                child_fail(report[1], "cannot redirect stderr", std::strerror(errno));
            }
        }

        if (new_session) {
            if (::setsid() < 0) {
                child_fail(report[1], "cannot create a new session", std::strerror(errno));
            }
            if (request.stdin_fd >= 0 && ::isatty(STDIN_FILENO) == 1) {
                ::ioctl(STDIN_FILENO, TIOCSCTTY, 0);
            }
        }

        if (request.stdin_fd >= 3) ::close(request.stdin_fd);
        if (request.stdout_fd >= 3 && request.stdout_fd != request.stdin_fd) ::close(request.stdout_fd);
        if (request.stderr_fd >= 3 && request.stderr_fd != request.stdin_fd && request.stderr_fd != request.stdout_fd) ::close(request.stderr_fd);

        // Reset sigmask post-fork
        sigset_t empty;
        sigemptyset(&empty);
        ::sigprocmask(SIG_SETMASK, &empty, nullptr);
        for (int signum : {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGPIPE, SIGXCPU, SIGXFSZ}) {
            ::signal(signum, SIG_DFL);
        }

        if (cwd && ::chdir(cwd) != 0) {
            child_fail(report[1], "cannot enter the working directory", std::strerror(errno));
        }

        if (const char* error = apply_credentials_nothrow(credentials)) {
            child_fail(report[1], "cannot apply the credential policy", error);
        }

        if (const char* error = apply_rlimits_nothrow(limits)) {
            child_fail(report[1], "cannot apply resource limits", error);
        }

        // Apply sb profile last
        if (const char* error = sb::apply_profile_nothrow(profile)) {
            child_fail(report[1], "cannot apply the sandbox profile", error);
        }

        // Returns only on failure, having replaced this process on success.
        int rc = ::posix_spawn(nullptr, program, nullptr, attr.get(),
                               const_cast<char* const*>(argv.data()),
                               const_cast<char* const*>(envp.data()));
        child_fail(report[1], "cannot execute the program", std::strerror(rc));
    }

    ::close(report[1]);
    std::string message;
    char buf[256];
    while (true) {
        ssize_t n = ::read(report[0], buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        message.append(buf, static_cast<size_t>(n));
    }
    ::close(report[0]);

    if (!message.empty()) {
        int status = 0;
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
        throw Error(message);
    }

    return child;
}

} // namespace isolate::proc
