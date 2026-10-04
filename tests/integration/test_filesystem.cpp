#include "harness.h"
#include "runner.h"

using namespace isolate::test;

// What a capsule can and cannot reach on the filesystem, asked of the kernel
// rather than of the profile generator.

TEST(filesystem, a_default_capsule_cannot_write_outside_itself) {
    Environment env;
    std::string target = env.path("should-not-appear");

    Run run = env.run_shell({}, "echo x > " + target + " 2>/dev/null; echo rc=$?");
    CHECK(run.out_contains("rc=1"));
    CHECK(!env.exists("should-not-appear"));
}

TEST(filesystem, a_default_capsule_cannot_read_the_home_directory) {
    Environment env;
    Run run = env.run_shell({}, "ls /Users >/dev/null 2>&1; echo rc=$?");
    CHECK(run.out_contains("rc=1"));
}

TEST(filesystem, a_default_capsule_can_run_base_system_tools) {
    Environment env;
    Run run = env.run_shell({}, "/bin/echo from-a-capsule");
    CHECK(run.ok());
    CHECK(run.out_contains("from-a-capsule"));
}

TEST(filesystem, read_only_mounts_allow_reads_and_refuse_writes) {
    Environment env;
    env.write("data/file.txt", "contents\n");

    Run run = env.run_shell({"--ro", env.path("data")},
                            "cat " + env.path("data/file.txt") +
                                "; echo x > " + env.path("data/new") +
                                " 2>/dev/null; echo rc=$?");
    CHECK(run.out_contains("contents"));
    CHECK(run.out_contains("rc=1"));
    CHECK(!env.exists("data/new"));
}

TEST(filesystem, read_write_mounts_write_through_to_the_host) {
    Environment env;
    env.write("data/file.txt", "before\n");

    Run run = env.run_shell({"--rw", env.path("data")},
                            "echo after > " + env.path("data/file.txt"));
    CHECK(run.ok());
    CHECK_EQ(env.read("data/file.txt"), std::string("after\n"));
}

TEST(filesystem, a_denial_overrides_an_enclosing_read_write_mount) {
    Environment env;
    env.write("data/open/fine.txt", "fine\n");
    env.write("data/secret/private.txt", "secret\n");

    Run run = env.run_shell({"--rw", env.path("data"), "--deny", env.path("data/secret")},
                            "cat " + env.path("data/open/fine.txt") + "; cat " +
                                env.path("data/secret/private.txt") +
                                " 2>/dev/null && echo LEAK || echo denied");
    CHECK(run.out_contains("fine"));
    CHECK(run.out_contains("denied"));
    CHECK(!run.out_contains("LEAK"));
    CHECK(!run.out_contains("secret"));
}

TEST(filesystem, a_denial_overrides_an_enclosing_read_only_mount) {
    // The case that fails if the generator ever mixes operation tokens: the
    // sandbox resolves a conflict by operation specificity first, so a deny
    // written with broader operations than the allow is silently ignored.
    Environment env;
    env.write("data/open/fine.txt", "fine\n");
    env.write("data/secret/private.txt", "secret\n");

    Run run = env.run_shell({"--ro", env.path("data"), "--deny", env.path("data/secret")},
                            "cat " + env.path("data/open/fine.txt") + "; cat " +
                                env.path("data/secret/private.txt") +
                                " 2>/dev/null && echo LEAK || echo denied");
    CHECK(run.out_contains("fine"));
    CHECK(run.out_contains("denied"));
    CHECK(!run.out_contains("LEAK"));
    CHECK(!run.out_contains("secret"));
}

TEST(filesystem, a_denied_path_cannot_even_be_listed) {
    Environment env;
    env.write("data/secret/private.txt", "secret\n");

    Run run = env.run_shell({"--ro", env.path("data"), "--deny", env.path("data/secret")},
                            "ls " + env.path("data/secret") +
                                " 2>/dev/null && echo LISTED || echo hidden");
    CHECK(run.out_contains("hidden"));
}

TEST(filesystem, a_cloned_mount_keeps_writes_away_from_the_host) {
    Environment env;
    env.write("project/src/main.c", "original\n");
    env.write("project/README", "readme\n");

    Run run = env.run_shell({"--cow", env.path("project")},
                            "echo changed > src/main.c; echo new > added.txt; rm README; "
                            "ls");
    CHECK(run.ok());
    CHECK(run.out_contains("added.txt"));

    // The host is exactly as it was.
    CHECK_EQ(env.read("project/src/main.c"), std::string("original\n"));
    CHECK(env.exists("project/README"));
    CHECK(!env.exists("project/added.txt"));
}

TEST(filesystem, a_cloned_mount_becomes_the_working_directory) {
    Environment env;
    env.write("project/marker", "here\n");

    // The relative path resolves, which is what makes `isolate run --cow .`
    // behave the way a user expects.
    Run run = env.run_shell({"--cow", env.path("project")}, "pwd; cat marker");
    CHECK(run.ok());
    CHECK(run.out_contains("here"));
    CHECK(run.out_contains("/layers/project"));
    // The working directory is the clone, not the original.
    CHECK(!run.out_contains(env.path("project") + "\n"));
}

TEST(filesystem, the_original_of_a_cloned_mount_is_unreachable) {
    // Even when an enclosing directory is mounted read-only, which would
    // otherwise expose the very files the clone exists to protect.
    Environment env;
    env.write("tree/project/secret.txt", "original\n");
    env.write("tree/other.txt", "other\n");

    Run run = env.run_shell({"--ro", env.path("tree"), "--cow", env.path("tree/project")},
                            "cat " + env.path("tree/other.txt") + "; cat " +
                                env.path("tree/project/secret.txt") +
                                " 2>/dev/null && echo LEAK || echo denied; cat secret.txt");
    CHECK(run.out_contains("other"));
    CHECK(run.out_contains("denied"));
    CHECK(!run.out_contains("LEAK"));
    // The clone's own copy is readable.
    CHECK(run.out_contains("original"));
}

TEST(filesystem, the_capsule_cannot_read_or_alter_its_own_metadata) {
    // If it could, it would be able to rewrite the policy being enforced or the
    // baseline that `isolate diff` reports against.
    Environment env;
    env.write("project/file", "x\n");

    Run run = env.run_shell(
        {"--cow", env.path("project")},
        "cat ../../capsule.json 2>/dev/null && echo READ_POLICY || echo policy-hidden; "
        "ls ../../base 2>/dev/null && echo READ_BASE || echo base-hidden; "
        "echo x > ../../capsule.json 2>/dev/null && echo WROTE_POLICY || echo policy-ro");
    CHECK(run.out_contains("policy-hidden"));
    CHECK(run.out_contains("base-hidden"));
    CHECK(run.out_contains("policy-ro"));
    CHECK(!run.contains("READ_POLICY"));
    CHECK(!run.contains("READ_BASE"));
    CHECK(!run.contains("WROTE_POLICY"));
}

TEST(filesystem, the_capsule_cannot_reach_other_capsules) {
    Environment env;
    env.write("project/file", "x\n");

    // Create a persistent capsule whose layer holds a recognisable string.
    Run first = env.isolate({"run", "--name", "other", "--cow", env.path("project"), "--",
                             "/bin/sh", "-c", "echo marker-from-other > secret"});
    CHECK(first.ok());

    // Checked by path rather than by searching the filesystem: the store is
    // where every other capsule's layers and policy live.
    Run run = env.run_shell({"--cow", env.path("project")},
                            "ls " + env.store() + " 2>/dev/null && echo SAW_STORE || "
                            "echo store-hidden; "
                            "ls " + env.store() + "/capsules 2>/dev/null && "
                            "echo SAW_CAPSULES || echo capsules-hidden");
    CHECK(run.out_contains("store-hidden"));
    CHECK(run.out_contains("capsules-hidden"));
    CHECK(!run.contains("SAW_STORE"));
    CHECK(!run.contains("SAW_CAPSULES"));
}

TEST(filesystem, the_store_cannot_be_mounted_into_a_capsule) {
    Environment env;
    Run run = env.isolate({"run", "--ephemeral", "--rw", env.store(), "--",
                           "/usr/bin/true"});
    CHECK(!run.ok());
    CHECK(run.err_contains("capsule store"));
}

TEST(filesystem, private_tmp_and_home_are_inside_the_capsule) {
    Environment env;
    Run run = env.run_shell({}, "echo home=$HOME; echo tmp=$TMPDIR; "
                                "touch $HOME/file && echo home-writable; "
                                "touch $TMPDIR/file && echo tmp-writable");
    CHECK(run.ok());
    CHECK(run.out_contains("home-writable"));
    CHECK(run.out_contains("tmp-writable"));
    CHECK(run.out_contains("home=" + env.store()));
    CHECK(run.out_contains("tmp=" + env.store()));
}

TEST(filesystem, the_host_temporary_directory_is_not_reachable) {
    Environment env;
    Run run = env.run_shell({}, "echo x > /private/tmp/isolate-should-not-write "
                                "2>/dev/null && echo WROTE || echo denied");
    CHECK(run.out_contains("denied"));
}

TEST(filesystem, two_capsules_do_not_share_scratch_space) {
    Environment env;
    Run first = env.run_shell({}, "echo from-first > $TMPDIR/shared; ls $TMPDIR");
    CHECK(first.ok());
    CHECK(first.out_contains("shared"));

    Run second = env.run_shell({}, "cat $TMPDIR/shared 2>/dev/null && echo SHARED || "
                                   "echo private");
    CHECK(second.out_contains("private"));
    CHECK(!second.contains("from-first"));
}

TEST(filesystem, writes_to_private_tmp_do_not_survive_an_ephemeral_capsule) {
    Environment env;
    Run run = env.run_shell({}, "echo x > $TMPDIR/file; echo $TMPDIR > /dev/null");
    CHECK(run.ok());
    // The capsule directory is gone, so nothing it wrote is left.
    Run listed = env.isolate({"ls"});
    CHECK(listed.out_contains("no capsules"));
}

TEST(filesystem, a_capsule_cannot_execute_privilege_helpers) {
    Environment env;
    Run run = env.run_shell({}, "/usr/bin/sudo -n /usr/bin/true 2>/dev/null && echo RAN "
                                "|| echo denied");
    CHECK(run.out_contains("denied"));
    CHECK(!run.contains("RAN"));
}

TEST(filesystem, relative_paths_are_resolved_before_the_policy_is_built) {
    // /tmp is a symlink to /private/tmp, and the sandbox matches resolved
    // paths, so a rule written for /tmp would never match anything.
    Environment env;
    env.write("data/file", "contents\n");

    Run run = env.isolate({"run", "--ephemeral", "--ro", env.path("data"), "--",
                           "/bin/cat", env.path("data/file")});
    CHECK(run.ok());
    CHECK(run.out_contains("contents"));

    // /etc is a symlink to /private/etc, and the recorded policy shows the
    // resolved path because that is what the kernel will match against.
    Run inspect = env.isolate({"run", "--name", "resolved", "--ro", "/etc", "--",
                               "/usr/bin/true"});
    CHECK(inspect.ok());
    Run shown = env.isolate({"inspect", "resolved"});
    CHECK(shown.out_contains("/private/etc"));
}

TEST(filesystem, a_program_outside_every_mount_can_still_be_started) {
    Environment env;
    env.write("bin/hello", "#!/bin/sh\necho hello-from-script\n");
    Run chmod_run = execute("/bin/chmod", {"755", env.path("bin/hello")});
    CHECK(chmod_run.ok());

    Run run = env.isolate({"run", "--ephemeral", "--", env.path("bin/hello")});
    CHECK(run.ok());
    CHECK(run.out_contains("hello-from-script"));
}

TEST(filesystem, a_missing_program_is_reported_before_a_capsule_is_created) {
    Environment env;
    Run run = env.isolate({"run", "--ephemeral", "--", "/no/such/program"});
    CHECK(!run.ok());
    CHECK(run.err_contains("cannot run"));
    // No capsule directory left behind.
    Run listed = env.isolate({"ls"});
    CHECK(listed.out_contains("no capsules"));
}

TEST(filesystem, a_program_name_is_resolved_against_the_capsule_path) {
    Environment env;
    Run run = env.isolate({"run", "--ephemeral", "--", "echo", "resolved-by-name"});
    CHECK(run.ok());
    CHECK(run.out_contains("resolved-by-name"));
}

TEST(filesystem, cow_requires_a_directory) {
    Environment env;
    env.write("file", "x");
    Run run = env.isolate({"run", "--ephemeral", "--cow", env.path("file"), "--",
                           "/usr/bin/true"});
    CHECK(!run.ok());
    CHECK(run.err_contains("needs a directory"));
}

TEST(filesystem, a_capsule_can_write_a_large_file_within_its_layer) {
    Environment env;
    env.write("project/placeholder", "x\n");

    Run run = env.run_shell({"--cow", env.path("project")},
                            "/usr/bin/head -c 2000000 /dev/zero > big.bin && "
                            "/usr/bin/stat -f%z big.bin");
    CHECK(run.ok());
    CHECK(run.out_contains("2000000"));
}
