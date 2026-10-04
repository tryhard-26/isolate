#include "util/fs.h"

#include "util/error.h"
#include "util/path.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/attr.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <utility>

namespace isolate::fs {

Fd& Fd::operator=(Fd&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

Fd::~Fd() {
    reset();
}

int Fd::release() {
    int fd = fd_;
    fd_ = -1;
    return fd;
}

void Fd::reset(int fd) {
    if (fd_ >= 0 && fd_ != fd) ::close(fd_);
    fd_ = fd;
}

bool exists(const std::string& p) {
    struct stat st {};
    return ::lstat(p.c_str(), &st) == 0;
}

bool is_dir(const std::string& p) {
    struct stat st {};
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool is_symlink(const std::string& p) {
    struct stat st {};
    return ::lstat(p.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
}

std::optional<struct stat> lstat_opt(const std::string& p) {
    struct stat st {};
    if (::lstat(p.c_str(), &st) != 0) return std::nullopt;
    return st;
}

struct stat lstat_or_throw(const std::string& p) {
    struct stat st {};
    if (::lstat(p.c_str(), &st) != 0) throw Error("cannot stat '" + p + "'", errno);
    return st;
}

void mkdir_p(const std::string& p, mode_t mode) {
    std::string n = path::normalize(p);
    if (n == "/" || n == ".") return;
    if (is_dir(n)) return;

    std::string parent = path::dirname(n);
    if (parent != n) mkdir_p(parent, mode);
    if (::mkdir(n.c_str(), mode) != 0 && errno != EEXIST) {
        throw Error("cannot create directory '" + n + "'", errno);
    }
}

std::string read_file(const std::string& p) {
    Fd fd(::open(p.c_str(), O_RDONLY | O_CLOEXEC));
    if (!fd.valid()) throw Error("cannot open '" + p + "'", errno);

    std::string out;
    char buf[65536];
    while (true) {
        ssize_t n = ::read(fd.get(), buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw Error("cannot read '" + p + "'", errno);
        }
        if (n == 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    return out;
}

void write_file_atomic(const std::string& p, std::string_view data, mode_t mode) {
    std::string tmp = p + ".tmp." + std::to_string(::getpid());
    {
        Fd fd(::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode));
        if (!fd.valid()) throw Error("cannot create '" + tmp + "'", errno);

        size_t off = 0;
        while (off < data.size()) {
            ssize_t n = ::write(fd.get(), data.data() + off, data.size() - off);
            if (n < 0) {
                if (errno == EINTR) continue;
                int saved = errno;
                ::unlink(tmp.c_str());
                throw Error("cannot write '" + tmp + "'", saved);
            }
            off += static_cast<size_t>(n);
        }
        // State surviving a crash but pointing at a missing tree
        // is worse than losing the write.
        if (::fsync(fd.get()) != 0) {
            int saved = errno;
            ::unlink(tmp.c_str());
            throw Error("cannot flush '" + tmp + "'", saved);
        }
    }
    if (::rename(tmp.c_str(), p.c_str()) != 0) {
        int saved = errno;
        ::unlink(tmp.c_str());
        throw Error("cannot replace '" + p + "'", saved);
    }
}

std::string read_link(const std::string& p) {
    std::string buf(1024, '\0');
    while (true) {
        ssize_t n = ::readlink(p.c_str(), buf.data(), buf.size());
        if (n < 0) throw Error("cannot read symlink '" + p + "'", errno);
        if (static_cast<size_t>(n) < buf.size()) {
            buf.resize(static_cast<size_t>(n));
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
}

void write_link(const std::string& target, const std::string& at) {
    if (::symlink(target.c_str(), at.c_str()) != 0) {
        throw Error("cannot create symlink '" + at + "'", errno);
    }
}

namespace {

// Retries after clearing the flags that block removal; a capsule
// could otherwise chflags its scratch tree undeletable.
void force_unlink(int dirfd, const char* name, bool is_directory) {
    int flags = is_directory ? AT_REMOVEDIR : 0;
    if (::unlinkat(dirfd, name, flags) == 0) return;
    if (errno != EPERM && errno != EACCES) {
        throw Error(std::string("cannot remove '") + name + "'", errno);
    }

    // No chflagsat(2) on macOS, so go through an fd on the entry.
    // O_SYMLINK opens the link itself, which matters b/c
    // UF_IMMUTABLE can be set on a symlink.
    struct stat st {};
    if (::fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) == 0 && st.st_flags != 0) {
        int open_flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC;
        if (S_ISLNK(st.st_mode)) open_flags = O_SYMLINK | O_CLOEXEC;
        else if (S_ISDIR(st.st_mode)) open_flags |= O_DIRECTORY;
        Fd entry(::openat(dirfd, name, open_flags));
        if (entry.valid()) ::fchflags(entry.get(), 0);
    }
    if (::unlinkat(dirfd, name, flags) != 0) {
        throw Error(std::string("cannot remove '") + name + "'", errno);
    }
}

void remove_tree_at(int parent_fd, const std::string& name, const std::string& display) {
    struct stat st {};
    if (::fstatat(parent_fd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) return;
        throw Error("cannot stat '" + display + "'", errno);
    }

    if (!S_ISDIR(st.st_mode)) {
        force_unlink(parent_fd, name.c_str(), false);
        return;
    }

    Fd dir(::openat(parent_fd, name.c_str(),
                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!dir.valid()) throw Error("cannot open '" + display + "'", errno);

    // fdopendir takes the fd on success.
    DIR* d = ::fdopendir(dir.get());
    if (!d) throw Error("cannot read '" + display + "'", errno);
    dir.release();

    std::vector<std::pair<std::string, bool>> children;
    while (dirent* e = ::readdir(d)) {
        std::string child = e->d_name;
        if (child == "." || child == "..") continue;
        children.emplace_back(child, e->d_type == DT_DIR);
    }
    int dfd = ::dirfd(d);
    for (const auto& [child, child_is_dir] : children) {
        if (child_is_dir) {
            remove_tree_at(dfd, child, display + "/" + child);
        } else {
            force_unlink(dfd, child.c_str(), false);
        }
    }
    ::closedir(d);

    force_unlink(parent_fd, name.c_str(), true);
}

} // namespace

void remove_tree(const std::string& p) {
    std::string n = path::normalize(p);
    if (n == "/" || n == "." || n.empty()) {
        throw Error("refusing to delete '" + n + "'");
    }
    std::string parent = path::dirname(n);
    Fd parent_fd(::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!parent_fd.valid()) {
        if (errno == ENOENT) return;
        throw Error("cannot open '" + parent + "'", errno);
    }
    remove_tree_at(parent_fd.get(), path::basename(n), n);
}

namespace {

Kind kind_of(const struct stat& st) {
    if (S_ISDIR(st.st_mode)) return Kind::Dir;
    if (S_ISREG(st.st_mode)) return Kind::File;
    if (S_ISLNK(st.st_mode)) return Kind::Symlink;
    return Kind::Other;
}

void walk_dir(const std::string& root, const std::string& rel,
              const std::function<bool(const Entry&)>& visit) {
    std::string dir_path = rel.empty() ? root : root + "/" + rel;
    DIR* d = ::opendir(dir_path.c_str());
    if (!d) {
        if (errno == ENOENT) return;
        throw Error("cannot read directory '" + dir_path + "'", errno);
    }

    std::vector<std::string> names;
    while (dirent* e = ::readdir(d)) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        names.push_back(std::move(name));
    }
    ::closedir(d);

    // APFS readdir is hash order, so sort for deterministic diffs.
    std::sort(names.begin(), names.end());

    for (const std::string& name : names) {
        Entry entry;
        entry.relative = rel.empty() ? name : rel + "/" + name;
        std::string full = root + "/" + entry.relative;
        if (::lstat(full.c_str(), &entry.st) != 0) {
            if (errno == ENOENT) continue; // vanished mid-walk
            throw Error("cannot stat '" + full + "'", errno);
        }
        entry.kind = kind_of(entry.st);
        bool descend = visit(entry);
        if (entry.kind == Kind::Dir && descend) {
            walk_dir(root, entry.relative, visit);
        }
    }
}

} // namespace

void walk(const std::string& root, const std::function<bool(const Entry&)>& visit) {
    walk_dir(path::normalize(root), {}, visit);
}

std::vector<std::string> list_dir(const std::string& p) {
    std::vector<std::string> out;
    DIR* d = ::opendir(p.c_str());
    if (!d) {
        if (errno == ENOENT) return out;
        throw Error("cannot read directory '" + p + "'", errno);
    }
    while (dirent* e = ::readdir(d)) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        out.push_back(std::move(name));
    }
    ::closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

uint64_t tree_apparent_size(const std::string& p) {
    uint64_t total = 0;
    walk(p, [&](const Entry& e) {
        if (e.kind == Kind::File || e.kind == Kind::Dir) {
            total += static_cast<uint64_t>(e.st.st_blocks) * 512;
        }
        return true;
    });
    return total;
}

void chown_tree(const std::string& p, uid_t uid, gid_t gid) {
    if (::lchown(p.c_str(), uid, gid) != 0) {
        throw Error("cannot chown '" + p + "'", errno);
    }
    if (!is_dir(p)) return;
    walk(p, [&](const Entry& e) {
        std::string full = p + "/" + e.relative;
        if (::lchown(full.c_str(), uid, gid) != 0) {
            throw Error("cannot chown '" + full + "'", errno);
        }
        return true;
    });
}

void copy_metadata(const struct stat& from, const std::string& to) {
    timeval times[2];
    times[0].tv_sec = from.st_atimespec.tv_sec;
    times[0].tv_usec = static_cast<suseconds_t>(from.st_atimespec.tv_nsec / 1000);
    times[1].tv_sec = from.st_mtimespec.tv_sec;
    times[1].tv_usec = static_cast<suseconds_t>(from.st_mtimespec.tv_nsec / 1000);

    if (S_ISLNK(from.st_mode)) {
        ::lutimes(to.c_str(), times);
        return;
    }
    ::chmod(to.c_str(), from.st_mode & 07777);
    ::utimes(to.c_str(), times);
    if (from.st_flags != 0) ::chflags(to.c_str(), from.st_flags);
}

} // namespace isolate::fs
