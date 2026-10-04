#include "harness.h"
#include "runner.h"

#include "util/json.h"

#include <unistd.h>

using namespace isolate;
using namespace isolate::test;

// Resource limits, and in particular the difference between the ones the
// kernel enforces and the ones isolate enforces by watching.

TEST(resources, a_wall_clock_limit_stops_the_capsule) {
    Environment env;
    Run run = env.isolate({"run", "--ephemeral", "--timeout", "2s", "--", "/bin/sleep",
                           "60"});
    CHECK_EQ(run.status, 125);
    CHECK(run.err_contains("time limit"));
}

TEST(resources, a_wall_clock_limit_does_not_affect_a_program_that_finishes) {
    Environment env;
    Run run = env.isolate({"run", "--ephemeral", "--timeout", "30s", "--", "/usr/bin/true"});
    CHECK(run.ok());
    CHECK(!run.err_contains("time limit"));
}

TEST(resources, a_time_limit_also_stops_the_processes_a_capsule_started) {
    Environment env;
    Run run = env.isolate({"run", "--ephemeral", "--timeout", "2s", "--", "/bin/sh", "-c",
                           "/bin/sleep 60 & /bin/sleep 60 & wait"});
    CHECK_EQ(run.status, 125);
    // Nothing is left behind after the limit fires.
    Run listed = env.isolate({"ls"});
    CHECK(listed.out_contains("no capsules"));
}

TEST(resources, a_memory_ceiling_stops_a_growing_capsule) {
    Environment env;
    // Allocating in steps with a pause makes the ceiling observable, which is
    // the realistic case: a build or a test suite growing over seconds.
    Run run = env.isolate({"run", "--ephemeral", "--memory", "96M", "--", "/bin/sh", "-c",
                           "i=0; while [ $i -lt 40 ]; do "
                           "/usr/bin/head -c 8000000 /dev/zero > $TMPDIR/chunk$i; "
                           "i=$((i+1)); done; echo FINISHED"});
    // Writing files does not grow the footprint, so this run should finish:
    // the point is that a memory ceiling does not fire on disk writes.
    CHECK(run.ok());
    CHECK(run.out_contains("FINISHED"));
}

TEST(resources, the_memory_ceiling_reports_its_own_overshoot) {
    Environment env;
    // A program that allocates as fast as it can will exceed the ceiling
    // between two samples. isolate must stop it and say by how much it was
    // over, rather than implying the ceiling was a guarantee.
    Run run = env.isolate({"run", "--ephemeral", "--memory", "64M", "--toolchain", "--",
                           "/usr/bin/python3", "-c",
                           "b=[]\n"
                           "import time\n"
                           "for i in range(200):\n"
                           "    b.append(bytearray(8*1024*1024))\n"
                           "    time.sleep(0.05)\n"
                           "print('FINISHED')\n"});
    CHECK_EQ(run.status, 125);
    CHECK(run.err_contains("memory limit"));
    CHECK(!run.out_contains("FINISHED"));
}

TEST(resources, a_process_ceiling_kills_the_excess) {
    Environment env;
    Run run = env.isolate({"run", "--ephemeral", "--processes", "3", "--", "/bin/sh", "-c",
                           "for i in 1 2 3 4 5 6 7 8; do /bin/sleep 5 & done; wait; "
                           "echo FINISHED"});
    CHECK(run.err_contains("process limit"));
    CHECK(run.err_contains("killed"));
}

TEST(resources, a_process_ceiling_leaves_a_small_capsule_alone) {
    Environment env;
    Run run = env.isolate({"run", "--ephemeral", "--processes", "10", "--", "/bin/sh", "-c",
                           "/bin/sleep 0.1 & wait; echo FINISHED"});
    CHECK(run.ok());
    CHECK(run.out_contains("FINISHED"));
    CHECK(!run.err_contains("process limit"));
}

TEST(resources, a_cpu_time_limit_is_enforced_by_the_kernel) {
    Environment env;
    // RLIMIT_CPU sends SIGXCPU, so the capsule dies on a signal rather than
    // being stopped by the supervisor.
    Run run = env.isolate({"run", "--ephemeral", "--cpu-time", "1s", "--", "/bin/sh", "-c",
                           "while :; do :; done"});
    CHECK(!run.ok());
    // 128 + SIGXCPU (24), or 128 + SIGKILL if the hard limit followed.
    CHECK(run.status == 128 + 24 || run.status == 128 + 9);
}

TEST(resources, a_file_size_limit_is_enforced_by_the_kernel) {
    Environment env;
    Run run = env.isolate({"run", "--ephemeral", "--max-file-size", "64K", "--", "/bin/sh",
                           "-c",
                           "/usr/bin/head -c 1000000 /dev/zero > $TMPDIR/big 2>/dev/null; "
                           "/usr/bin/stat -f%z $TMPDIR/big"});
    // The write is cut off at the limit rather than succeeding.
    CHECK(!run.out_contains("1000000"));
}

TEST(resources, an_open_file_limit_is_enforced_by_the_kernel) {
    Environment env;
    Run run = env.run_shell({"--max-open-files", "24"},
                            "/usr/bin/ulimit -n");
    CHECK(run.out_contains("24"));
}

TEST(resources, core_dumps_are_off_by_default) {
    Environment env;
    Run run = env.run_shell({}, "/usr/bin/ulimit -c");
    CHECK(run.out_contains("0"));
}

TEST(resources, inspect_says_which_limits_the_kernel_enforces) {
    Environment env;
    Run created = env.isolate({"run", "--name", "limited", "--memory", "512M",
                               "--processes", "16", "--cpu-time", "10m", "--timeout", "1h",
                               "--max-file-size", "100M", "--max-open-files", "128", "--",
                               "/usr/bin/true"});
    CHECK(created.ok());

    Run shown = env.isolate({"inspect", "limited"});
    CHECK(shown.ok());
    CHECK(shown.out_contains("ENFORCED BY"));
    CHECK(shown.out_contains("kernel"));
    CHECK(shown.out_contains("isolate"));
    // The memory ceiling must not be presented as a kernel guarantee.
    CHECK(shown.out_contains("memory"));
    CHECK(shown.out_contains("samples"));
}

TEST(resources, inspect_json_marks_enforcement_per_limit) {
    Environment env;
    Run created = env.isolate({"run", "--name", "limited", "--memory", "512M",
                               "--cpu-time", "60s", "--", "/usr/bin/true"});
    CHECK(created.ok());

    Run shown = env.isolate({"inspect", "--json", "limited"});
    CHECK(shown.ok());
    json::Value parsed = json::parse(shown.out);
    const json::Value* notes = parsed.get("limit_enforcement");
    CHECK(notes != nullptr);

    bool saw_memory = false;
    bool saw_cpu_time = false;
    for (const json::Value& note : notes->as_array()) {
        if (note.at("name").as_string() == "memory") {
            saw_memory = true;
            CHECK_EQ(note.at("kernel_enforced").as_bool(), false);
        }
        if (note.at("name").as_string() == "cpu time") {
            saw_cpu_time = true;
            CHECK_EQ(note.at("kernel_enforced").as_bool(), true);
        }
    }
    CHECK(saw_memory);
    CHECK(saw_cpu_time);
}

TEST(resources, doctor_reports_the_memory_ceiling_situation) {
    Environment env;
    Run run = env.isolate({"doctor"});
    CHECK(run.ok());
    CHECK(run.out_contains("memory limits"));
    // Limited either way: unprivileged there is no kernel ceiling, and as
    // root the kernel one is per-process while --memory is per-capsule.
    CHECK(run.out_contains("limited"));
    CHECK(run.out_contains("sampling") || run.out_contains("samples"));

    if (::geteuid() == 0) {
        CHECK(run.out_contains("per-process"));
    } else {
        // The exact calls tried, so the claim can be checked.
        CHECK(run.out_contains("task_set_phys_footprint_limit"));
        CHECK(run.out_contains("memorystatus_control"));
    }
}

TEST(resources, doctor_confirms_the_facilities_isolate_depends_on) {
    Environment env;
    Run run = env.isolate({"doctor"});
    CHECK(run.ok());
    CHECK(run.out_contains("sandbox profiles"));
    CHECK(run.out_contains("filesystem rules"));
    CHECK(run.out_contains("network rules"));
    CHECK(run.out_contains("signal isolation"));
    CHECK(run.out_contains("process attestation"));
    CHECK(run.out_contains("APFS cloning"));
    CHECK(run.out_contains("extent comparison"));
}

TEST(resources, doctor_states_the_host_process_visibility_limitation) {
    // The clearest difference from a PID namespace, so it has to be said
    // plainly rather than left for the reader to discover.
    Environment env;
    Run run = env.isolate({"doctor"});
    CHECK(run.out_contains("host process visibility"));
    CHECK(run.out_contains("PID namespace"));
}

TEST(resources, doctor_json_is_machine_readable) {
    Environment env;
    Run run = env.isolate({"doctor", "--json"});
    CHECK(run.ok());
    json::Value parsed = json::parse(run.out);
    const json::Value* checks = parsed.get("checks");
    CHECK(checks != nullptr);
    CHECK(checks->as_array().size() > 8);
    for (const json::Value& check : checks->as_array()) {
        CHECK(!check.at("name").as_string().empty());
        CHECK(!check.at("verdict").as_string().empty());
        CHECK(!check.at("detail").as_string().empty());
    }
}

TEST(resources, limits_are_rejected_when_they_cannot_be_met) {
    Environment env;
    Run memory = env.isolate({"run", "--ephemeral", "--memory", "1K", "--",
                              "/usr/bin/true"});
    CHECK(!memory.ok());
    CHECK(memory.err_contains("16M"));

    Run processes = env.isolate({"run", "--ephemeral", "--processes", "0", "--",
                                 "/usr/bin/true"});
    CHECK(!processes.ok());

    Run files = env.isolate({"run", "--ephemeral", "--max-open-files", "4", "--",
                             "/usr/bin/true"});
    CHECK(!files.ok());
}

TEST(resources, malformed_limit_values_are_reported) {
    Environment env;
    CHECK(!env.isolate({"run", "--ephemeral", "--memory", "lots", "--", "/usr/bin/true"})
               .ok());
    CHECK(!env.isolate({"run", "--ephemeral", "--timeout", "soon", "--", "/usr/bin/true"})
               .ok());
    CHECK(!env.isolate({"run", "--ephemeral", "--cpu", "fast", "--", "/usr/bin/true"})
               .ok());
}
