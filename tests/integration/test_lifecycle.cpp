#include "harness.h"
#include "runner.h"

#include "util/json.h"

using namespace isolate;
using namespace isolate::test;

// Capsule lifecycle: ephemeral and persistent capsules, diff, commit,
// snapshots, and destruction.

TEST(lifecycle, an_ephemeral_capsule_leaves_nothing_behind) {
    Environment env;
    env.write("project/file", "original\n");

    Run run = env.run_shell({"--cow", env.path("project")},
                            "echo changed > file; echo extra > new");
    CHECK(run.ok());

    Run listed = env.isolate({"ls"});
    CHECK(listed.out_contains("no capsules"));
    CHECK_EQ(env.read("project/file"), std::string("original\n"));
    CHECK(!env.exists("project/new"));
}

TEST(lifecycle, keep_retains_an_ephemeral_capsule) {
    Environment env;
    env.write("project/file", "original\n");

    Run run = env.isolate({"run", "--ephemeral", "--keep", "--name", "kept", "--cow",
                           env.path("project"), "--", "/bin/sh", "-c",
                           "echo changed > file"});
    CHECK(run.ok());

    Run listed = env.isolate({"ls"});
    CHECK(listed.out_contains("kept"));
    CHECK(listed.out_contains("ephemeral"));
}

TEST(lifecycle, a_persistent_capsule_keeps_its_changes_between_runs) {
    Environment env;
    env.write("project/file", "original\n");

    Run first = env.isolate({"run", "--name", "dev", "--cow", env.path("project"), "--",
                             "/bin/sh", "-c", "echo step-one > progress"});
    CHECK(first.ok());

    Run second = env.isolate({"enter", "dev", "--", "/bin/sh", "-c",
                              "cat progress; echo step-two >> progress"});
    CHECK(second.ok());
    CHECK(second.out_contains("step-one"));

    Run third = env.isolate({"enter", "dev", "--", "/bin/cat", "progress"});
    CHECK(third.ok());
    CHECK(third.out_contains("step-one"));
    CHECK(third.out_contains("step-two"));

    // None of it reached the host.
    CHECK(!env.exists("project/progress"));
}

TEST(lifecycle, enter_refuses_to_change_the_policy) {
    // A capsule's policy is fixed when it is created; otherwise `isolate
    // inspect` would not describe what actually happened.
    Environment env;
    env.write("project/file", "x\n");
    Run created = env.isolate({"run", "--name", "fixed", "--cow", env.path("project"), "--",
                               "/usr/bin/true"});
    CHECK(created.ok());

    Run widened = env.isolate({"enter", "fixed", "--rw", "/private/tmp", "--",
                               "/usr/bin/true"});
    CHECK(!widened.ok());
    CHECK(widened.err_contains("fixed when it is created"));

    Run networked = env.isolate({"enter", "fixed", "--network", "all", "--",
                                 "/usr/bin/true"});
    CHECK(!networked.ok());
}

TEST(lifecycle, enter_accepts_per_run_limits) {
    Environment env;
    env.write("project/file", "x\n");
    Run created = env.isolate({"run", "--name", "limited", "--cow", env.path("project"),
                               "--", "/usr/bin/true"});
    CHECK(created.ok());

    Run entered = env.isolate({"enter", "limited", "--timeout", "2s", "--", "/bin/sleep",
                               "60"});
    CHECK_EQ(entered.status, 125);

    // The capsule's recorded policy is unchanged by that run.
    Run shown = env.isolate({"inspect", "--json", "limited"});
    json::Value parsed = json::parse(shown.out);
    const json::Value* limits = parsed.at("policy").get("limits");
    CHECK(limits != nullptr);
    CHECK(limits->get("wall_seconds") == nullptr ||
          limits->get("wall_seconds")->is_null());
}

TEST(lifecycle, diff_reports_what_a_capsule_changed) {
    Environment env;
    env.write("project/keep", "keep\n");
    env.write("project/modify", "before\n");
    env.write("project/remove", "gone\n");

    Run run = env.isolate({"run", "--name", "changed", "--cow", env.path("project"), "--",
                           "/bin/sh", "-c",
                           "echo after > modify; rm remove; echo new > added"});
    CHECK(run.ok());

    Run diff = env.isolate({"diff", "changed"});
    CHECK(diff.out_contains("A added"));
    CHECK(diff.out_contains("M modify"));
    CHECK(diff.out_contains("D remove"));
    CHECK(!diff.out_contains("keep"));
}

TEST(lifecycle, diff_proves_unchanged_files_from_their_extent_maps) {
    Environment env;
    for (int i = 0; i < 20; ++i) {
        env.write("project/f" + std::to_string(i), std::string(40000, 'x'));
    }

    Run run = env.isolate({"run", "--name", "mostly-unchanged", "--cow",
                           env.path("project"), "--", "/bin/sh", "-c",
                           "echo touched > f3"});
    CHECK(run.ok());

    Run diff = env.isolate({"diff", "mostly-unchanged"});
    CHECK(diff.out_contains("M f3"));
    CHECK(diff.out_contains("proven unchanged from their extent maps"));
    // 19 untouched files cost no I/O to verify.
    CHECK(diff.out_contains("19 proven unchanged"));
}

TEST(lifecycle, diff_collapses_a_whole_new_directory) {
    Environment env;
    env.write("project/file", "x\n");

    Run run = env.isolate({"run", "--name", "built", "--cow", env.path("project"), "--",
                           "/bin/sh", "-c",
                           "mkdir -p out/deep; for i in 1 2 3 4 5; do echo x > "
                           "out/deep/f$i; done"});
    CHECK(run.ok());

    Run diff = env.isolate({"diff", "built"});
    CHECK(diff.out_contains("A out/"));
    CHECK(diff.out_contains("entries"));
    // The files inside are summarised rather than listed.
    CHECK(!diff.out_contains("out/deep/f1"));
}

TEST(lifecycle, diff_json_is_machine_readable) {
    Environment env;
    env.write("project/file", "before\n");
    Run run = env.isolate({"run", "--name", "changed", "--cow", env.path("project"), "--",
                           "/bin/sh", "-c", "echo after > file"});
    CHECK(run.ok());

    Run diff = env.isolate({"diff", "--json", "changed"});
    json::Value parsed = json::parse(diff.out);
    const json::Value* layers = parsed.get("layers");
    CHECK(layers != nullptr);
    CHECK_EQ(layers->as_array().size(), size_t(1));
    const json::Value& layer = layers->as_array()[0];
    CHECK_EQ(layer.at("capsule_changes").at("changes").as_array().size(), size_t(1));
}

TEST(lifecycle, diff_says_so_when_there_is_nothing_to_report) {
    Environment env;
    env.write("project/file", "x\n");
    Run run = env.isolate({"run", "--name", "untouched", "--cow", env.path("project"), "--",
                           "/usr/bin/true"});
    CHECK(run.ok());

    Run diff = env.isolate({"diff", "untouched"});
    CHECK(diff.out_contains("no changes"));
}

TEST(lifecycle, diff_explains_a_capsule_with_no_cloned_mounts) {
    Environment env;
    Run run = env.isolate({"run", "--name", "direct", "--", "/usr/bin/true"});
    CHECK(run.ok());
    Run diff = env.isolate({"diff", "direct"});
    CHECK(diff.out_contains("no cloned mounts"));
}

TEST(lifecycle, commit_writes_changes_back_to_the_host) {
    Environment env;
    env.write("project/file", "before\n");

    Run run = env.isolate({"run", "--name", "work", "--cow", env.path("project"), "--",
                           "/bin/sh", "-c", "echo after > file; echo new > added"});
    CHECK(run.ok());

    Run commit = env.isolate({"commit", "work"});
    CHECK(commit.ok());
    CHECK_EQ(env.read("project/file"), std::string("after\n"));
    CHECK_EQ(env.read("project/added"), std::string("new\n"));
}

TEST(lifecycle, commit_dry_run_changes_nothing) {
    Environment env;
    env.write("project/file", "before\n");
    Run run = env.isolate({"run", "--name", "work", "--cow", env.path("project"), "--",
                           "/bin/sh", "-c", "echo after > file"});
    CHECK(run.ok());

    Run dry = env.isolate({"commit", "--dry-run", "work"});
    CHECK(dry.ok());
    CHECK(dry.out_contains("dry run"));
    CHECK_EQ(env.read("project/file"), std::string("before\n"));
}

TEST(lifecycle, commit_withholds_paths_that_changed_on_the_host) {
    Environment env;
    env.write("project/shared", "original\n");

    Run run = env.isolate({"run", "--name", "work", "--cow", env.path("project"), "--",
                           "/bin/sh", "-c", "echo from-capsule > shared"});
    CHECK(run.ok());

    env.write("project/shared", "from-host\n");

    Run commit = env.isolate({"commit", "work"});
    CHECK(!commit.ok());
    CHECK(commit.out_contains("also changed on the host"));
    CHECK_EQ(env.read("project/shared"), std::string("from-host\n"));
}

TEST(lifecycle, commit_with_force_overwrites_a_conflict) {
    Environment env;
    env.write("project/shared", "original\n");
    Run run = env.isolate({"run", "--name", "work", "--cow", env.path("project"), "--",
                           "/bin/sh", "-c", "echo from-capsule > shared"});
    CHECK(run.ok());
    env.write("project/shared", "from-host\n");

    Run commit = env.isolate({"commit", "--force", "work"});
    CHECK(commit.ok());
    CHECK_EQ(env.read("project/shared"), std::string("from-capsule\n"));
}

TEST(lifecycle, a_second_commit_has_nothing_to_do) {
    Environment env;
    env.write("project/file", "before\n");
    Run run = env.isolate({"run", "--name", "work", "--cow", env.path("project"), "--",
                           "/bin/sh", "-c", "echo after > file"});
    CHECK(run.ok());

    CHECK(env.isolate({"commit", "work"}).ok());
    Run again = env.isolate({"commit", "work"});
    CHECK(again.ok());
    CHECK(again.out_contains("no changes"));
}

TEST(lifecycle, commit_refuses_a_capsule_with_no_cloned_mounts) {
    Environment env;
    Run run = env.isolate({"run", "--name", "direct", "--", "/usr/bin/true"});
    CHECK(run.ok());
    Run commit = env.isolate({"commit", "direct"});
    CHECK(!commit.ok());
    CHECK(commit.err_contains("nothing to commit"));
}

TEST(lifecycle, a_snapshot_can_be_restored_after_arbitrary_damage) {
    Environment env;
    env.write("project/keep.txt", "keep\n");
    env.write("project/sub/nested.txt", "nested\n");

    Run created = env.isolate({"run", "--name", "lab", "--cow", env.path("project"), "--",
                               "/usr/bin/true"});
    CHECK(created.ok());

    Run snapshot = env.isolate({"snapshot", "create", "lab", "clean"});
    CHECK(snapshot.ok());

    Run wreck = env.isolate({"enter", "lab", "--", "/bin/sh", "-c",
                             "rm -rf sub; echo wrecked > keep.txt; echo junk > junk.txt"});
    CHECK(wreck.ok());

    Run before = env.isolate({"enter", "lab", "--", "/bin/cat", "keep.txt"});
    CHECK(before.out_contains("wrecked"));

    Run restore = env.isolate({"snapshot", "restore", "lab", "clean", "--force"});
    CHECK(restore.ok());

    Run after = env.isolate({"enter", "lab", "--", "/bin/sh", "-c",
                             "cat keep.txt; cat sub/nested.txt; ls junk.txt 2>/dev/null "
                             "&& echo JUNK_SURVIVED || echo junk-gone"});
    CHECK(after.out_contains("keep"));
    CHECK(after.out_contains("nested"));
    CHECK(after.out_contains("junk-gone"));
    CHECK(!after.contains("wrecked"));
}

TEST(lifecycle, restoring_resets_what_diff_reports) {
    Environment env;
    env.write("project/file", "original\n");
    Run created = env.isolate({"run", "--name", "lab", "--cow", env.path("project"), "--",
                               "/usr/bin/true"});
    CHECK(created.ok());
    CHECK(env.isolate({"snapshot", "create", "lab", "clean"}).ok());

    Run changed = env.isolate({"enter", "lab", "--", "/bin/sh", "-c",
                               "echo changed > file"});
    CHECK(changed.ok());
    CHECK(env.isolate({"diff", "lab"}).out_contains("M file"));

    CHECK(env.isolate({"snapshot", "restore", "lab", "clean", "--force"}).ok());
    CHECK(env.isolate({"diff", "lab"}).out_contains("no changes"));
}

TEST(lifecycle, restore_refuses_to_discard_uncommitted_work_without_force) {
    Environment env;
    env.write("project/file", "original\n");
    Run created = env.isolate({"run", "--name", "lab", "--cow", env.path("project"), "--",
                               "/usr/bin/true"});
    CHECK(created.ok());
    CHECK(env.isolate({"snapshot", "create", "lab", "clean"}).ok());
    CHECK(env.isolate({"enter", "lab", "--", "/bin/sh", "-c", "echo changed > file"}).ok());

    Run restore = env.isolate({"snapshot", "restore", "lab", "clean"});
    CHECK(!restore.ok());
    CHECK(restore.err_contains("uncommitted"));
}

TEST(lifecycle, snapshots_can_be_listed_and_deleted) {
    Environment env;
    env.write("project/file", "x\n");
    Run created = env.isolate({"run", "--name", "lab", "--cow", env.path("project"), "--",
                               "/usr/bin/true"});
    CHECK(created.ok());

    CHECK(env.isolate({"snapshot", "create", "lab", "first", "--note", "the first"}).ok());
    CHECK(env.isolate({"snapshot", "create", "lab", "second"}).ok());

    Run listed = env.isolate({"snapshot", "ls", "lab"});
    CHECK(listed.out_contains("first"));
    CHECK(listed.out_contains("the first"));
    CHECK(listed.out_contains("second"));

    CHECK(env.isolate({"snapshot", "rm", "lab", "first"}).ok());
    Run after = env.isolate({"snapshot", "ls", "lab"});
    CHECK(!after.out_contains("first"));
    CHECK(after.out_contains("second"));
}

TEST(lifecycle, a_snapshot_name_is_checked) {
    Environment env;
    Run created = env.isolate({"run", "--name", "lab", "--", "/usr/bin/true"});
    CHECK(created.ok());
    CHECK(!env.isolate({"snapshot", "create", "lab", "../escape"}).ok());
    CHECK(!env.isolate({"snapshot", "restore", "lab", "nosuch"}).ok());
}

TEST(lifecycle, destroy_removes_a_capsule_without_touching_the_host) {
    Environment env;
    env.write("project/file", "original\n");

    Run created = env.isolate({"run", "--name", "doomed", "--cow", env.path("project"), "--",
                               "/bin/sh", "-c", "echo changed > file"});
    CHECK(created.ok());

    Run destroyed = env.isolate({"destroy", "--force", "doomed"});
    CHECK(destroyed.ok());
    CHECK(env.isolate({"ls"}).out_contains("no capsules"));
    CHECK_EQ(env.read("project/file"), std::string("original\n"));
}

TEST(lifecycle, destroy_refuses_to_discard_uncommitted_changes) {
    Environment env;
    env.write("project/file", "original\n");
    Run created = env.isolate({"run", "--name", "unsaved", "--cow", env.path("project"),
                               "--", "/bin/sh", "-c", "echo changed > file"});
    CHECK(created.ok());

    Run destroyed = env.isolate({"destroy", "unsaved"});
    CHECK(!destroyed.ok());
    CHECK(destroyed.err_contains("uncommitted"));
    CHECK(env.isolate({"ls"}).out_contains("unsaved"));
}

TEST(lifecycle, destroy_proceeds_once_the_changes_are_committed) {
    Environment env;
    env.write("project/file", "original\n");
    Run created = env.isolate({"run", "--name", "saved", "--cow", env.path("project"), "--",
                               "/bin/sh", "-c", "echo changed > file"});
    CHECK(created.ok());
    CHECK(env.isolate({"commit", "saved"}).ok());

    Run destroyed = env.isolate({"destroy", "saved"});
    CHECK(destroyed.ok());
    CHECK_EQ(env.read("project/file"), std::string("changed\n"));
}

TEST(lifecycle, destroy_all_removes_every_capsule) {
    Environment env;
    CHECK(env.isolate({"run", "--name", "one", "--", "/usr/bin/true"}).ok());
    CHECK(env.isolate({"run", "--name", "two", "--", "/usr/bin/true"}).ok());

    Run destroyed = env.isolate({"destroy", "--all", "--force"});
    CHECK(destroyed.ok());
    CHECK(env.isolate({"ls"}).out_contains("no capsules"));
}

TEST(lifecycle, destroy_reports_a_capsule_that_does_not_exist) {
    Environment env;
    Run run = env.isolate({"destroy", "nosuch"});
    CHECK(!run.ok());
}

TEST(lifecycle, a_capsule_can_be_referred_to_by_identifier_or_name) {
    Environment env;
    Run created = env.isolate({"run", "--name", "named", "--", "/usr/bin/true"});
    CHECK(created.ok());

    std::string id = env.only_capsule();
    CHECK(env.isolate({"inspect", id}).ok());
    CHECK(env.isolate({"inspect", "named"}).ok());
}

TEST(lifecycle, run_count_and_last_status_are_recorded) {
    Environment env;
    CHECK(env.isolate({"run", "--name", "counted", "--", "/usr/bin/true"}).ok());
    CHECK_EQ(env.isolate({"enter", "counted", "--", "/bin/sh", "-c", "exit 7"}).status, 7);

    Run shown = env.isolate({"inspect", "--json", "counted"});
    json::Value parsed = json::parse(shown.out);
    CHECK_EQ(parsed.at("run_count").as_int(), int64_t(2));
    CHECK_EQ(parsed.at("last_exit_code").as_int(), int64_t(7));
}
