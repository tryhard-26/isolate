#include "harness.h"

#include "core/ids.h"
#include "core/store.h"
#include "util/error.h"
#include "util/fs.h"
#include "util/path.h"

#include <sys/wait.h>
#include <unistd.h>

using namespace isolate;

namespace {

Spec spec_for(const std::string& name) {
    Spec spec;
    spec.id = generate_capsule_id();
    spec.name = name;
    spec.created_at = 1700000000;
    spec.command = {"/usr/bin/true"};
    return spec;
}

} // namespace

TEST(store, create_lays_out_the_capsule_directory) {
    test::TempDir dir;
    Store store(dir.path());
    Spec spec = spec_for("first");

    std::unique_ptr<Capsule> capsule = store.create(spec);
    CHECK(capsule->locked());
    CHECK(fs::is_dir(capsule->dir()));
    CHECK(fs::exists(capsule->spec_path()));
    CHECK(fs::exists(capsule->state_path()));
    CHECK(fs::is_dir(capsule->layers_dir()));
    CHECK(fs::is_dir(capsule->base_dir()));
    CHECK(fs::is_dir(capsule->snapshots_dir()));
    CHECK(fs::is_dir(capsule->home_dir()));
    CHECK(fs::is_dir(capsule->tmp_dir()));
}

TEST(store, capsule_contents_are_private_to_the_user) {
    test::TempDir dir;
    Store store(dir.path());
    std::unique_ptr<Capsule> capsule = store.create(spec_for("private"));

    // A layer can hold a copy of the user's files, so nothing in the tree may
    // be group or world readable.
    struct stat st = fs::lstat_or_throw(capsule->dir());
    CHECK_EQ(st.st_mode & 07777, mode_t(0700));
    struct stat spec_st = fs::lstat_or_throw(capsule->spec_path());
    CHECK_EQ(spec_st.st_mode & 07777, mode_t(0600));
}

TEST(store, create_refuses_a_duplicate_identifier) {
    test::TempDir dir;
    Store store(dir.path());
    Spec spec = spec_for("dup");
    store.create(spec);
    CHECK_THROWS(store.create(spec));
}

TEST(store, open_by_identifier_and_by_name) {
    test::TempDir dir;
    Store store(dir.path());
    Spec spec = spec_for("byname");
    std::string id = spec.id;
    store.create(spec)->save_spec();

    std::unique_ptr<Capsule> by_id = store.open(id, false);
    CHECK_EQ(by_id->spec().id, id);

    std::unique_ptr<Capsule> by_name = store.open("byname", false);
    CHECK_EQ(by_name->spec().id, id);
}

TEST(store, open_reports_missing_and_malformed_references) {
    test::TempDir dir;
    Store store(dir.path());
    CHECK_THROWS(store.open("", false));
    CHECK_THROWS(store.open("nosuchname", false));
    CHECK_THROWS(store.open("0123456789ab", false));
    CHECK_THROWS(store.open("../escape", false));
    CHECK_THROWS(store.open("has space", false));
}

TEST(store, open_reports_an_ambiguous_name) {
    test::TempDir dir;
    Store store(dir.path());
    store.create(spec_for("same"));
    store.create(spec_for("same"));

    bool threw = false;
    try {
        store.open("same", false);
    } catch (const Error& e) {
        threw = true;
        CHECK(std::string(e.what()).find("more than one") != std::string::npos);
    }
    CHECK(threw);
}

TEST(store, exclusive_open_fails_while_another_process_holds_the_lock) {
    test::TempDir dir;
    Store store(dir.path());
    Spec spec = spec_for("locked");
    std::string id = spec.id;

    // Two pipes: one for the child to say it holds the lock, one for the parent
    // to tell it to let go. A Darwin pipe is unidirectional, so the child
    // cannot wait by reading from the end it writes to.
    int ready_pipe[2];
    int release_pipe[2];
    CHECK_EQ(::pipe(ready_pipe), 0);
    CHECK_EQ(::pipe(release_pipe), 0);

    pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        ::close(ready_pipe[0]);
        ::close(release_pipe[1]);
        Store child_store(dir.path());
        std::unique_ptr<Capsule> held = child_store.create(spec);
        char ready = held->locked() ? 'y' : 'n';
        ssize_t ignored = ::write(ready_pipe[1], &ready, 1);
        (void)ignored;
        // Blocks until the parent closes its end, holding the lock meanwhile.
        char wait_byte = 0;
        ignored = ::read(release_pipe[0], &wait_byte, 1);
        (void)ignored;
        ::_exit(0);
    }
    ::close(ready_pipe[1]);
    ::close(release_pipe[0]);

    char ready = 0;
    CHECK_EQ(::read(ready_pipe[0], &ready, 1), ssize_t(1));
    CHECK_EQ(ready, 'y');

    // A read-only open must still work while another process is running it.
    CHECK_NO_THROW(store.open(id, false));
    CHECK_THROWS(store.open(id, true));

    ::close(release_pipe[1]);
    ::close(ready_pipe[0]);
    int status = 0;
    ::waitpid(child, &status, 0);
    CHECK_NO_THROW(store.open(id, true));
}

TEST(store, list_is_newest_first_and_skips_unreadable_capsules) {
    test::TempDir dir;
    Store store(dir.path());

    Spec older = spec_for("older");
    older.created_at = 1000;
    Spec newer = spec_for("newer");
    newer.created_at = 2000;
    store.create(older);
    store.create(newer);

    // A directory that looks like a capsule but holds nothing readable must
    // not stop the listing.
    std::string broken = path::join(store.capsules_dir(), "0123456789ab");
    fs::mkdir_p(broken, 0700);
    fs::write_file_atomic(path::join(broken, "capsule.json"), "{not json");

    std::vector<std::unique_ptr<Capsule>> listed = store.list();
    CHECK_EQ(listed.size(), size_t(2));
    CHECK_EQ(listed[0]->spec().name, std::string("newer"));
    CHECK_EQ(listed[1]->spec().name, std::string("older"));
}

TEST(store, list_ignores_directories_that_are_not_capsule_ids) {
    test::TempDir dir;
    Store store(dir.path());
    store.create(spec_for("real"));
    fs::mkdir_p(path::join(store.capsules_dir(), "not-an-id"), 0700);
    fs::mkdir_p(path::join(store.capsules_dir(), ".DS_Store_dir"), 0700);

    CHECK_EQ(store.list().size(), size_t(1));
}

TEST(store, state_round_trips) {
    test::TempDir dir;
    Store store(dir.path());
    Spec spec = spec_for("stateful");
    std::string id = spec.id;

    {
        std::unique_ptr<Capsule> capsule = store.create(spec);
        State& state = capsule->mutable_state();
        state.status = Status::Stopped;
        state.run_count = 3;
        state.started_at = 111;
        state.finished_at = 222;
        state.last_exit_code = 42;
        state.last_termination = "signalled (SIGKILL)";
        capsule->save_state();
    }

    std::unique_ptr<Capsule> reopened = store.open(id, false);
    CHECK(reopened->state().status == Status::Stopped);
    CHECK_EQ(reopened->state().run_count, 3);
    CHECK_EQ(*reopened->state().last_exit_code, 42);
    CHECK_EQ(reopened->state().last_termination, std::string("signalled (SIGKILL)"));
}

TEST(store, a_running_capsule_whose_supervisor_is_gone_reads_as_stopped) {
    test::TempDir dir;
    Store store(dir.path());
    Spec spec = spec_for("orphan");
    std::string id = spec.id;

    {
        std::unique_ptr<Capsule> capsule = store.create(spec);
        State& state = capsule->mutable_state();
        state.status = Status::Running;
        // A pid that cannot be running: well past any plausible live process
        // and chosen so the kernel reports ESRCH.
        state.supervisor_pid = 0x7FFFFFF0;
        capsule->save_state();
    }

    std::unique_ptr<Capsule> reopened = store.open(id, false);
    CHECK(reopened->state().status == Status::Stopped);
    CHECK(reopened->state().last_termination.find("without cleanup") != std::string::npos);
}

TEST(store, destroy_removes_the_tree) {
    test::TempDir dir;
    Store store(dir.path());
    std::unique_ptr<Capsule> capsule = store.create(spec_for("doomed"));
    std::string path = capsule->dir();
    dir.write(path::relative_to(dir.path(), path) + "/layers/x/file", "content");

    store.destroy(*capsule);
    CHECK(!fs::exists(path));
}

TEST(store, destroy_requires_the_lock) {
    test::TempDir dir;
    Store store(dir.path());
    Spec spec = spec_for("unlocked");
    std::string id = spec.id;
    store.create(spec);

    std::unique_ptr<Capsule> read_only = store.open(id, false);
    CHECK(!read_only->locked());
    CHECK_THROWS(store.destroy(*read_only));
    CHECK(fs::exists(read_only->dir()));
}

TEST(store, layer_and_snapshot_names_are_checked) {
    test::TempDir dir;
    Store store(dir.path());
    std::unique_ptr<Capsule> capsule = store.create(spec_for("paths"));

    CHECK_NO_THROW(capsule->layer_dir("src"));
    CHECK_THROWS(capsule->layer_dir(""));
    CHECK_THROWS(capsule->layer_dir(".."));
    CHECK_THROWS(capsule->layer_dir("a/b"));
    CHECK_THROWS(capsule->base_layer_dir("../../etc"));

    CHECK_NO_THROW(capsule->snapshot_dir("clean"));
    CHECK_THROWS(capsule->snapshot_dir(".."));
    CHECK_THROWS(capsule->snapshot_dir("a/b"));
}

TEST(store, label_prefers_the_name) {
    test::TempDir dir;
    Store store(dir.path());
    Spec named = spec_for("pretty");
    std::unique_ptr<Capsule> with_name = store.create(named);
    CHECK_EQ(with_name->label(), std::string("pretty"));

    Spec unnamed = spec_for("");
    unnamed.name.clear();
    std::unique_ptr<Capsule> without_name = store.create(unnamed);
    CHECK_EQ(without_name->label(), unnamed.id);
}

TEST(store, honours_a_custom_root) {
    test::TempDir dir;
    std::string root = dir.child("custom/root");
    Store store(root);
    CHECK_EQ(store.root(), root);
    std::unique_ptr<Capsule> capsule = store.create(spec_for("custom"));
    CHECK(path::is_within(root, capsule->dir()));
}

TEST(store, snapshots_lists_only_directories) {
    test::TempDir dir;
    Store store(dir.path());
    std::unique_ptr<Capsule> capsule = store.create(spec_for("snaps"));

    fs::mkdir_p(path::join(capsule->snapshots_dir(), "one"), 0700);
    fs::mkdir_p(path::join(capsule->snapshots_dir(), "two"), 0700);
    fs::write_file_atomic(path::join(capsule->snapshots_dir(), "stray"), "x");

    std::vector<std::string> names = capsule->snapshots();
    std::vector<std::string> expected{"one", "two"};
    CHECK_EQ(names, expected);
}
