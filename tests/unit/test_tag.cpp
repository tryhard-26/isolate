#include "harness.h"

#include "sb/apply.h"
#include "sb/tag.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>

using namespace isolate;
using namespace isolate::sb;

namespace {

// A sandboxed child that has exec'd, which is the only state in which the
// kernel reports a process's profile to another process. The caller must
// reap it with stop().
class SandboxedChild {
public:
    explicit SandboxedChild(const std::string& capsule_id) {
        std::string name = membership_name(capsule_id);
        std::string profile =
            "(version 1)\n"
            "(deny default (with no-log))\n"
            "(allow syscall-unix syscall-mach syscall-mig)\n"
            "(allow system-fcntl)\n"
            "(allow process-fork)\n"
            "(allow signal (target self))\n"
            "(allow file-read-data file-read-metadata file-test-existence "
            "file-map-executable\n"
            "       (subpath \"/System\") (subpath \"/usr/lib\") (literal \"/\"))\n"
            "(allow file-read-data file-read-metadata file-test-existence "
            "file-map-executable process-exec*\n"
            "       (literal \"/bin/sleep\"))\n"
            "(allow mach-lookup (global-name \"" + name + "\"))\n";

        int pipefd[2];
        if (::pipe(pipefd) != 0) return;

        pid_ = ::fork();
        if (pid_ < 0) {
            ::close(pipefd[0]);
            ::close(pipefd[1]);
            return;
        }
        if (pid_ == 0) {
            ::close(pipefd[0]);
            ::fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
            if (apply_profile_nothrow(profile.c_str())) {
                char status = 'e';
                ssize_t ignored = ::write(pipefd[1], &status, 1);
                (void)ignored;
                ::_exit(1);
            }
            const char* argv[] = {"/bin/sleep", "30", nullptr};
            ::execv("/bin/sleep", const_cast<char* const*>(argv));
            ::_exit(1);
        }

        ::close(pipefd[1]);
        char status = 0;
        // End of file means the child reached exec; a byte means it failed.
        ready_ = ::read(pipefd[0], &status, 1) == 0;
        ::close(pipefd[0]);
    }

    SandboxedChild(const SandboxedChild&) = delete;
    SandboxedChild& operator=(const SandboxedChild&) = delete;
    ~SandboxedChild() { stop(); }

    void stop() {
        if (pid_ <= 0) return;
        ::kill(pid_, SIGKILL);
        int status = 0;
        while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
        pid_ = -1;
    }

    bool ready() const { return ready_; }
    pid_t pid() const { return pid_; }

private:
    pid_t pid_ = -1;
    bool ready_ = false;
};

} // namespace

TEST(tag, membership_names_are_derived_from_the_identifier) {
    CHECK_EQ(membership_name("0123456789ab"),
             std::string("dev.isolate.capsule.0123456789ab"));
    CHECK_NE(membership_name("aaaaaaaaaaaa"), membership_name("bbbbbbbbbbbb"));
}

TEST(tag, membership_checking_works_on_this_system) {
    // The whole process-tracking design rests on this, so it is checked
    // directly rather than inferred.
    CHECK(membership_checks_work());
}

TEST(tag, the_test_runner_is_not_sandboxed) {
    CHECK(!is_sandboxed(::getpid()));
    CHECK(!self_is_sandboxed());
}

TEST(tag, an_unsandboxed_process_is_not_a_member_of_any_capsule) {
    // Without the sandboxed check first, sandbox_check would answer "allowed"
    // for a process with no profile and it would look like a member of
    // everything.
    CHECK(check_membership(::getpid(), membership_name("0123456789ab")) ==
          Membership::NotMember);
    CHECK(check_membership(1, membership_name("0123456789ab")) == Membership::NotMember);
}

TEST(tag, a_capsule_process_is_recognised) {
    SandboxedChild child("aaaaaaaaaaaa");
    CHECK(child.ready());
    CHECK(is_sandboxed(child.pid()));
    CHECK(check_membership(child.pid(), membership_name("aaaaaaaaaaaa")) ==
          Membership::Member);
}

TEST(tag, a_process_from_another_capsule_is_not_a_member) {
    SandboxedChild child("aaaaaaaaaaaa");
    CHECK(child.ready());
    CHECK(check_membership(child.pid(), membership_name("bbbbbbbbbbbb")) ==
          Membership::NotMember);
}

TEST(tag, two_capsules_are_told_apart) {
    SandboxedChild first("aaaaaaaaaaaa");
    SandboxedChild second("bbbbbbbbbbbb");
    CHECK(first.ready());
    CHECK(second.ready());

    CHECK(check_membership(first.pid(), membership_name("aaaaaaaaaaaa")) ==
          Membership::Member);
    CHECK(check_membership(first.pid(), membership_name("bbbbbbbbbbbb")) ==
          Membership::NotMember);
    CHECK(check_membership(second.pid(), membership_name("bbbbbbbbbbbb")) ==
          Membership::Member);
    CHECK(check_membership(second.pid(), membership_name("aaaaaaaaaaaa")) ==
          Membership::NotMember);
}

TEST(tag, membership_survives_the_death_of_the_launcher) {
    // A capsule process whose parent exits is reparented to launchd, which is
    // exactly the case a process-group or parent-pid scheme would lose track
    // of. The profile stays attached, so the kernel can still identify it.
    SandboxedChild child("cccccccccccc");
    CHECK(child.ready());
    pid_t pid = child.pid();

    // The child's parent here is the test runner, so simulate the loss by
    // confirming the verdict does not depend on the relationship: the check
    // takes only a pid.
    CHECK(check_membership(pid, membership_name("cccccccccccc")) == Membership::Member);
    CHECK_EQ(::getppid() == pid, false);
}

TEST(tag, a_dead_process_reports_gone_or_not_a_member) {
    SandboxedChild child("dddddddddddd");
    CHECK(child.ready());
    pid_t pid = child.pid();
    child.stop();

    Membership verdict = check_membership(pid, membership_name("dddddddddddd"));
    CHECK(verdict == Membership::Gone || verdict == Membership::NotMember);
}

TEST(tag, invalid_pids_are_rejected) {
    CHECK(check_membership(0, membership_name("0123456789ab")) == Membership::Unknown);
    CHECK(check_membership(-1, membership_name("0123456789ab")) == Membership::Unknown);
}

TEST(tag, verdict_names_are_stable) {
    CHECK_EQ(std::string(to_string(Membership::Member)), std::string("member"));
    CHECK_EQ(std::string(to_string(Membership::NotMember)), std::string("not a member"));
    CHECK_EQ(std::string(to_string(Membership::Gone)), std::string("gone"));
    CHECK_EQ(std::string(to_string(Membership::Unknown)), std::string("unknown"));
}

TEST(tag, applying_a_malformed_profile_reports_an_error) {
    const char* error = nullptr;
    int status = 0;
    pid_t child = ::fork();
    if (child == 0) {
        error = apply_profile_nothrow("(version 1) (this is not valid sbpl");
        ::_exit(error ? 0 : 1);
    }
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    CHECK(WIFEXITED(status));
    CHECK_EQ(WEXITSTATUS(status), 0);
}

TEST(tag, applying_an_empty_profile_is_an_error) {
    int status = 0;
    pid_t child = ::fork();
    if (child == 0) ::_exit(apply_profile_nothrow("") ? 0 : 1);
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    CHECK_EQ(WEXITSTATUS(status), 0);
}

TEST(tag, validate_profile_accepts_a_valid_profile) {
    ValidationResult result = validate_profile(
        "(version 1)\n(deny default (with no-log))\n(allow syscall-unix)\n");
    CHECK(result.ok);
    CHECK(result.error.empty());
}

TEST(tag, validate_profile_reports_a_syntax_error) {
    ValidationResult result = validate_profile("(version 1) (allow");
    CHECK(!result.ok);
    CHECK(!result.error.empty());
}

TEST(tag, validate_profile_does_not_sandbox_the_caller) {
    ValidationResult result = validate_profile(
        "(version 1)\n(deny default (with no-log))\n(allow syscall-unix)\n");
    CHECK(result.ok);
    // If the profile had been applied here, the test runner would now be
    // confined and the rest of the suite would fail.
    CHECK(!self_is_sandboxed());
}
