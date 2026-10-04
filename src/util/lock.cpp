#include "util/lock.h"

#include "util/error.h"
#include "util/fs.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <string>

namespace isolate {

DirLock DirLock::open_and_lock(const std::string& lock_path, const std::string& what,
                               bool blocking) {
    DirLock lock;
    lock.fd_.reset(::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
    if (!lock.fd_.valid()) {
        throw Error("cannot open lock file '" + lock_path + "'", errno);
    }

    int flags = LOCK_EX | (blocking ? 0 : LOCK_NB);
    while (::flock(lock.fd_.get(), flags) != 0) {
        if (errno == EINTR) continue;
        if (!blocking && errno == EWOULDBLOCK) {
            lock.fd_.reset();
            return lock;
        }
        throw Error("cannot lock capsule " + what, errno);
    }

    // So a blocked user can be told who has it.
    std::string pid = std::to_string(::getpid());
    pid += '\n';
    if (::ftruncate(lock.fd_.get(), 0) == 0) {
        ssize_t written = ::pwrite(lock.fd_.get(), pid.data(), pid.size(), 0);
        (void)written; // advisory only
    }
    return lock;
}

DirLock DirLock::acquire(const std::string& lock_path, const std::string& what) {
    return open_and_lock(lock_path, what, true);
}

DirLock DirLock::try_acquire(const std::string& lock_path, const std::string& what) {
    return open_and_lock(lock_path, what, false);
}

void DirLock::release() {
    if (fd_.valid()) {
        ::flock(fd_.get(), LOCK_UN);
        fd_.reset();
    }
}

int DirLock::holder_pid(const std::string& lock_path) {
    try {
        std::string text = fs::read_file(lock_path);
        return std::atoi(text.c_str());
    } catch (const Error&) {
        return 0;
    }
}

} // namespace isolate
