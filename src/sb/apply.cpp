#include "sb/apply.h"

#include "util/error.h"

#include <unistd.h>
#include <sys/wait.h>

#include <cerrno>
#include <cstdio>
#include <cstring>


#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <sandbox.h>

extern "C" int sandbox_init_with_parameters(const char* profile, uint64_t flags,
                                            const char* const parameters[], char** errorbuf);
extern "C" int sandbox_check(pid_t pid, const char* operation, int type, ...);

namespace isolate::sb {
namespace {


char g_error[1024];

void record(const char* message) {
    std::snprintf(g_error, sizeof g_error, "%s", message ? message : "unknown error");
}

} // namespace

const char* apply_profile_nothrow(const char* profile) {
    if (!profile || !*profile) {
        record("empty sandbox profile");
        return g_error;
    }

    char* error = nullptr;
    if (sandbox_init_with_parameters(profile, 0, nullptr, &error) == 0) return nullptr;

    if (error) {
        record(error);
        sandbox_free_error(error);
    } else {
        record(std::strerror(errno));
    }
    return g_error;
}

ValidationResult validate_profile(const std::string& profile) {
    ValidationResult result;

    int pipefd[2];
    if (::pipe(pipefd) != 0) {
        result.error = std::string("cannot create a pipe: ") + std::strerror(errno);
        return result;
    }

    pid_t child = ::fork();
    if (child < 0) {
        int saved = errno;
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        result.error = std::string("cannot fork: ") + std::strerror(saved);
        return result;
    }

    if (child == 0) {
        ::close(pipefd[0]);
        const char* error = apply_profile_nothrow(profile.c_str());
        if (error) {
            size_t len = std::strlen(error);
            ssize_t ignored = ::write(pipefd[1], error, len);
            (void)ignored;
            ::_exit(1);
        }
        ::_exit(0);
    }

    ::close(pipefd[1]);
    std::string message;
    char buf[256];
    while (true) {
        ssize_t n = ::read(pipefd[0], buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        message.append(buf, static_cast<size_t>(n));
    }
    ::close(pipefd[0]);

    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        result.ok = true;
        return result;
    }
    if (WIFSIGNALED(status)) {
        result.error = "the sandbox compiler terminated on signal " +
                       std::to_string(WTERMSIG(status));
        return result;
    }
    result.error = message.empty() ? "the profile was rejected without a message" : message;
    return result;
}

bool self_is_sandboxed() {
    return sandbox_check(::getpid(), nullptr, 0) == 1;
}

} // namespace isolate::sb

#pragma clang diagnostic pop
