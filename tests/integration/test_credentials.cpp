#include "harness.h"
#include "runner.h"

#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace isolate::test;

// --user needs root. Run the suite with sudo to exercise the drop:
//   sudo ./build/tests/isolate_integration_tests credentials

namespace {

bool as_root() { return ::geteuid() == 0; }

// uid of a file, or -1.
long owner_of(const std::string& p) {
    struct stat st {};
    if (::lstat(p.c_str(), &st) != 0) return -1;
    return static_cast<long>(st.st_uid);
}

long uid_of(const char* user) {
    const passwd* pw = ::getpwnam(user);
    return pw ? static_cast<long>(pw->pw_uid) : -1;
}

} // namespace

TEST(credentials, user_is_refused_without_root) {
    if (as_root()) return;
    Environment env;
    Run run = env.isolate({"run", "--ephemeral", "--user", "nobody", "--", "/usr/bin/true"});
    CHECK(!run.ok());
    CHECK(run.err_contains("needs root"));
}

TEST(credentials, unknown_user_is_refused) {
    if (!as_root()) return;
    Environment env;
    Run run = env.isolate({"run", "--ephemeral", "--user", "nosuchuser9z", "--",
                           "/usr/bin/true"});
    CHECK(!run.ok());
    CHECK(run.err_contains("no such user"));
}

TEST(credentials, the_capsule_runs_as_the_named_user) {
    if (!as_root()) return;
    Environment env;
    Run run = env.isolate({"run", "--ephemeral", "--user", "nobody", "--", "/usr/bin/id",
                           "-un"});
    CHECK(run.ok());
    CHECK(run.out_contains("nobody"));
}

TEST(credentials, the_dropped_user_can_write_its_own_areas) {
    // The bug this guards: root creates the capsule 0700, so the dropped
    // user could reach none of it and --user was useless.
    if (!as_root()) return;
    Environment env;
    env.write("project/file", "v1\n");

    Run run = env.isolate({"run", "--ephemeral", "--user", "nobody", "--cow",
                           env.path("project"), "--", "/bin/sh", "-c",
                           "pwd >/dev/null && echo cwd-ok; "
                           "echo x > added && echo layer-ok; "
                           "touch $HOME/f && echo home-ok; "
                           "touch $TMPDIR/f && echo tmp-ok"});
    CHECK(run.ok());
    CHECK(run.out_contains("cwd-ok"));
    CHECK(run.out_contains("layer-ok"));
    CHECK(run.out_contains("home-ok"));
    CHECK(run.out_contains("tmp-ok"));
}

TEST(credentials, the_dropped_user_still_cannot_read_capsule_metadata) {
    // Opening traversal for --user must not open the policy or the
    // baseline, or the capsule could rewrite what diff reports.
    if (!as_root()) return;
    Environment env;
    env.write("project/file", "v1\n");

    Run run = env.isolate({"run", "--ephemeral", "--user", "nobody", "--cow",
                           env.path("project"), "--", "/bin/sh", "-c",
                           "cat ../../capsule.json 2>/dev/null && echo POLICY || "
                           "echo policy-denied; "
                           "ls ../../base 2>/dev/null && echo BASE || echo base-denied; "
                           "ls ../../snapshots 2>/dev/null && echo SNAPS || "
                           "echo snaps-denied"});
    CHECK(run.out_contains("policy-denied"));
    CHECK(run.out_contains("base-denied"));
    CHECK(run.out_contains("snaps-denied"));
    CHECK(!run.contains("POLICY"));
    CHECK(!run.contains("BASE"));
}

TEST(credentials, the_host_is_untouched_by_a_dropped_capsule) {
    if (!as_root()) return;
    Environment env;
    env.write("project/file", "v1\n");

    Run run = env.isolate({"run", "--ephemeral", "--user", "nobody", "--cow",
                           env.path("project"), "--", "/bin/sh", "-c",
                           "echo v2 > file; echo new > added"});
    CHECK(run.ok());
    CHECK_EQ(env.read("project/file"), std::string("v1\n"));
    CHECK(!env.exists("project/added"));
}

TEST(credentials, diff_does_not_report_the_chown_as_a_change) {
    // isolate chowns the layer to the dropped uid, so uid/gid differ from
    // the baseline for every entry. That is isolate's doing, not the
    // capsule's, and must not show up.
    if (!as_root()) return;
    Environment env;
    env.write("project/a", "a\n");
    env.write("project/sub/b", "b\n");

    Run created = env.isolate({"run", "--name", "dropped", "--user", "nobody", "--cow",
                               env.path("project"), "--", "/usr/bin/true"});
    CHECK(created.ok());

    Run diff = env.isolate({"diff", "dropped"});
    CHECK(diff.out_contains("no changes"));
}

TEST(credentials, commit_keeps_the_host_owner) {
    // Committing a layer owned by nobody must not hand the user's own
    // files to nobody.
    if (!as_root()) return;
    Environment env;
    env.write("project/file", "v1\n");
    long before = owner_of(env.path("project/file"));
    CHECK(before >= 0);

    Run run = env.isolate({"run", "--name", "dropped", "--user", "nobody", "--cow",
                           env.path("project"), "--", "/bin/sh", "-c",
                           "echo v2 > file; echo new > added; mkdir -p out; echo o > out/o"});
    CHECK(run.ok());
    CHECK(env.isolate({"commit", "dropped"}).ok());

    CHECK_EQ(env.read("project/file"), std::string("v2\n"));
    CHECK_EQ(owner_of(env.path("project/file")), before);
    CHECK_EQ(owner_of(env.path("project/added")), before);
    CHECK_EQ(owner_of(env.path("project/out")), before);
    CHECK_EQ(owner_of(env.path("project/out/o")), before);
    CHECK_NE(owner_of(env.path("project/added")), uid_of("nobody"));
}

TEST(credentials, the_dropped_capsule_cannot_regain_root) {
    if (!as_root()) return;
    Environment env;
    Run run = env.isolate({"run", "--ephemeral", "--user", "nobody", "--", "/bin/sh", "-c",
                           "/usr/bin/sudo -n /usr/bin/id 2>/dev/null && echo ROOT || "
                           "echo denied"});
    CHECK(run.out_contains("denied"));
    CHECK(!run.contains("uid=0"));
}

TEST(credentials, the_policy_records_the_user) {
    if (!as_root()) return;
    Environment env;
    Run created = env.isolate({"run", "--name", "dropped", "--user", "nobody", "--",
                               "/usr/bin/true"});
    CHECK(created.ok());
    Run shown = env.isolate({"inspect", "dropped"});
    CHECK(shown.out_contains("nobody"));
}

TEST(credentials, umask_is_applied_in_the_capsule) {
    Environment env;
    Run run = env.isolate({"run", "--ephemeral", "--umask", "022", "--", "/bin/sh", "-c",
                           "/usr/bin/touch $TMPDIR/f && /bin/ls -l $TMPDIR/f"});
    CHECK(run.ok());
    CHECK(run.out_contains("rw-r--r--"));
}
