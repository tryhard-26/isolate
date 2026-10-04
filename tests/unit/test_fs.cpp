#include "harness.h"

#include "util/fs.h"
#include "util/path.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

using namespace isolate;
using namespace isolate::fs;

TEST(fs, fd_closes_and_moves) {
    test::TempDir dir;
    dir.write("f", "x");

    int raw = -1;
    {
        Fd fd(::open(dir.child("f").c_str(), O_RDONLY));
        CHECK(fd.valid());
        raw = fd.get();

        Fd moved = std::move(fd);
        CHECK(moved.valid());
        CHECK(!fd.valid());
        CHECK_EQ(moved.get(), raw);
    }
    // After the scope the descriptor must be closed.
    CHECK_EQ(::fcntl(raw, F_GETFD), -1);
}

TEST(fs, fd_release_hands_off_ownership) {
    test::TempDir dir;
    dir.write("f", "x");
    int raw = -1;
    {
        Fd fd(::open(dir.child("f").c_str(), O_RDONLY));
        raw = fd.release();
        CHECK(!fd.valid());
    }
    CHECK_NE(::fcntl(raw, F_GETFD), -1);
    ::close(raw);
}

TEST(fs, mkdir_p_creates_intermediate_directories) {
    test::TempDir dir;
    mkdir_p(dir.child("a/b/c"));
    CHECK(is_dir(dir.child("a/b/c")));
    // Idempotent.
    CHECK_NO_THROW(mkdir_p(dir.child("a/b/c")));
}

TEST(fs, mkdir_p_fails_when_a_component_is_a_file) {
    test::TempDir dir;
    dir.write("file", "x");
    CHECK_THROWS(mkdir_p(dir.child("file/under")));
}

TEST(fs, write_file_atomic_replaces_contents) {
    test::TempDir dir;
    std::string target = dir.child("state.json");
    write_file_atomic(target, "first");
    CHECK_EQ(read_file(target), std::string("first"));
    write_file_atomic(target, "second");
    CHECK_EQ(read_file(target), std::string("second"));

    // No temporary files left behind.
    for (const std::string& name : list_dir(dir.path())) {
        CHECK(name.find(".tmp.") == std::string::npos);
    }
}

TEST(fs, write_file_atomic_honours_mode) {
    test::TempDir dir;
    std::string target = dir.child("secret");
    write_file_atomic(target, "x", 0600);
    struct stat st = lstat_or_throw(target);
    CHECK_EQ(st.st_mode & 07777, mode_t(0600));
}

TEST(fs, read_file_reports_missing_paths) {
    test::TempDir dir;
    CHECK_THROWS(read_file(dir.child("absent")));
}

TEST(fs, read_file_handles_large_contents) {
    test::TempDir dir;
    std::string big(300000, 'a');
    write_file_atomic(dir.child("big"), big);
    CHECK_EQ(read_file(dir.child("big")).size(), big.size());
}

TEST(fs, symlink_round_trip) {
    test::TempDir dir;
    dir.write("target", "x");
    write_link("target", dir.child("link"));
    CHECK(is_symlink(dir.child("link")));
    CHECK_EQ(read_link(dir.child("link")), std::string("target"));
    // is_dir follows symlinks; is_symlink does not.
    CHECK(!is_symlink(dir.child("target")));
}

TEST(fs, lstat_opt_returns_nothing_for_missing_paths) {
    test::TempDir dir;
    CHECK(!lstat_opt(dir.child("absent")).has_value());
    dir.write("here", "x");
    CHECK(lstat_opt(dir.child("here")).has_value());
}

TEST(fs, walk_visits_entries_depth_first_and_sorted) {
    test::TempDir dir;
    dir.write("b/2.txt", "x");
    dir.write("b/1.txt", "x");
    dir.write("a.txt", "x");
    write_link("a.txt", dir.child("l"));

    std::vector<std::string> seen;
    walk(dir.path(), [&](const Entry& e) {
        seen.push_back(e.relative);
        return true;
    });

    std::vector<std::string> expected{"a.txt", "b", "b/1.txt", "b/2.txt", "l"};
    CHECK_EQ(seen, expected);
}

TEST(fs, walk_reports_kinds) {
    test::TempDir dir;
    dir.write("file", "x");
    dir.make_dir("directory");
    write_link("file", dir.child("link"));

    std::vector<std::pair<std::string, Kind>> kinds;
    walk(dir.path(), [&](const Entry& e) {
        kinds.emplace_back(e.relative, e.kind);
        return true;
    });

    CHECK_EQ(kinds.size(), size_t(3));
    CHECK(kinds[0].second == Kind::Dir);    // "directory"
    CHECK(kinds[1].second == Kind::File);   // "file"
    CHECK(kinds[2].second == Kind::Symlink); // "link"
}

TEST(fs, walk_does_not_follow_directory_symlinks) {
    test::TempDir dir;
    dir.write("real/inside.txt", "x");
    write_link(dir.child("real"), dir.child("alias"));

    std::vector<std::string> seen;
    walk(dir.path(), [&](const Entry& e) {
        seen.push_back(e.relative);
        return true;
    });

    // "alias" appears once as a symlink, and its target is not walked through
    // it. Following it would make a commit copy the same tree twice, and a
    // symlink loop would never terminate.
    std::vector<std::string> expected{"alias", "real", "real/inside.txt"};
    CHECK_EQ(seen, expected);
}

TEST(fs, walk_skips_contents_when_visitor_returns_false) {
    test::TempDir dir;
    dir.write("skipme/deep/file", "x");
    dir.write("keep.txt", "x");

    std::vector<std::string> seen;
    walk(dir.path(), [&](const Entry& e) {
        seen.push_back(e.relative);
        return e.relative != "skipme";
    });

    std::vector<std::string> expected{"keep.txt", "skipme"};
    CHECK_EQ(seen, expected);
}

TEST(fs, walk_tolerates_a_missing_root) {
    test::TempDir dir;
    int count = 0;
    CHECK_NO_THROW(walk(dir.child("absent"), [&](const Entry&) {
        ++count;
        return true;
    }));
    CHECK_EQ(count, 0);
}

TEST(fs, remove_tree_deletes_nested_contents) {
    test::TempDir dir;
    dir.write("tree/a/b/c.txt", "x");
    write_link("c.txt", dir.child("tree/a/b/link"));
    remove_tree(dir.child("tree"));
    CHECK(!exists(dir.child("tree")));
}

TEST(fs, remove_tree_ignores_missing_paths) {
    test::TempDir dir;
    CHECK_NO_THROW(remove_tree(dir.child("absent")));
}

TEST(fs, remove_tree_refuses_dangerous_paths) {
    CHECK_THROWS(remove_tree("/"));
    CHECK_THROWS(remove_tree(""));
}

TEST(fs, remove_tree_clears_bsd_flags_that_block_removal) {
    // A program inside a capsule can chflags its own files; the capsule still
    // has to be destroyable afterwards.
    test::TempDir dir;
    dir.write("tree/locked", "x");
    CHECK_EQ(::chflags(dir.child("tree/locked").c_str(), UF_IMMUTABLE), 0);
    CHECK_NO_THROW(remove_tree(dir.child("tree")));
    CHECK(!exists(dir.child("tree")));
}

TEST(fs, remove_tree_does_not_follow_a_symlinked_directory) {
    test::TempDir dir;
    dir.write("outside/precious.txt", "keep me");
    dir.make_dir("tree");
    write_link(dir.child("outside"), dir.child("tree/alias"));

    remove_tree(dir.child("tree"));
    CHECK(!exists(dir.child("tree")));
    CHECK(exists(dir.child("outside/precious.txt")));
}

TEST(fs, tree_apparent_size_counts_allocated_blocks) {
    test::TempDir dir;
    CHECK_EQ(tree_apparent_size(dir.child("absent")), uint64_t(0));

    dir.write("a", std::string(8192, 'x'));
    uint64_t used = tree_apparent_size(dir.path());
    CHECK(used >= 8192);
}

TEST(fs, copy_metadata_transfers_mode_and_times) {
    test::TempDir dir;
    dir.write("src", "x");
    dir.write("dst", "y");
    CHECK_EQ(::chmod(dir.child("src").c_str(), 0640), 0);

    struct stat src = lstat_or_throw(dir.child("src"));
    copy_metadata(src, dir.child("dst"));

    struct stat dst = lstat_or_throw(dir.child("dst"));
    CHECK_EQ(dst.st_mode & 07777, mode_t(0640));
    CHECK_EQ(dst.st_mtimespec.tv_sec, src.st_mtimespec.tv_sec);
}

TEST(fs, list_dir_is_sorted_and_excludes_dot_entries) {
    test::TempDir dir;
    dir.write("c", "x");
    dir.write("a", "x");
    dir.make_dir("b");

    std::vector<std::string> expected{"a", "b", "c"};
    CHECK_EQ(list_dir(dir.path()), expected);
    CHECK_EQ(list_dir(dir.child("absent")).size(), size_t(0));
}
