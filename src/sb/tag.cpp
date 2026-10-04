#include "sb/tag.h"

#include "sb/apply.h"
#include "util/error.h"
#include "util/log.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>

// Private libsystem_sandbox symbol for sandbox validation.
extern "C" int sandbox_check(pid_t pid, const char* operation, int type, ...);

namespace isolate::sb {
namespace {

constexpr int kFilterNone = 0;
constexpr int kFilterGlobalName = 2;
// Suppress audit log violation on probe check.
constexpr int kNoReport = 0x40000000;

constexpr const char* kProbeId = "probeprobe000";

// Minimal alive test binary.
constexpr const char* kProbeProgram = "/bin/sleep";

} // namespace

const char* to_string(Membership m) {
    switch (m) {
        case Membership::Member: return "member";
        case Membership::NotMember: return "not a member";
        case Membership::Gone: return "gone";
        case Membership::Unknown: return "unknown";
    }
    return "?";
}

std::string membership_name(const std::string& capsule_id) {
    return "dev.isolate.capsule." + capsule_id;
}

bool is_sandboxed(pid_t pid) {
    // Check if proc has any active sb profile.
    return sandbox_check(pid, nullptr, kFilterNone) == 1;
}

Membership check_membership(pid_t pid, const std::string& name) {
    if (pid <= 0) return Membership::Unknown;

    errno = 0;
    int sandboxed = sandbox_check(pid, nullptr, kFilterNone);
    if (sandboxed < 0) {
        return errno == ESRCH ? Membership::Gone : Membership::Unknown;
    }
    if (sandboxed == 0) {
        // Unsandboxed procs pass everything; not a capsule member.
        return Membership::NotMember;
    }

    // Sandboxed procs without mach-lookup filtering (e.g. system helpers)
    // pass any query. Verify ungranted canary is refused first.
    constexpr const char* kCanary = "dev.isolate.canary.never_allowed";
    errno = 0;
    int canary = sandbox_check(pid, "mach-lookup", kFilterGlobalName | kNoReport, kCanary);
    if (canary == 0) {
        return Membership::NotMember;
    }
    if (canary < 0 && errno == ESRCH) {
        return Membership::Gone;
    }

    errno = 0;
    int allowed = sandbox_check(pid, "mach-lookup", kFilterGlobalName | kNoReport,
                                name.c_str());
    if (allowed < 0) {
        return errno == ESRCH ? Membership::Gone : Membership::Unknown;
    }
    // 0 == permitted via capsule profile tag rule.
    return allowed == 0 ? Membership::Member : Membership::NotMember;
}

bool membership_checks_work() {
    std::string name = membership_name(kProbeId);
    std::string other = membership_name("otherother00");

    // Child must exec to report its own profile rather than parent's.
    std::string profile =
        "(version 1)\n"
        "(deny default (with no-log))\n"
        "(allow syscall-unix syscall-mach syscall-mig)\n"
        "(allow system-fcntl)\n"
        "(allow process-fork)\n"
        "(allow signal (target self))\n"
        "(allow file-read* file-test-existence file-map-executable\n"
        "       (subpath \"/System\")\n"
        "       (subpath \"/usr/lib\")\n"
        "       (literal \"/\"))\n"
        "(allow file-read* file-test-existence file-map-executable process-exec*\n"
        "       (literal \"" + std::string(kProbeProgram) + "\"))\n"
        "(allow mach-lookup (global-name \"" + name + "\"))\n";

    int pipefd[2];
    if (::pipe(pipefd) != 0) return false;

    pid_t child = ::fork();
    if (child < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return false;
    }

    if (child == 0) {
        ::close(pipefd[0]);
        // Cloexec pipe: parent reads EOF on successful exec.
        ::fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
        const char* error = apply_profile_nothrow(profile.c_str());
        if (error) {
            char status = 'e';
            ssize_t ignored = ::write(pipefd[1], &status, 1);
            (void)ignored;
            ::_exit(1);
        }
        const char* args[] = {kProbeProgram, "30", nullptr};
        ::execv(kProbeProgram, const_cast<char* const*>(args));
        char status = 'x';
        ssize_t ignored = ::write(pipefd[1], &status, 1);
        (void)ignored;
        ::_exit(1);
    }

    ::close(pipefd[1]);
    char status = 0;
    ssize_t got = ::read(pipefd[0], &status, 1);
    ::close(pipefd[0]);

    // EOF indicates successful exec.
    bool ready = got == 0;

    bool works = false;
    if (ready) {
        works = check_membership(child, name) == Membership::Member &&
                check_membership(child, other) == Membership::NotMember &&
                check_membership(::getpid(), name) == Membership::NotMember;
    }

    ::kill(child, SIGKILL);
    int wait_status = 0;
    while (::waitpid(child, &wait_status, 0) < 0 && errno == EINTR) {
    }
    return works;
}

} // namespace isolate::sb
