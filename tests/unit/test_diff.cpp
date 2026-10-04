#include "harness.h"

#include "fsview/clone.h"
#include "fsview/diff.h"
#include "util/fs.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

using namespace isolate;
using namespace isolate::fsview;

namespace {

const Change* find(const Diff& diff, const std::string& path) {
    for (const Change& change : diff.changes) {
        if (change.path == path) return &change;
    }
    return nullptr;
}

} // namespace

TEST(diff, identical_trees_report_no_changes) {
    test::TempDir dir;
    dir.write("base/a.txt", "a");
    dir.write("base/sub/b.txt", "b");
    clone_tree(dir.child("base"), dir.child("copy"));

    Diff diff = diff_trees(dir.child("base"), dir.child("copy"));
    CHECK(diff.empty());
    CHECK_EQ(diff.stats.compared, uint64_t(3));
}

TEST(diff, an_untouched_clone_is_proven_unchanged_without_reading_data) {
    // The point of the extent comparison: no file contents are read when
    // nothing has been written.
    test::TempDir dir;
    for (int i = 0; i < 10; ++i) {
        dir.write("base/f" + std::to_string(i), std::string(64 * 1024, 'x'));
    }
    clone_tree(dir.child("base"), dir.child("copy"));

    Diff diff = diff_trees(dir.child("base"), dir.child("copy"));
    CHECK(diff.empty());
    CHECK_EQ(diff.stats.proved_by_extents, uint64_t(10));
    CHECK_EQ(diff.stats.read_to_compare, uint64_t(0));
}

TEST(diff, detects_added_files) {
    test::TempDir dir;
    dir.write("base/kept", "k");
    clone_tree(dir.child("base"), dir.child("copy"));
    dir.write("copy/added", "new");

    Diff diff = diff_trees(dir.child("base"), dir.child("copy"));
    const Change* change = find(diff, "added");
    CHECK(change != nullptr);
    CHECK(change->kind == ChangeKind::Added);
    CHECK_EQ(change->size, uint64_t(3));
    CHECK_EQ(diff.stats.bytes_added, uint64_t(3));
}

TEST(diff, detects_deleted_files) {
    test::TempDir dir;
    dir.write("base/gone", "bye");
    clone_tree(dir.child("base"), dir.child("copy"));
    CHECK_EQ(::unlink(dir.child("copy/gone").c_str()), 0);

    Diff diff = diff_trees(dir.child("base"), dir.child("copy"));
    const Change* change = find(diff, "gone");
    CHECK(change != nullptr);
    CHECK(change->kind == ChangeKind::Deleted);
    CHECK_EQ(diff.stats.bytes_removed, uint64_t(3));
}

TEST(diff, detects_modified_files) {
    test::TempDir dir;
    dir.write("base/file", "before");
    clone_tree(dir.child("base"), dir.child("copy"));
    dir.write("copy/file", "after!");

    Diff diff = diff_trees(dir.child("base"), dir.child("copy"));
    const Change* change = find(diff, "file");
    CHECK(change != nullptr);
    CHECK(change->kind == ChangeKind::Modified);
}

TEST(diff, a_rewrite_with_the_same_contents_is_not_a_change) {
    // Block sharing is broken, so the extent comparison says Different, and the
    // byte comparison has to settle it. Reporting this as modified would make
    // every build that rewrites unchanged files look destructive.
    test::TempDir dir;
    dir.write("base/file", std::string(128 * 1024, 'x'));
    clone_tree(dir.child("base"), dir.child("copy"));
    dir.write("copy/file", std::string(128 * 1024, 'x'));

    Diff diff = diff_trees(dir.child("base"), dir.child("copy"));
    CHECK(diff.empty());
    CHECK_EQ(diff.stats.read_to_compare, uint64_t(1));
}

TEST(diff, a_same_size_different_content_change_is_found) {
    test::TempDir dir;
    dir.write("base/file", std::string(4096, 'a'));
    clone_tree(dir.child("base"), dir.child("copy"));
    dir.write("copy/file", std::string(4096, 'b'));

    Diff diff = diff_trees(dir.child("base"), dir.child("copy"));
    CHECK_EQ(diff.changes.size(), size_t(1));
    CHECK(diff.changes[0].kind == ChangeKind::Modified);
}

TEST(diff, detects_a_type_change) {
    test::TempDir dir;
    dir.write("base/thing", "file");
    clone_tree(dir.child("base"), dir.child("copy"));
    CHECK_EQ(::unlink(dir.child("copy/thing").c_str()), 0);
    fs::mkdir_p(dir.child("copy/thing"));

    Diff diff = diff_trees(dir.child("base"), dir.child("copy"));
    const Change* change = find(diff, "thing");
    CHECK(change != nullptr);
    CHECK(change->kind == ChangeKind::TypeChanged);
    CHECK(change->detail.find("file") != std::string::npos);
    CHECK(change->detail.find("directory") != std::string::npos);
}

TEST(diff, detects_a_retargeted_symlink) {
    test::TempDir dir;
    dir.write("base/a", "a");
    dir.write("base/b", "b");
    fs::write_link("a", dir.child("base/link"));
    clone_tree(dir.child("base"), dir.child("copy"));

    CHECK_EQ(::unlink(dir.child("copy/link").c_str()), 0);
    fs::write_link("b", dir.child("copy/link"));

    Diff diff = diff_trees(dir.child("base"), dir.child("copy"));
    const Change* change = find(diff, "link");
    CHECK(change != nullptr);
    CHECK(change->kind == ChangeKind::Modified);
}

TEST(diff, detects_a_mode_change_as_metadata_only) {
    test::TempDir dir;
    dir.write("base/script", "#!/bin/sh\n");
    clone_tree(dir.child("base"), dir.child("copy"));
    CHECK_EQ(::chmod(dir.child("copy/script").c_str(), 0755), 0);

    Diff diff = diff_trees(dir.child("base"), dir.child("copy"));
    const Change* change = find(diff, "script");
    CHECK(change != nullptr);
    CHECK(change->kind == ChangeKind::Metadata);
    CHECK(change->detail.find("0755") != std::string::npos);
}

TEST(diff, metadata_changes_can_be_excluded) {
    test::TempDir dir;
    dir.write("base/script", "x");
    clone_tree(dir.child("base"), dir.child("copy"));
    CHECK_EQ(::chmod(dir.child("copy/script").c_str(), 0700), 0);

    DiffOptions options;
    options.include_metadata = false;
    CHECK(diff_trees(dir.child("base"), dir.child("copy"), options).empty());
}

TEST(diff, a_wholly_added_directory_is_one_change) {
    // A new build directory with a thousand files is one thing that happened,
    // not a thousand.
    test::TempDir dir;
    dir.write("base/keep", "k");
    clone_tree(dir.child("base"), dir.child("copy"));
    for (int i = 0; i < 5; ++i) {
        dir.write("copy/out/deep/f" + std::to_string(i), "data");
    }

    Diff diff = diff_trees(dir.child("base"), dir.child("copy"));
    CHECK_EQ(diff.changes.size(), size_t(1));
    CHECK_EQ(diff.changes[0].path, std::string("out"));
    CHECK(diff.changes[0].is_dir);
    CHECK(diff.changes[0].kind == ChangeKind::Added);
    // "deep" plus five files.
    CHECK_EQ(diff.changes[0].contained_entries, uint64_t(6));
    CHECK_EQ(diff.changes[0].size, uint64_t(20));
}

TEST(diff, a_wholly_deleted_directory_is_one_change) {
    test::TempDir dir;
    for (int i = 0; i < 4; ++i) dir.write("base/sub/f" + std::to_string(i), "x");
    clone_tree(dir.child("base"), dir.child("copy"));
    fs::remove_tree(dir.child("copy/sub"));

    Diff diff = diff_trees(dir.child("base"), dir.child("copy"));
    CHECK_EQ(diff.changes.size(), size_t(1));
    CHECK_EQ(diff.changes[0].path, std::string("sub"));
    CHECK(diff.changes[0].kind == ChangeKind::Deleted);
    CHECK_EQ(diff.changes[0].contained_entries, uint64_t(4));
}

TEST(diff, a_modified_file_inside_an_added_directory_is_not_collapsed_away) {
    // Only entries of the same kind collapse into the directory, so a change
    // that is not simply "this appeared" stays visible.
    test::TempDir dir;
    dir.write("base/keep", "k");
    clone_tree(dir.child("base"), dir.child("copy"));
    dir.write("copy/new/file", "x");
    dir.write("copy/other", "y");

    Diff diff = diff_trees(dir.child("base"), dir.child("copy"));
    CHECK_EQ(diff.changes.size(), size_t(2));
    CHECK(find(diff, "new") != nullptr);
    CHECK(find(diff, "other") != nullptr);
}

TEST(diff, changes_are_sorted_by_path) {
    test::TempDir dir;
    dir.write("base/keep", "k");
    clone_tree(dir.child("base"), dir.child("copy"));
    dir.write("copy/zebra", "z");
    dir.write("copy/apple", "a");
    dir.write("copy/mango", "m");

    Diff diff = diff_trees(dir.child("base"), dir.child("copy"));
    std::vector<std::string> paths;
    for (const Change& change : diff.changes) paths.push_back(change.path);
    CHECK(std::is_sorted(paths.begin(), paths.end()));
}

TEST(diff, exclusions_are_skipped) {
    test::TempDir dir;
    dir.write("base/keep", "k");
    clone_tree(dir.child("base"), dir.child("copy"));
    dir.write("copy/ignored/file", "x");
    dir.write("copy/counted", "y");

    DiffOptions options;
    options.exclude = {"ignored"};
    Diff diff = diff_trees(dir.child("base"), dir.child("copy"), options);
    CHECK_EQ(diff.changes.size(), size_t(1));
    CHECK_EQ(diff.changes[0].path, std::string("counted"));
}

TEST(diff, a_missing_new_tree_reports_everything_as_deleted) {
    test::TempDir dir;
    dir.write("base/a", "a");
    dir.write("base/b", "b");

    Diff diff = diff_trees(dir.child("base"), dir.child("absent"));
    CHECK_EQ(diff.changes.size(), size_t(2));
    CHECK(diff.changes[0].kind == ChangeKind::Deleted);
}

TEST(diff, a_missing_old_tree_reports_everything_as_added) {
    test::TempDir dir;
    dir.write("new/a", "a");
    Diff diff = diff_trees(dir.child("absent"), dir.child("new"));
    CHECK_EQ(diff.changes.size(), size_t(1));
    CHECK(diff.changes[0].kind == ChangeKind::Added);
}

TEST(diff, two_missing_trees_report_nothing) {
    test::TempDir dir;
    CHECK(diff_trees(dir.child("a"), dir.child("b")).empty());
}

TEST(diff, counts_by_kind) {
    test::TempDir dir;
    dir.write("base/modified", "before");
    dir.write("base/deleted", "x");
    clone_tree(dir.child("base"), dir.child("copy"));
    dir.write("copy/modified", "after!");
    CHECK_EQ(::unlink(dir.child("copy/deleted").c_str()), 0);
    dir.write("copy/added", "new");

    Diff diff = diff_trees(dir.child("base"), dir.child("copy"));
    CHECK_EQ(diff.count(ChangeKind::Added), size_t(1));
    CHECK_EQ(diff.count(ChangeKind::Modified), size_t(1));
    CHECK_EQ(diff.count(ChangeKind::Deleted), size_t(1));
    CHECK_EQ(diff.count(ChangeKind::TypeChanged), size_t(0));
}

TEST(diff, markers_are_stable) {
    CHECK_EQ(marker(ChangeKind::Added), 'A');
    CHECK_EQ(marker(ChangeKind::Deleted), 'D');
    CHECK_EQ(marker(ChangeKind::Modified), 'M');
    CHECK_EQ(marker(ChangeKind::TypeChanged), 'T');
    CHECK_EQ(marker(ChangeKind::Metadata), 'm');
}

TEST(diff, does_not_follow_symlinks_out_of_the_tree) {
    // A symlink to somewhere else must be compared as a link, not walked into,
    // or a capsule could make a diff traverse the whole filesystem.
    test::TempDir dir;
    dir.write("outside/secret", "data");
    dir.write("base/keep", "k");
    clone_tree(dir.child("base"), dir.child("copy"));
    fs::write_link(dir.child("outside"), dir.child("copy/escape"));

    Diff diff = diff_trees(dir.child("base"), dir.child("copy"));
    CHECK_EQ(diff.changes.size(), size_t(1));
    CHECK_EQ(diff.changes[0].path, std::string("escape"));
    CHECK_EQ(diff.changes[0].contained_entries, uint64_t(0));
}
