#include "harness.h"

#include "util/fs.h"
#include "util/path.h"

#include <unistd.h>

using namespace isolate;
using namespace isolate::path;

TEST(path, normalize_collapses_separators_and_dots) {
    CHECK_EQ(normalize("/a//b/./c/"), std::string("/a/b/c"));
    CHECK_EQ(normalize("/a/b/../c"), std::string("/a/c"));
    CHECK_EQ(normalize("/"), std::string("/"));
    CHECK_EQ(normalize("//"), std::string("/"));
    CHECK_EQ(normalize(""), std::string("."));
    CHECK_EQ(normalize("."), std::string("."));
    CHECK_EQ(normalize("a/b"), std::string("a/b"));
}

TEST(path, normalize_clamps_dotdot_at_root) {
    // A policy path of "/../../etc" must not be allowed to escape to a
    // different prefix than the one the user sees.
    CHECK_EQ(normalize("/../../etc"), std::string("/etc"));
    CHECK_EQ(normalize("/a/../.."), std::string("/"));
    CHECK_EQ(normalize("../a"), std::string("../a"));
    CHECK_EQ(normalize("a/../../b"), std::string("../b"));
}

TEST(path, absolute_uses_cwd_or_base) {
    CHECK_EQ(absolute("/already", "/base"), std::string("/already"));
    CHECK_EQ(absolute("rel/x", "/base"), std::string("/base/rel/x"));
    CHECK_EQ(absolute("../x", "/base/sub"), std::string("/base/x"));
    CHECK_EQ(absolute("", "/base"), std::string("/base"));

    char cwd[1024];
    CHECK(getcwd(cwd, sizeof cwd) != nullptr);
    CHECK_EQ(absolute("x"), std::string(cwd) + "/x");
}

TEST(path, dirname_and_basename) {
    CHECK_EQ(dirname("/a/b/c"), std::string("/a/b"));
    CHECK_EQ(dirname("/a"), std::string("/"));
    CHECK_EQ(dirname("/"), std::string("/"));
    CHECK_EQ(dirname("a"), std::string("."));
    CHECK_EQ(basename("/a/b/c"), std::string("c"));
    CHECK_EQ(basename("/a/b/c/"), std::string("c"));
    CHECK_EQ(basename("/"), std::string("/"));
}

TEST(path, join_handles_absolute_second_argument) {
    CHECK_EQ(join("/a", "b"), std::string("/a/b"));
    CHECK_EQ(join("/a/", "/b"), std::string("/b"));
    CHECK_EQ(join("/a", ""), std::string("/a"));
    CHECK_EQ(join("", "b"), std::string("b"));
}

TEST(path, is_within_requires_a_component_boundary) {
    CHECK(is_within("/a/b", "/a/b"));
    CHECK(is_within("/a/b", "/a/b/c"));
    CHECK(is_within("/", "/anything"));
    // The important case: a sibling directory whose name merely starts with the
    // parent's name must not count as contained, or an --ro=/a/b policy would
    // leak /a/bc as well.
    CHECK(!is_within("/a/b", "/a/bc"));
    CHECK(!is_within("/a/b/c", "/a/b"));
    CHECK(!is_within("/x", "/y"));
}

TEST(path, relative_to_computes_suffix) {
    CHECK_EQ(relative_to("/a/b", "/a/b/c/d"), std::string("c/d"));
    CHECK_EQ(relative_to("/a/b", "/a/b"), std::string("."));
    CHECK_EQ(relative_to("/", "/etc"), std::string("etc"));
    CHECK_THROWS(relative_to("/a/b", "/a/bc"));
}

TEST(path, ancestors_lists_each_parent_directory) {
    auto a = ancestors("/a/b/c");
    CHECK_EQ(a.size(), size_t(3));
    CHECK_EQ(a[0], std::string("/"));
    CHECK_EQ(a[1], std::string("/a"));
    CHECK_EQ(a[2], std::string("/a/b"));

    CHECK_EQ(ancestors("/").size(), size_t(0));
    CHECK_EQ(ancestors("/a").size(), size_t(1));
}

TEST(path, resolve_follows_symlinks_and_firmlinks) {
    // /tmp is a symlink to /private/tmp on macOS. Sandbox rules are matched
    // against resolved paths, so policy paths have to be resolved the same way.
    CHECK_EQ(resolve("/tmp"), std::string("/private/tmp"));
    CHECK_EQ(resolve("/etc"), std::string("/private/etc"));
    CHECK_THROWS(resolve("/definitely/not/here"));
}

TEST(path, resolve_lenient_handles_missing_leaves) {
    test::TempDir dir;
    std::string missing = dir.child("nope/deeper");
    CHECK_EQ(resolve_lenient(missing), missing);

    fs::mkdir_p(dir.child("real"));
    CHECK_EQ(resolve_lenient(dir.child("real")), dir.child("real"));
}

TEST(path, resolve_lenient_resolves_an_existing_prefix) {
    test::TempDir dir;
    fs::mkdir_p(dir.child("target"));
    fs::write_link(dir.child("target"), dir.child("link"));
    CHECK_EQ(resolve_lenient(dir.child("link/child.txt")), dir.child("target/child.txt"));
}

TEST(path, self_executable_points_at_the_test_binary) {
    std::string exe = self_executable();
    CHECK(!exe.empty());
    CHECK_EQ(exe.front(), '/');
    CHECK(fs::exists(exe));
}

TEST(path, state_dir_honours_the_environment_override) {
    // The integration tests rely on this to keep capsules out of the real
    // ~/.local/share/isolate.
    const char* previous = std::getenv("ISOLATE_HOME");
    std::string saved = previous ? previous : "";

    setenv("ISOLATE_HOME", "/private/tmp/isolate-state-test", 1);
    CHECK_EQ(state_dir(), std::string("/private/tmp/isolate-state-test"));

    if (previous) {
        setenv("ISOLATE_HOME", saved.c_str(), 1);
    } else {
        unsetenv("ISOLATE_HOME");
    }
}
