#pragma once

#include "util/fs.h"

#include <string>

namespace isolate {

// flock on a capsule dir. Two runs against the same persistent
// capsule would otherwise interleave writes to capsule.json and
// the layers. Held by an fd, so the kernel drops it on crash;
// no stale lock file to clean up.
class DirLock {
public:
    DirLock() = default;

    // Blocks. `what` names the capsule in errors.
    static DirLock acquire(const std::string& lock_path, const std::string& what);

    // Unheld result if someone else has it.
    static DirLock try_acquire(const std::string& lock_path, const std::string& what);

    bool held() const { return fd_.valid(); }
    void release();

    // 0 if unknown. Advisory, for error messages.
    static int holder_pid(const std::string& lock_path);

private:
    static DirLock open_and_lock(const std::string& lock_path, const std::string& what,
                                 bool blocking);

    fs::Fd fd_;
};

} // namespace isolate
