#include "harness.h"

#include "core/spec.h"
#include "proc/limits.h"

#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <functional>
#include <vector>

using namespace isolate;
using namespace isolate::proc;

namespace {

const LimitNote* note_for(const std::vector<LimitNote>& notes, const std::string& name) {
    for (const LimitNote& note : notes) {
        if (note.name == name) return &note;
    }
    return nullptr;
}

// Runs `body` in a child process and returns its exit status, so a test can
// change its own resource limits without affecting the test runner.
int in_child(const std::function<int()>& body) {
    pid_t child = ::fork();
    if (child < 0) return -1;
    if (child == 0) ::_exit(body());
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

} // namespace

TEST(limits, describe_reports_nothing_for_an_empty_policy) {
    Limits limits;
    // Core dumps are off by default, and that is enforced, so it is reported.
    std::vector<LimitNote> notes = describe(limits);
    CHECK_EQ(notes.size(), size_t(1));
    CHECK_EQ(notes[0].name, std::string("core dumps"));
}

TEST(limits, describe_separates_kernel_from_supervisor_enforcement) {
    Limits limits;
    limits.memory_bytes = 1ull << 30;
    limits.cpu_seconds = 60;
    limits.max_processes = 10;
    limits.wall_seconds = 300;
    limits.file_bytes = 1ull << 20;
    limits.open_files = 64;

    std::vector<LimitNote> notes = describe(limits);

    // The distinction a user has to be able to see: which of these the kernel
    // guarantees and which isolate only observes.
    CHECK(!note_for(notes, "memory")->kernel_enforced);
    CHECK(!note_for(notes, "processes")->kernel_enforced);
    CHECK(!note_for(notes, "wall time")->kernel_enforced);
    CHECK(note_for(notes, "cpu time")->kernel_enforced);
    CHECK(note_for(notes, "file size")->kernel_enforced);
    CHECK(note_for(notes, "open files")->kernel_enforced);
}

TEST(limits, every_note_explains_its_mechanism) {
    Limits limits;
    limits.memory_bytes = 1ull << 30;
    limits.cpu_seconds = 60;
    limits.max_processes = 10;
    limits.wall_seconds = 300;
    limits.file_bytes = 1ull << 20;
    limits.open_files = 64;
    limits.cpu_multiple = 2.0;

    for (const LimitNote& note : describe(limits)) {
        CHECK(!note.name.empty());
        CHECK(!note.value.empty());
        CHECK(note.mechanism.size() > 10);
    }
}

TEST(limits, the_memory_note_says_why_it_is_not_a_kernel_limit) {
    Limits limits;
    limits.memory_bytes = 1ull << 30;
    std::vector<LimitNote> notes = describe(limits);
    const LimitNote* note = note_for(notes, "memory");
    CHECK(note != nullptr);
    CHECK(note->mechanism.find("rlimit") != std::string::npos);
}

TEST(limits, the_process_note_explains_why_rlimit_nproc_is_unused) {
    // RLIMIT_NPROC counts every process belonging to the uid, so using it would
    // either do nothing or break the user's whole session.
    Limits limits;
    limits.max_processes = 8;
    std::vector<LimitNote> notes = describe(limits);
    const LimitNote* note = note_for(notes, "processes");
    CHECK(note != nullptr);
    CHECK(note->mechanism.find("per-uid") != std::string::npos);
}

TEST(limits, qos_clamp_selection) {
    Limits none;
    CHECK_EQ(qos_clamp_for(none), 0);

    Limits background;
    background.background_priority = true;
    CHECK_NE(qos_clamp_for(background), 0);

    Limits part_of_a_core;
    part_of_a_core.cpu_multiple = 0.5;
    Limits several_cores;
    several_cores.cpu_multiple = 4.0;
    // Less than a core means "do not compete"; more means "run, but yield".
    CHECK_NE(qos_clamp_for(part_of_a_core), qos_clamp_for(several_cores));
    CHECK_NE(qos_clamp_for(several_cores), 0);
}

TEST(limits, qos_clamp_names_are_stable) {
    CHECK_EQ(std::string(qos_clamp_name(0)), std::string("none"));
    CHECK_EQ(std::string(qos_clamp_name(1)), std::string("utility"));
    CHECK_EQ(std::string(qos_clamp_name(2)), std::string("background"));
    CHECK_EQ(std::string(qos_clamp_name(99)), std::string("unknown"));
}

TEST(limits, the_cpu_priority_note_does_not_claim_a_cpu_share) {
    // A clamp caps priority; it does not reserve or cap CPU time. Saying
    // otherwise would be the most tempting wrong claim in the project.
    Limits limits;
    limits.cpu_multiple = 2.0;
    std::vector<LimitNote> notes = describe(limits);
    const LimitNote* note = note_for(notes, "cpu priority");
    CHECK(note != nullptr);
    CHECK(note->mechanism.find("not CPU share") != std::string::npos);
}

TEST(limits, applying_rlimits_succeeds_for_a_reasonable_policy) {
    Limits limits;
    limits.cpu_seconds = 3600;
    limits.file_bytes = 1ull << 30;
    limits.open_files = 256;

    int status = in_child([&limits] {
        return apply_rlimits_nothrow(limits) == nullptr ? 0 : 1;
    });
    CHECK_EQ(status, 0);
}

TEST(limits, rlimit_cpu_rounds_up_rather_than_killing_immediately) {
    Limits limits;
    limits.cpu_seconds = 0.5;

    int status = in_child([&limits] {
        if (apply_rlimits_nothrow(limits) != nullptr) return 1;
        rlimit current {};
        if (::getrlimit(RLIMIT_CPU, &current) != 0) return 2;
        return current.rlim_cur >= 1 ? 0 : 3;
    });
    CHECK_EQ(status, 0);
}

TEST(limits, a_request_above_the_hard_limit_is_clamped_not_rejected) {
    Limits limits;
    limits.open_files = 1 << 20;

    int status = in_child([&limits] {
        rlimit before {};
        if (::getrlimit(RLIMIT_NOFILE, &before) != 0) return 2;
        if (apply_rlimits_nothrow(limits) != nullptr) return 1;
        rlimit after {};
        if (::getrlimit(RLIMIT_NOFILE, &after) != 0) return 3;
        if (before.rlim_max == RLIM_INFINITY) return 0;
        return after.rlim_cur <= before.rlim_max ? 0 : 4;
    });
    CHECK_EQ(status, 0);
}

TEST(limits, core_dumps_are_disabled_unless_asked_for) {
    Limits limits;
    int status = in_child([&limits] {
        if (apply_rlimits_nothrow(limits) != nullptr) return 1;
        rlimit current {};
        if (::getrlimit(RLIMIT_CORE, &current) != 0) return 2;
        return current.rlim_cur == 0 ? 0 : 3;
    });
    CHECK_EQ(status, 0);
}

TEST(limits, umask_is_applied) {
    Credentials credentials;
    credentials.umask = 0027;
    int status = in_child([&credentials] {
        if (apply_credentials_nothrow(credentials) != nullptr) return 1;
        mode_t previous = ::umask(0);
        return previous == 0027 ? 0 : 2;
    });
    CHECK_EQ(status, 0);
}

TEST(limits, dropping_to_another_user_is_refused_without_root) {
    // Claiming to run as another user while actually running as the caller
    // would be the worst possible outcome, so it has to fail loudly.
    if (::geteuid() == 0) return; // the refusal only applies unprivileged

    Credentials credentials;
    credentials.user = "nobody";
    int status = in_child([&credentials] {
        return apply_credentials_nothrow(credentials) != nullptr ? 0 : 1;
    });
    CHECK_EQ(status, 0);
}

TEST(limits, the_kernel_has_no_memory_ceiling_for_an_unprivileged_process) {
    // A claim about macOS, so it is checked rather than asserted. Root does
    // get these calls; see the doctor output for what that means.
    if (::geteuid() == 0) return;

    MemoryLimitProbe probe = probe_kernel_memory_limits();
    CHECK(!probe.footprint_limit_available);
    CHECK(!probe.memorystatus_available);
    CHECK(!probe.footprint_limit_error.empty());
    CHECK(!probe.memorystatus_error.empty());
}
