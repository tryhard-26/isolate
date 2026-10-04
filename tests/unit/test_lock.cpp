#include "harness.h"

#include "util/lock.h"

#include <sys/wait.h>
#include <unistd.h>

using namespace isolate;

TEST(lock, acquires_and_releases) {
    test::TempDir dir;
    std::string lock_path = dir.child("lock");

    DirLock lock = DirLock::acquire(lock_path, "test");
    CHECK(lock.held());
    lock.release();
    CHECK(!lock.held());

    DirLock again = DirLock::acquire(lock_path, "test");
    CHECK(again.held());
}

TEST(lock, try_acquire_fails_while_another_process_holds_it) {
    test::TempDir dir;
    std::string lock_path = dir.child("lock");

    // A Darwin pipe is unidirectional, so the child needs a second pipe to wait
    // on rather than reading from the end it writes to, which would fail
    // immediately and let it exit while the parent still expects it to be
    // holding the lock.
    int ready_pipe[2];
    int release_pipe[2];
    CHECK_EQ(::pipe(ready_pipe), 0);
    CHECK_EQ(::pipe(release_pipe), 0);

    pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        ::close(ready_pipe[0]);
        ::close(release_pipe[1]);
        DirLock held = DirLock::acquire(lock_path, "test");
        char ready = held.held() ? 'y' : 'n';
        ssize_t ignored = ::write(ready_pipe[1], &ready, 1);
        (void)ignored;
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

    DirLock attempt = DirLock::try_acquire(lock_path, "test");
    CHECK(!attempt.held());
    CHECK_EQ(DirLock::holder_pid(lock_path), child);

    ::close(release_pipe[1]);
    ::close(ready_pipe[0]);
    int status = 0;
    ::waitpid(child, &status, 0);

    // Once the holder exits the kernel drops its lock.
    DirLock after = DirLock::try_acquire(lock_path, "test");
    CHECK(after.held());
}

TEST(lock, records_the_holder_pid) {
    test::TempDir dir;
    std::string lock_path = dir.child("lock");
    DirLock lock = DirLock::acquire(lock_path, "test");
    CHECK_EQ(DirLock::holder_pid(lock_path), ::getpid());
}

TEST(lock, holder_pid_is_zero_for_a_missing_file) {
    test::TempDir dir;
    CHECK_EQ(DirLock::holder_pid(dir.child("never-created")), 0);
}

TEST(lock, reports_an_unwritable_directory) {
    CHECK_THROWS(DirLock::acquire("/no/such/directory/lock", "test"));
}
