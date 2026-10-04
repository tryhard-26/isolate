#include "harness.h"

#include "core/ids.h"
#include "core/store.h"
#include "fsview/layer.h"
#include "util/error.h"
#include "util/fs.h"
#include "util/path.h"

#include <sys/stat.h>
#include <unistd.h>

using namespace isolate;
using namespace isolate::fsview;

namespace {

struct Fixture {
    test::TempDir dir;
    Store store;
    std::unique_ptr<Capsule> capsule;
    std::string source;

    Fixture() : store(dir.child("store")) {
        source = dir.child("project");
        fs::mkdir_p(source);
        dir.write("project/src/main.c", "int main(void) { return 0; }\n");
        dir.write("project/README", "readme\n");

        Spec spec;
        spec.id = generate_capsule_id();
        spec.name = "fixture";
        spec.created_at = 1700000000;
        spec.command = {"/usr/bin/true"};

        Mount mount;
        mount.host = source;
        mount.access = Access::ReadWrite;
        mount.storage = Storage::Clone;
        mount.clone_name = "project";
        mount.primary = true;
        spec.mounts.push_back(mount);

        capsule = store.create(spec);
        prepare_layers(*capsule);
    }

    const Mount& mount() const { return capsule->spec().mounts.front(); }
    std::string layer(const std::string& relative) const {
        return path::join(capsule->layer_dir("project"), relative);
    }
    std::string host(const std::string& relative) const {
        return path::join(source, relative);
    }
};

} // namespace

TEST(layer, prepare_creates_a_working_layer_and_a_baseline) {
    Fixture f;
    CHECK(fs::is_dir(f.capsule->layer_dir("project")));
    CHECK(fs::is_dir(f.capsule->base_layer_dir("project")));
    CHECK_EQ(fs::read_file(f.layer("README")), std::string("readme\n"));
    CHECK_EQ(f.mount().guest, f.capsule->layer_dir("project"));
}

TEST(layer, prepare_is_idempotent_and_keeps_existing_contents) {
    Fixture f;
    fs::write_file_atomic(f.layer("from-a-previous-run"), "kept");

    PrepareResult result = prepare_layers(*f.capsule);
    CHECK_EQ(result.layers_created, 0);
    CHECK_EQ(result.layers_reused, 1);
    CHECK_EQ(fs::read_file(f.layer("from-a-previous-run")), std::string("kept"));
}

TEST(layer, prepare_reports_a_missing_source) {
    test::TempDir dir;
    Store store(dir.child("store"));
    Spec spec;
    spec.id = generate_capsule_id();
    spec.created_at = 1;
    spec.command = {"/usr/bin/true"};
    Mount mount;
    mount.host = dir.child("does-not-exist");
    mount.access = Access::ReadWrite;
    mount.storage = Storage::Clone;
    mount.clone_name = "missing";
    spec.mounts.push_back(mount);

    std::unique_ptr<Capsule> capsule = store.create(spec);
    CHECK_THROWS(prepare_layers(*capsule));
}

TEST(layer, prepare_rebuilds_a_lost_baseline_from_the_working_layer) {
    Fixture f;
    fs::remove_tree(f.capsule->base_layer_dir("project"));
    CHECK_NO_THROW(prepare_layers(*f.capsule));
    CHECK(fs::is_dir(f.capsule->base_layer_dir("project")));
    // Rebuilt from the layer, so the capsule's prior changes become baseline.
    CHECK(layer_diff(*f.capsule, f.mount()).empty());
}

TEST(layer, resolve_guest_paths_does_not_touch_the_filesystem) {
    Fixture f;
    Spec copy = f.capsule->spec();
    for (Mount& mount : copy.mounts) mount.guest.clear();
    resolve_guest_paths(copy, *f.capsule);
    CHECK_EQ(copy.mounts.front().guest, f.capsule->layer_dir("project"));
}

TEST(layer, a_fresh_layer_reports_no_changes) {
    Fixture f;
    CHECK(layer_diff(*f.capsule, f.mount()).empty());
    CHECK(host_drift(*f.capsule, f.mount()).empty());
}

TEST(layer, changes_in_the_layer_are_reported_against_the_baseline) {
    Fixture f;
    fs::write_file_atomic(f.layer("src/main.c"), "changed\n");
    fs::write_file_atomic(f.layer("added.txt"), "new\n");
    CHECK_EQ(::unlink(f.layer("README").c_str()), 0);

    Diff diff = layer_diff(*f.capsule, f.mount());
    CHECK_EQ(diff.count(ChangeKind::Modified), size_t(1));
    CHECK_EQ(diff.count(ChangeKind::Added), size_t(1));
    CHECK_EQ(diff.count(ChangeKind::Deleted), size_t(1));

    // Writes inside the capsule must not have reached the host.
    CHECK_EQ(fs::read_file(f.host("src/main.c")),
             std::string("int main(void) { return 0; }\n"));
    CHECK(fs::exists(f.host("README")));
}

TEST(layer, host_changes_are_reported_separately) {
    Fixture f;
    fs::write_file_atomic(f.host("README"), "edited on the host\n");

    CHECK(layer_diff(*f.capsule, f.mount()).empty());
    Diff drift = host_drift(*f.capsule, f.mount());
    CHECK_EQ(drift.changes.size(), size_t(1));
    CHECK_EQ(drift.changes[0].path, std::string("README"));
}

TEST(layer, operations_refuse_a_mount_that_is_not_a_clone) {
    Fixture f;
    Mount direct;
    direct.host = f.source;
    direct.access = Access::ReadWrite;
    direct.storage = Storage::Host;
    CHECK_THROWS(layer_diff(*f.capsule, direct));
    CHECK_THROWS(host_drift(*f.capsule, direct));
}

TEST(layer, commit_applies_changes_to_the_host) {
    Fixture f;
    fs::write_file_atomic(f.layer("src/main.c"), "committed\n");
    fs::write_file_atomic(f.layer("added.txt"), "new\n");
    CHECK_EQ(::unlink(f.layer("README").c_str()), 0);

    CommitPlan plan = plan_commit(*f.capsule, f.mount());
    CHECK_EQ(plan.apply.size(), size_t(3));
    CHECK(plan.conflicts.empty());

    CommitResult result = commit_layer(*f.capsule, f.mount(), plan, false);
    CHECK_EQ(result.applied, 3);
    CHECK(result.failures.empty());

    CHECK_EQ(fs::read_file(f.host("src/main.c")), std::string("committed\n"));
    CHECK_EQ(fs::read_file(f.host("added.txt")), std::string("new\n"));
    CHECK(!fs::exists(f.host("README")));
}

TEST(layer, commit_refreshes_the_baseline) {
    Fixture f;
    fs::write_file_atomic(f.layer("added.txt"), "new\n");
    CommitPlan plan = plan_commit(*f.capsule, f.mount());
    commit_layer(*f.capsule, f.mount(), plan, false);

    // After committing, the capsule and the host agree, so a second diff is
    // empty and a second commit has nothing to do.
    CHECK(layer_diff(*f.capsule, f.mount()).empty());
    CHECK(plan_commit(*f.capsule, f.mount()).empty());
}

TEST(layer, commit_withholds_paths_that_changed_on_the_host) {
    Fixture f;
    fs::write_file_atomic(f.layer("README"), "capsule version\n");
    fs::write_file_atomic(f.host("README"), "host version\n");

    CommitPlan plan = plan_commit(*f.capsule, f.mount());
    CHECK(plan.apply.empty());
    CHECK_EQ(plan.conflicts.size(), size_t(1));

    CommitResult result = commit_layer(*f.capsule, f.mount(), plan, false);
    CHECK_EQ(result.applied, 0);
    CHECK_EQ(result.skipped, 1);
    CHECK_EQ(fs::read_file(f.host("README")), std::string("host version\n"));
}

TEST(layer, commit_with_force_overwrites_a_conflict) {
    Fixture f;
    fs::write_file_atomic(f.layer("README"), "capsule version\n");
    fs::write_file_atomic(f.host("README"), "host version\n");

    CommitPlan plan = plan_commit(*f.capsule, f.mount());
    CommitResult result = commit_layer(*f.capsule, f.mount(), plan, true);
    CHECK_EQ(result.applied, 1);
    CHECK_EQ(fs::read_file(f.host("README")), std::string("capsule version\n"));
}

TEST(layer, commit_applies_non_conflicting_changes_alongside_conflicts) {
    Fixture f;
    fs::write_file_atomic(f.layer("README"), "capsule version\n");
    fs::write_file_atomic(f.host("README"), "host version\n");
    fs::write_file_atomic(f.layer("clean.txt"), "no conflict\n");

    CommitPlan plan = plan_commit(*f.capsule, f.mount());
    CommitResult result = commit_layer(*f.capsule, f.mount(), plan, false);
    CHECK_EQ(result.applied, 1);
    CHECK_EQ(result.skipped, 1);
    CHECK_EQ(fs::read_file(f.host("clean.txt")), std::string("no conflict\n"));
    CHECK_EQ(fs::read_file(f.host("README")), std::string("host version\n"));
}

TEST(layer, commit_creates_missing_parent_directories) {
    Fixture f;
    fs::mkdir_p(f.layer("deep/nested"), 0755);
    fs::write_file_atomic(f.layer("deep/nested/file.txt"), "content\n");

    CommitPlan plan = plan_commit(*f.capsule, f.mount());
    commit_layer(*f.capsule, f.mount(), plan, false);
    CHECK_EQ(fs::read_file(f.host("deep/nested/file.txt")), std::string("content\n"));
}

TEST(layer, commit_removes_a_deleted_directory_and_its_contents) {
    Fixture f;
    fs::remove_tree(f.layer("src"));

    CommitPlan plan = plan_commit(*f.capsule, f.mount());
    commit_layer(*f.capsule, f.mount(), plan, false);
    CHECK(!fs::exists(f.host("src")));
}

TEST(layer, commit_preserves_modes) {
    Fixture f;
    fs::write_file_atomic(f.layer("script.sh"), "#!/bin/sh\n");
    CHECK_EQ(::chmod(f.layer("script.sh").c_str(), 0755), 0);

    CommitPlan plan = plan_commit(*f.capsule, f.mount());
    commit_layer(*f.capsule, f.mount(), plan, false);

    struct stat st = fs::lstat_or_throw(f.host("script.sh"));
    CHECK_EQ(st.st_mode & 07777, mode_t(0755));
}

TEST(layer, commit_carries_symlinks_across) {
    Fixture f;
    fs::write_link("src/main.c", f.layer("shortcut"));

    CommitPlan plan = plan_commit(*f.capsule, f.mount());
    commit_layer(*f.capsule, f.mount(), plan, false);
    CHECK(fs::is_symlink(f.host("shortcut")));
    CHECK_EQ(fs::read_link(f.host("shortcut")), std::string("src/main.c"));
}

TEST(snapshot, create_and_restore_round_trip) {
    Fixture f;
    fs::write_file_atomic(f.layer("README"), "state at snapshot\n");
    snapshot_create(*f.capsule, "clean", "before experiments");

    // Wreck the layer thoroughly.
    fs::remove_tree(f.layer("src"));
    fs::write_file_atomic(f.layer("README"), "wrecked\n");
    fs::write_file_atomic(f.layer("junk"), "junk\n");

    snapshot_restore(*f.capsule, "clean");
    CHECK_EQ(fs::read_file(f.layer("README")), std::string("state at snapshot\n"));
    CHECK(fs::exists(f.layer("src/main.c")));
    CHECK(!fs::exists(f.layer("junk")));
}

TEST(snapshot, restore_also_restores_the_baseline) {
    Fixture f;
    snapshot_create(*f.capsule, "clean", "");
    fs::write_file_atomic(f.layer("added.txt"), "x\n");

    // Committing moves the baseline forward; restoring must move it back, or
    // the diff after a restore would be wrong.
    CommitPlan plan = plan_commit(*f.capsule, f.mount());
    commit_layer(*f.capsule, f.mount(), plan, false);
    CHECK(layer_diff(*f.capsule, f.mount()).empty());

    snapshot_restore(*f.capsule, "clean");
    CHECK(layer_diff(*f.capsule, f.mount()).empty());
    CHECK(!fs::exists(f.layer("added.txt")));
}

TEST(snapshot, list_reports_metadata_newest_first) {
    Fixture f;
    snapshot_create(*f.capsule, "first", "the first one");
    snapshot_create(*f.capsule, "second", "");

    std::vector<SnapshotInfo> snapshots = snapshot_list(*f.capsule);
    CHECK_EQ(snapshots.size(), size_t(2));
    bool found_note = false;
    for (const SnapshotInfo& info : snapshots) {
        if (info.name == "first") {
            found_note = info.note == "the first one";
            CHECK(info.created_at > 0);
        }
    }
    CHECK(found_note);
}

TEST(snapshot, create_rejects_a_duplicate_or_malformed_name) {
    Fixture f;
    snapshot_create(*f.capsule, "once", "");
    CHECK_THROWS(snapshot_create(*f.capsule, "once", ""));
    CHECK_THROWS(snapshot_create(*f.capsule, "../escape", ""));
    CHECK_THROWS(snapshot_create(*f.capsule, "has space", ""));
    CHECK_THROWS(snapshot_create(*f.capsule, "", ""));
}

TEST(snapshot, restore_and_delete_report_a_missing_snapshot) {
    Fixture f;
    CHECK_THROWS(snapshot_restore(*f.capsule, "nosuch"));
    CHECK_THROWS(snapshot_delete(*f.capsule, "nosuch"));
}

TEST(snapshot, delete_removes_only_that_snapshot) {
    Fixture f;
    snapshot_create(*f.capsule, "keep", "");
    snapshot_create(*f.capsule, "drop", "");
    snapshot_delete(*f.capsule, "drop");

    std::vector<SnapshotInfo> snapshots = snapshot_list(*f.capsule);
    CHECK_EQ(snapshots.size(), size_t(1));
    CHECK_EQ(snapshots[0].name, std::string("keep"));
}

TEST(snapshot, restore_refuses_an_incomplete_snapshot) {
    Fixture f;
    snapshot_create(*f.capsule, "broken", "");
    fs::remove_tree(path::join(f.capsule->snapshot_dir("broken"), "layers"));
    CHECK_THROWS(snapshot_restore(*f.capsule, "broken"));
}

TEST(snapshot, create_refuses_to_overwrite_an_existing_snapshot_directory) {
    Fixture f;
    std::string dir = f.capsule->snapshot_dir("taken");
    fs::mkdir_p(dir, 0700);
    fs::write_file_atomic(path::join(dir, "marker"), "mine");

    CHECK_THROWS(snapshot_create(*f.capsule, "taken", ""));
    // The existing directory is left exactly as it was.
    CHECK_EQ(fs::read_file(path::join(dir, "marker")), std::string("mine"));
}

TEST(snapshot, a_failed_create_leaves_no_partial_snapshot) {
    // A half-written snapshot would restore a tree that never existed, so the
    // directory has to be removed if any of its clones fail. Removing the
    // capsule's layers directory makes the first clone fail after the snapshot
    // directory has already been created.
    Fixture f;
    fs::remove_tree(f.capsule->layers_dir());

    CHECK_THROWS(snapshot_create(*f.capsule, "halfway", ""));
    CHECK(!fs::exists(f.capsule->snapshot_dir("halfway")));
}

TEST(snapshot, restore_clears_the_capsule_scratch_directory) {
    Fixture f;
    snapshot_create(*f.capsule, "clean", "");
    fs::write_file_atomic(path::join(f.capsule->tmp_dir(), "stale.sock"), "x");

    snapshot_restore(*f.capsule, "clean");
    CHECK(fs::is_dir(f.capsule->tmp_dir()));
    CHECK(!fs::exists(path::join(f.capsule->tmp_dir(), "stale.sock")));
}

TEST(layer, apparent_size_counts_the_capsule_tree) {
    Fixture f;
    CHECK(capsule_apparent_size(*f.capsule) > 0);
}
