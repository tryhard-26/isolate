#pragma once

#include <sys/stat.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace isolate::fs {

// RAII fd wrapper.
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : fd_(other.release()) {}
    Fd& operator=(Fd&& other) noexcept;
    ~Fd();

    int get() const { return fd_; }
    bool valid() const { return fd_ >= 0; }
    int release();
    void reset(int fd = -1);

private:
    int fd_ = -1;
};

bool exists(const std::string& p);
bool is_dir(const std::string& p);
bool is_symlink(const std::string& p);

// Optional lstat, no throw on ENOENT.
std::optional<struct stat> lstat_opt(const std::string& p);
struct stat lstat_or_throw(const std::string& p);

void mkdir_p(const std::string& p, mode_t mode = 0700);

std::string read_file(const std::string& p);

// Atomic write via tmp file + rename.
void write_file_atomic(const std::string& p, std::string_view data, mode_t mode = 0600);

std::string read_link(const std::string& p);
void write_link(const std::string& target, const std::string& at);

// Recursive rm; clears BSD immutability flags before unlink.
void remove_tree(const std::string& p);

enum class Kind { File, Dir, Symlink, Other };

struct Entry {
    std::string relative; // never "." or empty
    Kind kind = Kind::Other;
    struct stat st {};
};

// Sorted recursive walk without following symlinks.
void walk(const std::string& root, const std::function<bool(const Entry&)>& visit);

std::vector<std::string> list_dir(const std::string& p);

// Apparent recursive tree byte size.
uint64_t tree_apparent_size(const std::string& p);

// Copy file mode, times, and BSD flags.
void copy_metadata(const struct stat& from, const std::string& to);

// Recursive chown without symlink follow. Needs root.
void chown_tree(const std::string& p, uid_t uid, gid_t gid);

} // namespace isolate::fs
