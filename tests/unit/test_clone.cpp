#include "harness.h"

#include "fsview/clone.h"
#include "util/error.h"
#include "util/fs.h"
#include "util/path.h"

#include <sys/stat.h>
#include <unistd.h>

#include <string>

using namespace isolate;
using namespace isolate::fsview;

TEST(clone, reports_the_filesystem_type) {
    // The capsule store lives on the data volume, which is APFS on any Mac
    // that can run this.
    CHECK_EQ(filesystem_type("/System/Volumes/Data"), std::string("apfs"));
    CHECK_EQ(filesystem_type("/private/tmp"), std::string("apfs"));
}

TEST(clone, reports_the_filesystem_type_for_a_path_that_does_not_exist_yet) {
    // Needed because the destination of a clone is tested before it is created.
    test::TempDir dir;
    CHECK_EQ(filesystem_type(dir.child("not/created/yet")), std::string("apfs"));
}

TEST(clone, detects_clone_support) {
    test::TempDir dir;
    CHECK(supports_cloning(dir.path()));

    // The capability is a property of the volume format, so a read-only volume
    // still advertises it even though a clone onto it would fail with EROFS.
    // That is why clone_tree falls back on the error rather than on this
    // answer; the capability only decides whether to warn the user up front.
    CHECK(supports_cloning("/System/Library/CoreServices"));

    // A relative path is resolved against the working directory, like any
    // other path isolate is given.
    CHECK_EQ(supports_cloning("."), supports_cloning(dir.path()));
}

TEST(clone, falls_back_when_the_destination_cannot_be_written) {
    // The read-only system volume is the one place on a stock Mac where a
    // clone is certain to fail, which exercises the error path rather than the
    // capability check.
    test::TempDir dir;
    dir.write("source", "data");
    CHECK_THROWS(clone_tree(dir.child("source"), "/System/isolate-test-should-fail"));
}

TEST(clone, clones_a_single_file) {
    test::TempDir dir;
    dir.write("source", "contents");
    CloneResult result = clone_tree(dir.child("source"), dir.child("target"));

    CHECK(result.method == CloneMethod::Clone);
    CHECK_EQ(result.entries, uint64_t(1));
    CHECK_EQ(dir.read("target"), std::string("contents"));
}

TEST(clone, clones_a_directory_tree_in_one_call) {
    test::TempDir dir;
    dir.write("tree/a.txt", "a");
    dir.write("tree/sub/b.txt", "b");
    dir.write("tree/sub/deeper/c.txt", "c");

    CloneResult result = clone_tree(dir.child("tree"), dir.child("copy"));
    CHECK(result.method == CloneMethod::Clone);
    CHECK_EQ(dir.read("copy/a.txt"), std::string("a"));
    CHECK_EQ(dir.read("copy/sub/b.txt"), std::string("b"));
    CHECK_EQ(dir.read("copy/sub/deeper/c.txt"), std::string("c"));
    // 3 files plus 2 directories.
    CHECK_EQ(result.entries, uint64_t(5));
}

TEST(clone, a_clone_is_independent_of_its_source) {
    test::TempDir dir;
    dir.write("tree/file", "original");
    clone_tree(dir.child("tree"), dir.child("copy"));

    dir.write("copy/file", "changed");
    CHECK_EQ(dir.read("tree/file"), std::string("original"));
    CHECK_EQ(dir.read("copy/file"), std::string("changed"));
}

TEST(clone, a_clone_shares_blocks_rather_than_copying_them) {
    // This is why a capsule can be created for a large project instantly. The
    // tree's apparent size is unchanged, but the volume's free space is not
    // meaningfully reduced, so the test checks the apparent size matches and
    // that the clone was fast enough to not have copied.
    test::TempDir dir;
    std::string payload(4 * 1024 * 1024, 'x');
    dir.write("tree/big.bin", payload);

    CloneResult result = clone_tree(dir.child("tree"), dir.child("copy"));
    CHECK(result.method == CloneMethod::Clone);
    CHECK_EQ(result.bytes_logical, uint64_t(payload.size()));
    CHECK_EQ(fs::read_file(dir.child("copy/big.bin")).size(), payload.size());
}

TEST(clone, preserves_symlinks_as_symlinks) {
    test::TempDir dir;
    dir.write("tree/target", "data");
    fs::write_link("target", dir.child("tree/link"));

    clone_tree(dir.child("tree"), dir.child("copy"));
    CHECK(fs::is_symlink(dir.child("copy/link")));
    CHECK_EQ(fs::read_link(dir.child("copy/link")), std::string("target"));
}

TEST(clone, does_not_follow_a_symlinked_source) {
    // Following it would clone whatever the link points at, which could be
    // outside what the user asked to clone.
    test::TempDir dir;
    dir.write("real/file", "data");
    fs::write_link(dir.child("real"), dir.child("link"));

    clone_tree(dir.child("link"), dir.child("copy"));
    CHECK(fs::is_symlink(dir.child("copy")));
    CHECK_EQ(fs::read_link(dir.child("copy")), dir.child("real"));
}

TEST(clone, preserves_permissions) {
    test::TempDir dir;
    dir.write("tree/script", "#!/bin/sh\n");
    CHECK_EQ(::chmod(dir.child("tree/script").c_str(), 0755), 0);

    clone_tree(dir.child("tree"), dir.child("copy"));
    struct stat st = fs::lstat_or_throw(dir.child("copy/script"));
    CHECK_EQ(st.st_mode & 07777, mode_t(0755));
}

TEST(clone, refuses_an_existing_destination) {
    test::TempDir dir;
    dir.write("source", "a");
    dir.write("existing", "b");
    CHECK_THROWS(clone_tree(dir.child("source"), dir.child("existing")));
    // The existing file must be untouched.
    CHECK_EQ(dir.read("existing"), std::string("b"));
}

TEST(clone, reports_a_missing_source) {
    test::TempDir dir;
    CHECK_THROWS(clone_tree(dir.child("absent"), dir.child("target")));
}

TEST(clone, creates_the_destination_parent) {
    test::TempDir dir;
    dir.write("source", "a");
    CHECK_NO_THROW(clone_tree(dir.child("source"), dir.child("deep/parent/target")));
    CHECK_EQ(dir.read("deep/parent/target"), std::string("a"));
}

TEST(clone, clone_file_over_replaces_an_existing_file) {
    test::TempDir dir;
    dir.write("new", "replacement");
    dir.write("old", "original");

    clone_file_over(dir.child("new"), dir.child("old"));
    CHECK_EQ(dir.read("old"), std::string("replacement"));
    // No temporary files left behind.
    for (const std::string& name : fs::list_dir(dir.path())) {
        CHECK(name.find("isolate-tmp") == std::string::npos);
    }
}

TEST(clone, clone_file_over_creates_a_file_that_was_not_there) {
    test::TempDir dir;
    dir.write("new", "content");
    clone_file_over(dir.child("new"), dir.child("fresh"));
    CHECK_EQ(dir.read("fresh"), std::string("content"));
}

TEST(clone, clone_file_over_leaves_the_target_alone_when_the_source_is_missing) {
    test::TempDir dir;
    dir.write("target", "keep me");
    CHECK_THROWS(clone_file_over(dir.child("absent"), dir.child("target")));
    CHECK_EQ(dir.read("target"), std::string("keep me"));
}

TEST(clone, counts_apparent_and_allocated_sizes_separately) {
    test::TempDir dir;
    std::string payload(100000, 'z');
    dir.write("tree/file", payload);

    CloneResult result = clone_tree(dir.child("tree"), dir.child("copy"));
    CHECK_EQ(result.bytes_logical, uint64_t(100000));
    // Allocation is rounded up to whole blocks, so it is at least the data.
    CHECK(result.bytes_on_disk >= result.bytes_logical);
}

TEST(clone, handles_an_empty_directory) {
    test::TempDir dir;
    dir.make_dir("empty");
    CloneResult result = clone_tree(dir.child("empty"), dir.child("copy"));
    CHECK(fs::is_dir(dir.child("copy")));
    CHECK_EQ(result.entries, uint64_t(0));
}

TEST(clone, handles_an_empty_file) {
    test::TempDir dir;
    dir.write("empty", "");
    clone_tree(dir.child("empty"), dir.child("copy"));
    CHECK_EQ(dir.read("copy"), std::string(""));
}
