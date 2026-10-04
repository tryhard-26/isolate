#include "harness.h"

#include "core/ids.h"
#include "proc/tracker.h"
#include "sb/apply.h"
#include "sb/tag.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <string>

using namespace isolate;
using namespace isolate::proc;

namespace {

// Starts a sandboxed process tree inside a capsule: a shell that spawns a
// number of sleeping children, so the tracker has something to find.
class CapsuleTree {
public:
    explicit CapsuleTree(int children)
        : CapsuleTree(generate_capsule_id(), children) {}

    CapsuleTree(const std::string& capsule_id, int children)
        : capsule_id_(capsule_id) {
        std::string name = sb::membership_name(capsule_id);
        std::string profile =
            "(version 1)\n"
            "(deny default (with no-log))\n"
            "(allow syscall-unix syscall-mach syscall-mig)\n"
            "(allow system-fcntl)\n"
            "(allow process-fork)\n"
            "(allow sysctl-read)\n"
            "(allow signal (target same-sandbox))\n"
            "(allow process-info* (target same-sandbox))\n"
            "(allow file-read-data file-read-metadata file-read-xattr file-test-existence "
            "file-map-executable\n"
            "       (subpath \"/System\") (subpath \"/usr/lib\") (subpath \"/usr/share\")\n"
            "       (subpath \"/private/var/select\") (literal \"/\"))\n"
            "(allow file-read-metadata file-test-existence\n"
            "       (literal \"/etc\") (literal \"/tmp\") (literal \"/var\"))\n"
            "(allow file-read-data file-read-metadata file-test-existence "
            "file-map-executable process-exec*\n"
            "       (subpath \"/bin\") (subpath \"/usr/bin\"))\n"
            "(allow file-read-data file-write-data (literal \"/dev/null\"))\n"
            "(allow mach-lookup (global-name \"" + name + "\"))\n";

        std::string script;
        for (int i = 0; i < children; ++i) script += "/bin/sleep 30 & ";
        script += "wait";

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
            ::setsid();
            // The profile does not grant the test runner's working directory,
            // and a shell that cannot read its cwd prints a warning.
            if (::chdir("/") != 0) ::_exit(1);
            if (sb::apply_profile_nothrow(profile.c_str())) {
                char status = 'e';
                ssize_t ignored = ::write(pipefd[1], &status, 1);
                (void)ignored;
                ::_exit(1);
            }
            const char* argv[] = {"/bin/sh", "-c", script.c_str(), nullptr};
            ::execv("/bin/sh", const_cast<char* const*>(argv));
            ::_exit(1);
        }

        ::close(pipefd[1]);
        char status = 0;
        ready_ = ::read(pipefd[0], &status, 1) == 0;
        ::close(pipefd[0]);
        // Give the shell time to start its children and for them to exec.
        if (ready_ && children > 0) usleep(500 * 1000);
    }

    CapsuleTree(const CapsuleTree&) = delete;
    CapsuleTree& operator=(const CapsuleTree&) = delete;
    ~CapsuleTree() { stop(); }

    void stop() {
        if (pid_ <= 0) return;
        // Kill the process group so the sleeps go too.
        ::kill(-pid_, SIGKILL);
        ::kill(pid_, SIGKILL);
        int status = 0;
        while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
        // Ensure child processes reparented to launchd have exited.
        Tracker tracker(sb::membership_name(capsule_id_), 0);
        for (int attempt = 0; attempt < 50; ++attempt) {
            std::vector<ProcInfo> lingering = tracker.scan();
            if (lingering.empty()) break;
            for (const ProcInfo& info : lingering) {
                ::kill(info.pid, SIGKILL);
            }
            usleep(10 * 1000);
        }
        pid_ = -1;
    }

    bool ready() const { return ready_; }
    pid_t pid() const { return pid_; }
    const std::string& capsule_id() const { return capsule_id_; }
    std::string membership_name() const { return sb::membership_name(capsule_id_); }

private:
    std::string capsule_id_;
    pid_t pid_ = -1;
    bool ready_ = false;
};

bool contains_pid(const std::vector<ProcInfo>& processes, pid_t pid) {
    return std::any_of(processes.begin(), processes.end(),
                       [pid](const ProcInfo& info) { return info.pid == pid; });
}

} // namespace

TEST(tracker, all_pids_returns_the_process_table) {
    std::vector<pid_t> pids = all_pids();
    CHECK(pids.size() > 10);
    CHECK(contains_pid({}, 0) == false);
    // The test runner itself must be in there, and no zero entries.
    CHECK(std::find(pids.begin(), pids.end(), ::getpid()) != pids.end());
    CHECK(std::find(pids.begin(), pids.end(), 0) == pids.end());
}

TEST(tracker, read_proc_info_describes_a_live_process) {
    ProcInfo info;
    CHECK(read_proc_info(::getpid(), info));
    CHECK_EQ(info.pid, ::getpid());
    CHECK_EQ(info.ppid, ::getppid());
    CHECK(!info.name.empty());
    CHECK(!info.path.empty());
    CHECK(info.footprint > 0);
    CHECK(info.started_at > 0);
    CHECK(info.thread_count >= 1);
}

TEST(tracker, read_proc_info_fails_for_a_pid_that_does_not_exist) {
    ProcInfo info;
    CHECK(!read_proc_info(0x7FFFFFF0, info));
}

TEST(tracker, read_proc_info_excludes_a_zombie) {
    // A zombie is still listed and still accepts kill(2). Counting one would
    // make isolate report that a capsule left a process behind when the
    // process had in fact finished.
    pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) ::_exit(0);

    // Wait for it to become a zombie without reaping it.
    ProcInfo info;
    bool became_zombie = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        usleep(10 * 1000);
        if (!read_proc_info(child, info)) {
            became_zombie = true;
            break;
        }
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    CHECK(became_zombie);
}

TEST(tracker, process_footprint_is_reported_for_our_own_process) {
    CHECK(process_footprint(::getpid()) > 0);
    CHECK_EQ(process_footprint(0x7FFFFFF0), uint64_t(0));
}

TEST(tracker, finds_every_process_in_a_capsule) {
    CapsuleTree tree(3);
    CHECK(tree.ready());

    Tracker tracker(tree.membership_name(), tree.pid());
    std::vector<ProcInfo> processes = tracker.scan();

    // The shell plus its three children.
    CHECK_EQ(processes.size(), size_t(4));
    CHECK(contains_pid(processes, tree.pid()));
    CHECK(tracker.attestation_working());
}

TEST(tracker, the_capsule_root_is_listed_first) {
    CapsuleTree tree(2);
    CHECK(tree.ready());

    Tracker tracker(tree.membership_name(), tree.pid());
    std::vector<ProcInfo> processes = tracker.scan();
    CHECK(!processes.empty());
    CHECK_EQ(processes[0].pid, tree.pid());
}

TEST(tracker, does_not_find_processes_from_another_capsule) {
    CapsuleTree mine(2);
    CapsuleTree theirs(2);
    CHECK(mine.ready());
    CHECK(theirs.ready());

    Tracker tracker(mine.membership_name(), mine.pid());
    std::vector<ProcInfo> processes = tracker.scan();

    CHECK(contains_pid(processes, mine.pid()));
    CHECK(!contains_pid(processes, theirs.pid()));
    CHECK_EQ(processes.size(), size_t(3));
}

TEST(tracker, does_not_find_host_processes) {
    CapsuleTree tree(1);
    CHECK(tree.ready());

    Tracker tracker(tree.membership_name(), tree.pid());
    std::vector<ProcInfo> processes = tracker.scan();

    // Neither launchd nor the test runner belongs to the capsule.
    CHECK(!contains_pid(processes, 1));
    CHECK(!contains_pid(processes, ::getpid()));
}

TEST(tracker, an_empty_capsule_has_no_processes) {
    Tracker tracker(sb::membership_name(generate_capsule_id()), 0);
    CHECK_EQ(tracker.scan().size(), size_t(0));
    CHECK(!tracker.attestation_working());
}

TEST(tracker, reports_the_evidence_for_each_process) {
    CapsuleTree tree(2);
    CHECK(tree.ready());

    Tracker tracker(tree.membership_name(), tree.pid());
    for (const ProcInfo& info : tracker.scan()) {
        // Everything has exec'd by now, so the kernel can attest to all of it.
        CHECK(info.evidence == Evidence::Attested);
    }
}

TEST(tracker, totals_are_summed_over_the_capsule) {
    CapsuleTree tree(2);
    CHECK(tree.ready());

    Tracker tracker(tree.membership_name(), tree.pid());
    std::vector<ProcInfo> processes = tracker.scan();

    uint64_t expected = 0;
    for (const ProcInfo& info : processes) expected += info.footprint;
    CHECK_EQ(tracker.total_footprint(), expected);
    CHECK_EQ(tracker.count(), processes.size());
    CHECK(tracker.total_cpu_seconds() >= 0);
}

TEST(tracker, repeated_scans_are_stable) {
    CapsuleTree tree(2);
    CHECK(tree.ready());

    Tracker tracker(tree.membership_name(), tree.pid());
    size_t first = tracker.scan().size();
    size_t second = tracker.scan().size();
    size_t third = tracker.scan().size();
    CHECK_EQ(first, second);
    CHECK_EQ(second, third);
}

TEST(tracker, signalling_extra_processes_keeps_the_root_and_the_oldest) {
    CapsuleTree tree(4);
    CHECK(tree.ready());

    Tracker tracker(tree.membership_name(), tree.pid());
    CHECK_EQ(tracker.scan().size(), size_t(5));

    int killed = tracker.signal_extra_processes(SIGKILL, 2);
    CHECK_EQ(killed, 3);

    usleep(300 * 1000);
    std::vector<ProcInfo> remaining = tracker.scan();
    CHECK(remaining.size() <= 2);
    // The capsule's first process is never the one chosen.
    CHECK(contains_pid(remaining, tree.pid()));
}

TEST(tracker, signalling_extra_processes_does_nothing_when_under_the_limit) {
    CapsuleTree tree(1);
    CHECK(tree.ready());

    Tracker tracker(tree.membership_name(), tree.pid());
    tracker.scan();
    CHECK_EQ(tracker.signal_extra_processes(SIGKILL, 10), 0);
}

TEST(tracker, signal_all_reaches_every_process) {
    CapsuleTree tree(3);
    CHECK(tree.ready());

    Tracker tracker(tree.membership_name(), tree.pid());
    size_t before = tracker.scan().size();
    CHECK_EQ(before, size_t(4));

    int delivered = tracker.signal_all(SIGKILL);
    CHECK_EQ(delivered, 4);

    usleep(400 * 1000);
    // The shell is this test's child, so reap it before scanning again or it
    // lingers as a zombie, which the tracker correctly ignores anyway.
    tree.stop();
    CHECK_EQ(tracker.scan().size(), size_t(0));
}

TEST(tracker, the_membership_name_is_exposed) {
    Tracker tracker(sb::membership_name("aaaaaaaaaaaa"), 0);
    CHECK_EQ(tracker.membership_name(), std::string("dev.isolate.capsule.aaaaaaaaaaaa"));
}

TEST(tracker, evidence_names_are_stable) {
    CHECK_EQ(std::string(to_string(Evidence::Attested)), std::string("attested"));
    CHECK_EQ(std::string(to_string(Evidence::Descendant)), std::string("descendant"));
}
