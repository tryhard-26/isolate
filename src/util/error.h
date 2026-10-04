#pragma once

#include <cerrno>
#include <cstring>
#include <exception>
#include <string>
#include <utility>

namespace isolate {

// main() prints `isolate: <msg>` and exits non-zero. Post-fork
// child paths never throw; see proc/launch.cpp.
class Error : public std::exception {
public:
    explicit Error(std::string message) : message_(std::move(message)) {}

    Error(std::string message, int err)
        : message_(std::move(message) + ": " + std::strerror(err)), errno_(err) {}

    const char* what() const noexcept override { return message_.c_str(); }
    int error_number() const noexcept { return errno_; }

private:
    std::string message_;
    int errno_ = 0;
};

// Can't work here, as opposed to went wrong. Exit 3.
class Unsupported : public Error {
public:
    using Error::Error;
};

[[noreturn]] inline void throw_errno(const std::string& what) {
    throw Error(what, errno);
}

} // namespace isolate
