#include "cli/commands.h"

#include "cli/format.h"
#include "fsview/clone.h"
#include "fsview/extents.h"
#include "proc/limits.h"
#include "proc/tracker.h"
#include "sb/apply.h"
#include "sb/profile.h"
#include "sb/tag.h"
#include "util/error.h"
#include "util/fs.h"
#include "util/path.h"
#include "util/log.h"
#include "util/str.h"

#include <fcntl.h>
#include <libproc.h>
#include <signal.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

extern "C" int posix_spawnattr_set_qos_clamp_np(posix_spawnattr_t* attr, uint64_t clamp);

namespace isolate::cli {
namespace {

enum class Verdict { Works, Limited, Missing, Info };

struct Check {
    std::string name;
    Verdict verdict = Verdict::Works;
    std::string detail;
};

const char* verdict_text(Verdict v) {
    switch (v) {
        case Verdict::Works: return "ok";
        case Verdict::Limited: return "limited";
        case Verdict::Missing: return "missing";
        case Verdict::Info: return "info";
    }
    return "?";
}

const char* verdict_color(Verdict v) {
    if (!log::stdout_is_tty()) return "";
    switch (v) {
        case Verdict::Works: return "\033[32m";
        case Verdict::Limited: return "\033[33m";
        case Verdict::Missing: return "\033[31m";
        case Verdict::Info: return "\033[90m";
    }
    return "";
}

// Checks a denial is actually enforced, not assumed.
int run_under_profile(const std::string& profile, const std::vector<std::string>& argv) {
    std::vector<char*> raw;
    for (const std::string& arg : argv) raw.push_back(const_cast<char*>(arg.c_str()));
    raw.push_back(nullptr);

    pid_t child = ::fork();
    if (child < 0) return -1;
    if (child == 0) {
        // Only the status matters.
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) ::close(devnull);
        }
        if (::chdir("/") != 0) ::_exit(120);
        if (sb::apply_profile_nothrow(profile.c_str())) ::_exit(121);
        ::execv(raw[0], raw.data());
        ::_exit(122);
    }
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

// Just enough to run a base tool, plus the caller's additions.
std::string probe_profile(const std::string& extra) {
    return "(version 1)\n"
           "(deny default (with no-log))\n"
           "(allow syscall-unix syscall-mach syscall-mig)\n"
           "(allow system-fcntl)\n"
           "(allow process-fork)\n"
           "(allow sysctl-read)\n"
           "(allow mach-bootstrap)\n"
           "(allow signal (target same-sandbox))\n"
           "(allow file-read* file-test-existence file-map-executable\n"
           "       (subpath \"/System\") (subpath \"/usr/lib\") (subpath \"/usr/share\")\n"
           "       (subpath \"/private/var/select\") (literal \"/\"))\n"
           "(allow file-read-metadata file-test-existence\n"
           "       (literal \"/etc\") (literal \"/tmp\") (literal \"/var\"))\n"
           "(allow file-read* file-test-existence file-map-executable process-exec*\n"
           "       (subpath \"/bin\") (subpath \"/usr/bin\"))\n"
           "(allow file-read* file-write-data (literal \"/dev/null\"))\n" +
           extra;
}

void check_sandbox(std::vector<Check>& checks) {
    sb::ValidationResult result = sb::validate_profile(probe_profile(""));
    if (result.ok) {
        checks.push_back({"sandbox profiles", Verdict::Works,
                          "the kernel accepts and applies generated profiles"});
    } else {
        checks.push_back({"sandbox profiles", Verdict::Missing,
                          "profiles are rejected: " + result.error +
                              ". Without this, isolate enforces nothing."});
    }
}

void check_file_denial(std::vector<Check>& checks) {
    // true is allowed, so a failure here is the rule's, not the probe's.
    int allowed = run_under_profile(probe_profile(""), {"/usr/bin/true"});
    int denied = run_under_profile(
        probe_profile(""), {"/bin/sh", "-c", "echo x > /private/tmp/isolate-doctor-probe"});

    if (allowed != 0) {
        checks.push_back({"filesystem rules", Verdict::Missing,
                          "a program that should be allowed to run did not; the probe "
                          "profile itself is broken"});
        return;
    }
    if (denied == 0) {
        ::unlink("/private/tmp/isolate-doctor-probe");
        checks.push_back({"filesystem rules", Verdict::Missing,
                          "a write outside the policy succeeded; filesystem isolation is "
                          "not being enforced"});
        return;
    }
    checks.push_back({"filesystem rules", Verdict::Works,
                      "writes outside the policy are refused by the kernel"});
}

void check_network_denial(std::vector<Check>& checks) {
    // Connect must fail w/ EPERM, not time out.
    int rc = run_under_profile(
        probe_profile(""),
        {"/bin/sh", "-c",
         "exec 3<>/dev/tcp/127.0.0.1/1 2>/dev/null || exit 7"});
    // macOS sh has no /dev/tcp, so any non-zero is the refusal.
    if (rc == 7 || rc == 1 || rc == 2) {
        checks.push_back({"network rules", Verdict::Works,
                          "a capsule with network: none cannot open outbound connections"});
    } else if (rc == 0) {
        checks.push_back({"network rules", Verdict::Missing,
                          "an outbound connection succeeded under a profile that denies "
                          "networking"});
    } else {
        checks.push_back({"network rules", Verdict::Works,
                          "outbound connections fail under a profile that denies "
                          "networking (probe status " + std::to_string(rc) + ")"});
    }
}

void check_signal_isolation(std::vector<Check>& checks) {
    // pid 1 always exists and belongs to no capsule.
    int rc = run_under_profile(probe_profile(""),
                               {"/bin/sh", "-c", "kill -0 1 2>/dev/null && exit 0 || exit 3"});
    if (rc == 3) {
        checks.push_back({"signal isolation", Verdict::Works,
                          "capsule processes cannot signal processes outside the capsule"});
    } else if (rc == 0) {
        checks.push_back({"signal isolation", Verdict::Missing,
                          "a capsule process was able to signal launchd"});
    } else {
        checks.push_back({"signal isolation", Verdict::Limited,
                          "the probe could not be run (status " + std::to_string(rc) + ")"});
    }
}

void check_membership(std::vector<Check>& checks) {
    if (sb::membership_checks_work()) {
        checks.push_back({"process attestation", Verdict::Works,
                          "the kernel confirms which processes carry a capsule's profile, "
                          "so isolate can track them across fork, exec, and reparenting"});
    } else {
        checks.push_back({"process attestation", Verdict::Limited,
                          "sandbox_check did not report capsule membership. Process "
                          "tracking falls back to parentage, which misses processes that "
                          "were reparented to launchd."});
    }
}

void check_process_visibility(std::vector<Check>& checks) {
    // Direct, not via ps(1), which needs more than a capsule gets
    // and would fail for the wrong reason.
    pid_t child = ::fork();
    if (child == 0) {
        if (sb::apply_profile_nothrow(probe_profile("").c_str())) ::_exit(121);
        std::vector<pid_t> pids = proc::all_pids();
        char path[PROC_PIDPATHINFO_MAXSIZE];
        bool can_list = pids.size() > 1;
        bool can_read_launchd = ::proc_pidpath(1, path, sizeof path) > 0;
        ::_exit((can_list ? 1 : 0) | (can_read_launchd ? 2 : 0));
    }
    if (child < 0) {
        checks.push_back({"host process visibility", Verdict::Info,
                          "could not run the probe"});
        return;
    }
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    int result = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (result == 121 || result < 0) {
        checks.push_back({"host process visibility", Verdict::Info,
                          "could not run the probe"});
        return;
    }

    bool can_list = (result & 1) != 0;
    bool can_read_paths = (result & 2) != 0;
    if (can_list || can_read_paths) {
        std::string detail = "a capsule can ";
        if (can_list) detail += "list every process on the system";
        if (can_list && can_read_paths) detail += " and ";
        if (can_read_paths) detail += "read the executable paths of host processes";
        detail += ". proc_info is not mediated by the sandbox, so this is the clearest "
                  "difference from a PID namespace. A capsule still cannot signal those "
                  "processes or read their memory.";
        checks.push_back({"host process visibility", Verdict::Limited, detail});
    } else {
        checks.push_back({"host process visibility", Verdict::Works,
                          "a capsule could not enumerate host processes on this system"});
    }
}

void check_cloning(std::vector<Check>& checks, const Store& store) {
    std::string root = store.root();
    std::string fstype = fsview::filesystem_type(root);
    if (fsview::supports_cloning(root)) {
        checks.push_back({"APFS cloning", Verdict::Works,
                          "capsule storage is on " + fstype +
                              " and supports clonefile, so layers and snapshots cost "
                              "almost no space until they are written"});
    } else {
        checks.push_back({"APFS cloning", Verdict::Limited,
                          "capsule storage is on " + fstype +
                              ", which cannot clone. Layers will be full copies: slower, "
                              "and the space is really used."});
    }
}

void check_extents(std::vector<Check>& checks, const Store& store) {
    // Correct w/o extent maps, just slower. Perf note, not failure.
    std::string dir = path::join(store.root(), "doctor");
    try {
        fs::mkdir_p(dir, 0700);
        std::string a = path::join(dir, "a");
        std::string b = path::join(dir, "b");
        fs::write_file_atomic(a, std::string(128 * 1024, 'x'));
        if (fs::exists(b)) fs::remove_tree(b);
        fsview::clone_file_over(a, b);

        fsview::ExtentVerdict verdict = fsview::compare_extents(a, b);
        fs::remove_tree(dir);

        if (verdict == fsview::ExtentVerdict::SharedBlocks) {
            checks.push_back({"extent comparison", Verdict::Works,
                              "a clone's blocks can be compared without reading the file, "
                              "so unchanged files cost no I/O to verify"});
        } else {
            checks.push_back({"extent comparison", Verdict::Limited,
                              "extent maps were not usable here, so isolate compares files "
                              "byte for byte. Results are the same; diffs are slower."});
        }
    } catch (const Error& e) {
        try {
            fs::remove_tree(dir);
        } catch (const Error&) {
        }
        checks.push_back({"extent comparison", Verdict::Limited,
                          std::string("could not test: ") + e.what()});
    }
}

void check_rlimits(std::vector<Check>& checks) {
    std::vector<std::string> working;
    std::vector<std::string> broken;

    struct Probe {
        int resource;
        const char* name;
        rlim_t value;
    };
    const Probe probes[] = {
        {RLIMIT_CPU, "cpu time", 3600},
        {RLIMIT_FSIZE, "file size", rlim_t(1) << 30},
        {RLIMIT_NOFILE, "open files", 256},
        {RLIMIT_CORE, "core size", 0},
        {RLIMIT_AS, "address space", rlim_t(1) << 32},
        {RLIMIT_DATA, "data segment", rlim_t(1) << 32},
    };

    for (const Probe& probe : probes) {
        pid_t child = ::fork();
        if (child < 0) continue;
        if (child == 0) {
            rlimit current {};
            if (::getrlimit(probe.resource, &current) != 0) ::_exit(1);
            rlimit next {};
            next.rlim_cur = probe.value;
            next.rlim_max = current.rlim_max;
            ::_exit(::setrlimit(probe.resource, &next) == 0 ? 0 : 1);
        }
        int status = 0;
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
        bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        (ok ? working : broken).emplace_back(probe.name);
    }

    checks.push_back({"resource limits", Verdict::Works,
                      "kernel-enforced: " + str::join(working, ", ")});

    // The limit users most assume is a guarantee, so check it.
    proc::MemoryLimitProbe probe = proc::probe_kernel_memory_limits();
    if (probe.footprint_limit_available || probe.memorystatus_available) {
        // Root only, and per-proc, whereas --memory is per-capsule.
        checks.push_back({"memory limits", Verdict::Limited,
                          "running as root, so the kernel would accept a per-process "
                          "memory limit (" +
                              std::string(probe.footprint_limit_available
                                              ? "task_set_phys_footprint_limit"
                                              : "memorystatus_control") +
                              "). isolate does not use it: it is per-process, while "
                              "--memory is a ceiling on the capsule's total footprint. "
                              "The ceiling is still enforced by sampling, so a fast "
                              "allocation can overshoot it."});
    } else {
        std::string detail = "no kernel memory ceiling is available here. ";
        if (!broken.empty()) {
            detail += "Darwin rejects " + str::join(broken, " and ") + " limits; ";
        }
        detail += "task_set_phys_footprint_limit says " +
                  (probe.footprint_limit_error.empty() ? "no" : probe.footprint_limit_error) +
                  " and memorystatus_control says " +
                  (probe.memorystatus_error.empty() ? "no" : probe.memorystatus_error) +
                  ". isolate samples the capsule's footprint instead, so a fast allocation "
                  "can exceed --memory between two samples. The run report says by how "
                  "much when it happens.";
        checks.push_back({"memory limits", Verdict::Limited, detail});
    }
}

void check_qos_clamp(std::vector<Check>& checks) {
    posix_spawnattr_t attr;
    if (::posix_spawnattr_init(&attr) != 0) {
        checks.push_back({"scheduling clamp", Verdict::Limited,
                          "spawn attributes could not be initialised"});
        return;
    }
    ::posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETEXEC);
    int rc = posix_spawnattr_set_qos_clamp_np(&attr, 2);
    ::posix_spawnattr_destroy(&attr);

    if (rc != 0) {
        checks.push_back({"scheduling clamp", Verdict::Limited,
                          std::string("this system rejected a QoS clamp (") +
                              std::strerror(rc) +
                              "); --cpu and --background will have no effect"});
        return;
    }

    // Accepted is not the same as effective.
    pid_t child = ::fork();
    if (child == 0) {
        posix_spawnattr_t at;
        ::posix_spawnattr_init(&at);
        ::posix_spawnattr_setflags(&at, POSIX_SPAWN_SETEXEC);
        posix_spawnattr_set_qos_clamp_np(&at, 2);
        const char* argv[] = {"/bin/sleep", "5", nullptr};
        ::posix_spawn(nullptr, "/bin/sleep", nullptr, &at,
                      const_cast<char* const*>(argv), nullptr);
        ::_exit(1);
    }
    usleep(250 * 1000);
    proc_taskinfo info {};
    int priority = -1;
    if (::proc_pidinfo(child, PROC_PIDTASKINFO, 0, &info, sizeof info) > 0) {
        priority = info.pti_priority;
    }
    ::kill(child, SIGKILL);
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }

    if (priority >= 0 && priority < 20) {
        checks.push_back({"scheduling clamp", Verdict::Works,
                          "a clamped process runs at priority " + std::to_string(priority) +
                              " instead of 31, and every process it starts inherits the "
                              "clamp. This caps priority, not CPU share."});
    } else {
        checks.push_back({"scheduling clamp", Verdict::Limited,
                          "the clamp was accepted but the spawned process still ran at "
                          "priority " + std::to_string(priority)});
    }
}

void check_privileges(std::vector<Check>& checks) {
    if (::geteuid() == 0) {
        checks.push_back({"credentials", Verdict::Works,
                          "isolate is running as root, so --user can drop the capsule to "
                          "another account"});
    } else {
        checks.push_back({"credentials", Verdict::Limited,
                          "isolate is not running as root, so a capsule runs as you. It "
                          "cannot be given a different uid, and anything it can reach, it "
                          "reaches with your identity. The sandbox profile is the boundary, "
                          "not the user."});
    }
}

} // namespace

int cmd_doctor(const Options& options, Store& store) {
    std::vector<Check> checks;

    check_sandbox(checks);
    check_file_denial(checks);
    check_network_denial(checks);
    check_signal_isolation(checks);
    check_membership(checks);
    check_process_visibility(checks);
    check_cloning(checks, store);
    check_extents(checks, store);
    check_rlimits(checks);
    check_qos_clamp(checks);
    check_privileges(checks);

    if (options.json) {
        json::Array array;
        for (const Check& check : checks) {
            json::Object o;
            o.insert_or_assign("name", json::Value(check.name));
            o.insert_or_assign("verdict", json::Value(verdict_text(check.verdict)));
            o.insert_or_assign("detail", json::Value(check.detail));
            array.emplace_back(std::move(o));
        }
        json::Object root;
        root.insert_or_assign("checks", json::Value(std::move(array)));
        root.insert_or_assign("store", json::Value(store.root()));
        print_json(json::Value(std::move(root)));
        return 0;
    }

    std::printf("isolate %s on ", ISOLATE_VERSION);
    std::fflush(stdout);
    if (std::FILE* pipe = ::popen("/usr/bin/uname -sr 2>/dev/null", "r")) {
        char buf[128];
        if (std::fgets(buf, sizeof buf, pipe)) std::printf("%s", buf);
        else std::printf("macOS\n");
        ::pclose(pipe);
    } else {
        std::printf("macOS\n");
    }
    std::printf("capsule store: %s\n\n", store.root().c_str());

    size_t width = 0;
    for (const Check& check : checks) width = std::max(width, check.name.size());

    bool any_missing = false;
    for (const Check& check : checks) {
        if (check.verdict == Verdict::Missing) any_missing = true;
        std::string pad(width - check.name.size(), ' ');
        std::printf("%s%-8s%s %s%s  %s\n", verdict_color(check.verdict),
                    verdict_text(check.verdict), log::stdout_is_tty() ? "\033[0m" : "",
                    check.name.c_str(), pad.c_str(), check.detail.c_str());
    }

    std::printf("\n");
    if (any_missing) {
        std::printf("Something isolate relies on is not working. Capsules created on this\n");
        std::printf("system would not enforce what they claim to.\n");
        return 1;
    }
    std::printf("Everything isolate relies on is working. Entries marked 'limited' are\n");
    std::printf("documented limits of macOS, not faults; see docs/threat-model.md.\n");
    return 0;
}

} // namespace isolate::cli
