#include "proc/limits.h"

#include "util/str.h"

#include <mach/mach.h>
#include <mach/task.h>

#include <grp.h>
#include <pwd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

// Private Jetsam iface for memlimits.
extern "C" int memorystatus_control(uint32_t command, int32_t pid, uint32_t flags,
                                    void* buffer, size_t buffersize);

namespace isolate::proc {
namespace {

// XNU kern_memorystatus.h defs.
constexpr uint32_t kMemorystatusSetMemlimitProperties = 7;

struct memorystatus_memlimit_properties {
    int32_t memlimit_active;
    uint32_t memlimit_active_attr;
    int32_t memlimit_inactive;
    uint32_t memlimit_inactive_attr;
};

// QoS clamps from posix_spawnattr.
constexpr int kClampNone = 0;
constexpr int kClampUtility = 1;
constexpr int kClampBackground = 2;
constexpr int kClampMaintenance = 3;

char g_error[256];

const char* fail(const char* what, int err) {
    std::snprintf(g_error, sizeof g_error, "%s: %s", what, std::strerror(err));
    return g_error;
}

bool set_limit(int resource, rlim_t value, const char** error, const char* name) {
    rlimit current {};
    if (::getrlimit(resource, &current) != 0) {
        *error = fail(name, errno);
        return false;
    }
    // Cannot raise hard limit without root; clamp to hard max.
    rlim_t hard = current.rlim_max;
    if (hard != RLIM_INFINITY && value > hard) value = hard;

    rlimit next {};
    next.rlim_cur = value;
    next.rlim_max = hard == RLIM_INFINITY ? value : hard;
    if (::setrlimit(resource, &next) != 0) {
        *error = fail(name, errno);
        return false;
    }
    return true;
}

} // namespace

const char* apply_rlimits_nothrow(const Limits& limits) {
    const char* error = nullptr;

    if (limits.cpu_seconds.has_value()) {
        // Round up cpu seconds.
        rlim_t seconds = static_cast<rlim_t>(*limits.cpu_seconds);
        if (static_cast<double>(seconds) < *limits.cpu_seconds) ++seconds;
        if (seconds == 0) seconds = 1;
        if (!set_limit(RLIMIT_CPU, seconds, &error, "cpu time limit")) return error;
    }

    if (limits.file_bytes.has_value()) {
        if (!set_limit(RLIMIT_FSIZE, static_cast<rlim_t>(*limits.file_bytes), &error,
                       "file size limit")) {
            return error;
        }
    }

    if (limits.open_files.has_value()) {
        if (!set_limit(RLIMIT_NOFILE, static_cast<rlim_t>(*limits.open_files), &error,
                       "open file limit")) {
            return error;
        }
    }

    if (!limits.core_dumps) {
        // Disable core dumps via rlimit.
        rlimit no_cores {};
        no_cores.rlim_cur = 0;
        no_cores.rlim_max = 0;
        ::setrlimit(RLIMIT_CORE, &no_cores);
    }

    return nullptr;
}

const char* apply_credentials_nothrow(const Credentials& credentials) {
    ::umask(static_cast<mode_t>(credentials.umask));

    if (credentials.user.empty()) return nullptr;

    if (::geteuid() != 0) {
        std::snprintf(g_error, sizeof g_error,
                      "cannot run as user '%s': isolate is not running as root",
                      credentials.user.c_str());
        return g_error;
    }

    const passwd* pw = ::getpwnam(credentials.user.c_str());
    if (!pw) {
        std::snprintf(g_error, sizeof g_error, "no such user '%s'",
                      credentials.user.c_str());
        return g_error;
    }

    // Drop creds: groups and gid first, setuid last.
    if (::initgroups(pw->pw_name, static_cast<int>(pw->pw_gid)) != 0) {
        return fail("initgroups", errno);
    }
    if (::setgid(pw->pw_gid) != 0) return fail("setgid", errno);
    if (::setuid(pw->pw_uid) != 0) return fail("setuid", errno);

    // Verify privilege drop.
    if (::getuid() != pw->pw_uid || ::geteuid() != pw->pw_uid) {
        std::snprintf(g_error, sizeof g_error, "dropping privileges to '%s' did not take effect",
                      credentials.user.c_str());
        return g_error;
    }
    if (::setuid(0) == 0) {
        std::snprintf(g_error, sizeof g_error,
                      "privileges could be regained after dropping to '%s'",
                      credentials.user.c_str());
        return g_error;
    }
    return nullptr;
}

int qos_clamp_for(const Limits& limits) {
    if (limits.background_priority) return kClampBackground;
    if (!limits.cpu_multiple.has_value()) return kClampNone;

    // <1 core uses background clamp; >=1 uses utility clamp.
    return *limits.cpu_multiple < 1.0 ? kClampBackground : kClampUtility;
}

const char* qos_clamp_name(int clamp) {
    switch (clamp) {
        case kClampNone: return "none";
        case kClampUtility: return "utility";
        case kClampBackground: return "background";
        case kClampMaintenance: return "maintenance";
        default: return "unknown";
    }
}

MemoryLimitProbe probe_kernel_memory_limits() {
    MemoryLimitProbe probe;

    // Run probe in child proc.
    int pipefd[2];
    if (::pipe(pipefd) != 0) {
        probe.footprint_limit_error = std::strerror(errno);
        probe.memorystatus_error = probe.footprint_limit_error;
        return probe;
    }

    pid_t child = ::fork();
    if (child < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        probe.footprint_limit_error = "could not fork a probe";
        probe.memorystatus_error = probe.footprint_limit_error;
        return probe;
    }

    if (child == 0) {
        ::close(pipefd[0]);
        int previous = 0;
        kern_return_t kr =
            ::task_set_phys_footprint_limit(mach_task_self(), 256, &previous);

        memorystatus_memlimit_properties properties {};
        properties.memlimit_active = 256;
        properties.memlimit_inactive = 256;
        errno = 0;
        int ms = memorystatus_control(kMemorystatusSetMemlimitProperties, ::getpid(), 0,
                                      &properties, sizeof properties);
        int ms_errno = errno;

        char buf[256];
        int len = std::snprintf(buf, sizeof buf, "%d|%s|%d|%s", kr,
                                kr == KERN_SUCCESS ? "" : mach_error_string(kr), ms,
                                ms == 0 ? "" : std::strerror(ms_errno));
        ssize_t ignored = ::write(pipefd[1], buf, static_cast<size_t>(len));
        (void)ignored;
        ::_exit(0);
    }

    ::close(pipefd[1]);
    std::string answer;
    char buf[64];
    while (true) {
        ssize_t n = ::read(pipefd[0], buf, sizeof buf);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        answer.append(buf, static_cast<size_t>(n));
    }
    ::close(pipefd[0]);
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }

    // "kr|krtext|ms|mstext"
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        size_t bar = answer.find('|', start);
        if (bar == std::string::npos) {
            parts.push_back(answer.substr(start));
            break;
        }
        parts.push_back(answer.substr(start, bar - start));
        start = bar + 1;
    }
    if (parts.size() >= 4) {
        probe.footprint_limit_available = parts[0] == "0";
        probe.footprint_limit_error = parts[1];
        probe.memorystatus_available = parts[2] == "0";
        probe.memorystatus_error = parts[3];
    } else {
        probe.footprint_limit_error = "the probe did not report back";
        probe.memorystatus_error = probe.footprint_limit_error;
    }
    return probe;
}

std::vector<LimitNote> describe(const Limits& limits) {
    std::vector<LimitNote> notes;

    if (limits.memory_bytes.has_value()) {
        notes.push_back({"memory", str::human_bytes(*limits.memory_bytes), false,
                         "supervisor samples the capsule's total footprint; Darwin has no "
                         "working rlimit for address space"});
    }
    if (limits.cpu_multiple.has_value() || limits.background_priority) {
        int clamp = qos_clamp_for(limits);
        notes.push_back({"cpu priority", qos_clamp_name(clamp), true,
                         "QoS clamp applied at spawn and inherited by descendants; caps "
                         "priority, not CPU share"});
    }
    if (limits.cpu_seconds.has_value()) {
        notes.push_back({"cpu time", str::human_duration(*limits.cpu_seconds), true,
                         "RLIMIT_CPU; SIGXCPU then SIGKILL"});
    }
    if (limits.max_processes.has_value()) {
        notes.push_back({"processes", std::to_string(*limits.max_processes), false,
                         "supervisor counts capsule members; RLIMIT_NPROC is per-uid and "
                         "would affect the whole login session"});
    }
    if (limits.wall_seconds.has_value()) {
        notes.push_back({"wall time", str::human_duration(*limits.wall_seconds), false,
                         "supervisor timer"});
    }
    if (limits.file_bytes.has_value()) {
        notes.push_back({"file size", str::human_bytes(*limits.file_bytes), true,
                         "RLIMIT_FSIZE; SIGXFSZ on a write past the limit"});
    }
    if (limits.open_files.has_value()) {
        notes.push_back({"open files", std::to_string(*limits.open_files), true,
                         "RLIMIT_NOFILE"});
    }
    if (!limits.core_dumps) {
        notes.push_back({"core dumps", "disabled", true, "RLIMIT_CORE set to 0"});
    }
    return notes;
}

} // namespace isolate::proc
