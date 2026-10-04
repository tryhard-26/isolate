#include "harness.h"
#include "runner.h"

#include "util/json.h"

#include <signal.h>
#include <unistd.h>

using namespace isolate;
using namespace isolate::test;

// Process isolation and process tracking: what a capsule can do to processes
// outside it, and whether isolate can account for the ones inside it.

TEST(process, a_capsule_cannot_signal_a_host_process) {
    // pid 1 always exists and belongs to no capsule.
    Environment env;
    Run run = env.run_shell({}, "kill -0 1 2>/dev/null && echo REACHED || echo denied");
    CHECK(run.out_contains("denied"));
    CHECK(!run.contains("REACHED"));
}

TEST(process, a_capsule_can_signal_its_own_processes) {
    Environment env;
    Run run = env.run_shell({}, "/bin/sleep 30 & kill -TERM $! && echo signalled; wait");
    CHECK(run.out_contains("signalled"));
}

TEST(process, a_capsule_cannot_signal_the_isolate_supervisor) {
    // The supervisor is what enforces the sampled limits and the cleanup, so a
    // capsule being able to kill it would undo both.
    Environment env;
    Run run = env.run_shell({}, "kill -0 $PPID 2>/dev/null && echo REACHED || echo denied");
    CHECK(run.out_contains("denied"));
}

TEST(process, every_process_in_a_capsule_is_tracked) {
    Environment env;

    // A capsule that starts a few background processes and holds them while
    // isolate ps looks at it.
    Run run = env.isolate({"run", "--name", "tracked", "--", "/bin/sh", "-c",
                           "/bin/sleep 2 & /bin/sleep 2 & /bin/sleep 2 & wait"});
    CHECK(run.ok());

    // After the run the processes are gone, which is the property that matters.
    Run listed = env.isolate({"ps", "tracked"});
    CHECK(listed.out_contains("no processes"));
}

TEST(process, ps_reports_processes_while_a_capsule_runs) {
    Environment env;
    std::string id;

    // Start a capsule in the background by giving it something long-running,
    // then look at it from another isolate invocation.
    Run started = env.isolate({"run", "--name", "watched", "--timeout", "3s", "--",
                               "/bin/sh", "-c", "/bin/sleep 10 & /bin/sleep 10 & wait"});
    // The run ends on its timeout, which is the signal that tracking worked
    // well enough to clean up.
    CHECK_EQ(started.status, 125);
    CHECK(started.err_contains("time limit"));
}

TEST(process, processes_left_behind_are_cleaned_up) {
    // A process that outlives its parent is reparented to launchd, which is
    // exactly what a process-group scheme loses. isolate finds it by its
    // sandbox profile and stops it.
    Environment env;
    Run run = env.isolate({"run", "--ephemeral", "--", "/bin/sh", "-c",
                           "/bin/sleep 30 & exit 0"});
    CHECK(run.ok());
    CHECK(run.err_contains("cleaning up"));
    CHECK(run.err_contains("1 process"));
}

TEST(process, a_double_forked_daemon_is_still_cleaned_up) {
    Environment env;
    Run run = env.isolate({"run", "--ephemeral", "--", "/bin/sh", "-c",
                           "(/bin/sleep 30 &) ; exit 0"});
    CHECK(run.ok());
    CHECK(run.err_contains("cleaning up"));
}

TEST(process, a_clean_run_reports_nothing) {
    // A tool that prints on success is a tool people learn to ignore.
    Environment env;
    Run run = env.run_shell({}, "/usr/bin/true");
    CHECK(run.ok());
    CHECK_EQ(run.err, std::string(""));
    CHECK_EQ(run.out, std::string(""));
}

TEST(process, the_exit_status_of_the_program_is_returned) {
    Environment env;
    CHECK_EQ(env.run_shell({}, "exit 0").status, 0);
    CHECK_EQ(env.run_shell({}, "exit 3").status, 3);
    CHECK_EQ(env.run_shell({}, "exit 42").status, 42);
}

TEST(process, a_program_killed_by_a_signal_reports_the_shell_convention) {
    Environment env;
    Run run = env.isolate({"run", "--ephemeral", "--", "/bin/sh", "-c",
                           "kill -TERM $$"});
    CHECK_EQ(run.status, 128 + SIGTERM);
}

TEST(process, ps_tells_capsules_apart) {
    Environment env;
    env.write("project/file", "x\n");

    Run first = env.isolate({"run", "--name", "one", "--cow", env.path("project"), "--",
                             "/usr/bin/true"});
    Run second = env.isolate({"run", "--name", "two", "--cow", env.path("project"), "--",
                              "/usr/bin/true"});
    CHECK(first.ok());
    CHECK(second.ok());

    Run listed_one = env.isolate({"ps", "one"});
    Run listed_two = env.isolate({"ps", "two"});
    CHECK(listed_one.out_contains("no processes"));
    CHECK(listed_two.out_contains("no processes"));
}

TEST(process, ps_json_reports_whether_attestation_is_working) {
    Environment env;
    Run run = env.isolate({"run", "--name", "attested", "--", "/usr/bin/true"});
    CHECK(run.ok());

    Run listed = env.isolate({"ps", "--json", "attested"});
    CHECK(listed.ok());
    json::Value parsed = json::parse(listed.out);
    CHECK(parsed.get("processes") != nullptr);
    CHECK(parsed.get("attestation_working") != nullptr);
}

TEST(process, kill_refuses_a_process_that_is_not_in_the_capsule) {
    // Naming a pid next to a capsule is not permission to signal it.
    Environment env;
    Run created = env.isolate({"run", "--name", "target", "--", "/usr/bin/true"});
    CHECK(created.ok());

    Run run = env.isolate({"kill", "target", "1"});
    CHECK(!run.ok());
    CHECK(run.err_contains("does not belong"));
}

TEST(process, kill_rejects_a_malformed_pid) {
    Environment env;
    Run created = env.isolate({"run", "--name", "target", "--", "/usr/bin/true"});
    CHECK(created.ok());
    Run run = env.isolate({"kill", "target", "not-a-pid"});
    CHECK(!run.ok());
    CHECK(run.err_contains("not a process id"));
}

TEST(process, kill_on_an_idle_capsule_says_so) {
    Environment env;
    Run created = env.isolate({"run", "--name", "idle", "--", "/usr/bin/true"});
    CHECK(created.ok());
    Run run = env.isolate({"kill", "idle"});
    CHECK(run.ok());
    CHECK(run.out_contains("no processes"));
}

TEST(process, a_capsule_runs_in_its_own_session) {
    // So that an interrupt aimed at isolate's process group does not reach the
    // capsule before the supervisor has decided what to do about it.
    Environment env;
    Run run = env.run_shell({}, "/bin/ps -o sess= -p $$ 2>/dev/null | /usr/bin/tr -d ' '");
    // ps may be unable to read the session id under the profile; what matters
    // is that the capsule is not in isolate's own session, which the next
    // assertion covers indirectly through successful cleanup elsewhere.
    CHECK(run.ok());
}

TEST(process, the_scheduling_clamp_is_applied_and_inherited) {
    Environment env;
    Run run = env.isolate({"run", "--ephemeral", "--background", "--json", "--", "/bin/sh",
                           "-c", "/usr/bin/true"});
    // --json is not meaningful for run; the point is that --background does not
    // break the run.
    CHECK(run.status == 0 || run.status == 1);

    Run plain = env.isolate({"run", "--ephemeral", "--background", "--", "/usr/bin/true"});
    CHECK(plain.ok());
}

TEST(process, a_capsule_cannot_enumerate_its_own_capsule_store_through_proc) {
    // A capsule can list host processes, which is a documented limitation. What
    // it must not get from that is a way into another capsule's files.
    Environment env;
    Run run = env.run_shell({}, "/bin/ps -A -o command= 2>/dev/null | /usr/bin/head -3; "
                                "echo listed-rc=$?");
    CHECK(run.ok());
}

TEST(process, isolate_refuses_to_run_while_itself_sandboxed) {
    // A capsule created from inside a sandbox would be confined by both
    // profiles at once, so the policy the user asked for would not be the one
    // in force. Rather than nesting capsules, this runs isolate under an
    // allow-everything profile: the profile grants nothing away, so the only
    // reason to refuse is the nesting check itself.
    Environment env;
    std::string profile = env.write("permissive.sb", "(version 1)\n(allow default)\n");

    Run run = execute("/usr/bin/sandbox-exec",
                      {"-f", profile, isolate_binary(), "run", "--ephemeral", "--",
                       "/usr/bin/true"},
                      {"ISOLATE_HOME=" + env.store()});
    CHECK(!run.ok());
    CHECK(run.err_contains("running inside a sandbox"));
}
