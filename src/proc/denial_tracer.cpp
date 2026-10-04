#include "proc/denial_tracer.h"

#include "util/log.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

namespace isolate::proc {

DenialTracer::DenialTracer() = default;

DenialTracer::~DenialTracer() {
    stop();
}

void DenialTracer::start() {
    if (running_.exchange(true)) return;

    int pipefd[2];
    if (::pipe(pipefd) != 0) {
        running_ = false;
        return;
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        running_ = false;
        return;
    }

    if (pid == 0) {
        ::close(pipefd[0]);
        ::dup2(pipefd[1], STDOUT_FILENO);
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) ::close(devnull);
        }
        if (pipefd[1] > STDERR_FILENO) ::close(pipefd[1]);

        const char* argv[] = {
            "/usr/bin/log",
            "stream",
            "--style",
            "ndjson",
            "--predicate",
            "subsystem == \"com.apple.sandbox\" AND eventMessage CONTAINS \"mach-lookup\"",
            nullptr,
        };
        ::execv(argv[0], const_cast<char* const*>(argv));
        ::_exit(1);
    }

    ::close(pipefd[1]);
    pipe_fd_ = pipefd[0];
    log_pid_ = pid;

    worker_ = std::thread([this] { run_loop(); });
}

void DenialTracer::stop() {
    if (!running_.exchange(false)) return;

    if (log_pid_ > 0) {
        ::kill(log_pid_, SIGKILL);
        int status = 0;
        while (::waitpid(log_pid_, &status, 0) < 0 && errno == EINTR) {
        }
        log_pid_ = -1;
    }

    if (pipe_fd_ >= 0) {
        ::close(pipe_fd_);
        pipe_fd_ = -1;
    }

    if (worker_.joinable()) {
        worker_.join();
    }
}

void DenialTracer::run_loop() {
    std::string line;
    char buf[1024];

    while (running_) {
        ssize_t n = ::read(pipe_fd_, buf, sizeof(buf));
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }

        for (ssize_t i = 0; i < n; ++i) {
            char c = buf[i];
            if (c == '\n') {
                if (line.find("deny") != std::string::npos &&
                    line.find("mach-lookup") != std::string::npos) {
                    size_t pos = line.find("mach-lookup");
                    if (pos != std::string::npos) {
                        pos += 11; // strlen("mach-lookup")
                        while (pos < line.size() && (line[pos] == ' ' || line[pos] == '"')) ++pos;
                        size_t end = pos;
                        while (end < line.size() && line[end] != ' ' && line[end] != '"' &&
                               line[end] != '\\' && line[end] != '\n') {
                            ++end;
                        }
                        if (end > pos) {
                            std::string svc = line.substr(pos, end - pos);
                            if (!svc.empty() && reported_services_.insert(svc).second) {
                                std::fprintf(
                                    stderr,
                                    "isolate: notice: program requested Mach service '%s'; retry with '--service %s'\n",
                                    svc.c_str(), svc.c_str());
                            }
                        }
                    }
                }
                line.clear();
            } else {
                if (line.size() < 4096) line += c;
            }
        }
    }
}

} // namespace isolate::proc
