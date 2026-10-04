#include "runner.h"

#include "harness.h"
#include "util/fs.h"
#include "util/path.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace isolate::test {
namespace {

std::string drain(int fd) {
    std::string out;
    char buf[4096];
    while (true) {
        ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    return out;
}

} // namespace

const char* isolate_binary() {
#ifdef ISOLATE_BINARY
    return ISOLATE_BINARY;
#else
#error "ISOLATE_BINARY must be defined by the build"
#endif
}

bool Run::out_contains(const std::string& needle) const {
    return out.find(needle) != std::string::npos;
}

bool Run::err_contains(const std::string& needle) const {
    return err.find(needle) != std::string::npos;
}

bool Run::contains(const std::string& needle) const {
    return out_contains(needle) || err_contains(needle);
}

Run execute(const std::string& program, const std::vector<std::string>& args,
            const std::vector<std::string>& extra_env) {
    Run result;

    int out_pipe[2];
    int err_pipe[2];
    if (::pipe(out_pipe) != 0) return result;
    if (::pipe(err_pipe) != 0) {
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        return result;
    }

    std::vector<std::string> argv_storage;
    argv_storage.push_back(program);
    for (const std::string& arg : args) argv_storage.push_back(arg);

    std::vector<char*> argv;
    for (std::string& arg : argv_storage) argv.push_back(arg.data());
    argv.push_back(nullptr);

    // A deliberately small environment: these tests must not depend on
    // whatever the developer happens to have exported.
    std::vector<std::string> env_storage = {
        "PATH=/usr/bin:/bin:/usr/sbin:/sbin",
        "HOME=" + std::string(std::getenv("HOME") ? std::getenv("HOME") : "/private/tmp"),
        "NO_COLOR=1",
    };
    for (const std::string& entry : extra_env) env_storage.push_back(entry);

    std::vector<char*> envp;
    for (std::string& entry : env_storage) envp.push_back(entry.data());
    envp.push_back(nullptr);

    pid_t child = ::fork();
    if (child < 0) {
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        ::close(err_pipe[0]);
        ::close(err_pipe[1]);
        return result;
    }

    if (child == 0) {
        ::close(out_pipe[0]);
        ::close(err_pipe[0]);
        ::dup2(out_pipe[1], STDOUT_FILENO);
        ::dup2(err_pipe[1], STDERR_FILENO);
        if (out_pipe[1] > STDERR_FILENO) ::close(out_pipe[1]);
        if (err_pipe[1] > STDERR_FILENO) ::close(err_pipe[1]);
        // stdin is /dev/null, so there is no controlling terminal and the
        // capsule gets no terminal rule. Tests that need one say so.
        int devnull = ::open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            if (devnull > STDERR_FILENO) ::close(devnull);
        }
        ::execve(argv[0], argv.data(), envp.data());
        std::fprintf(stderr, "cannot execute %s: %s\n", argv[0], std::strerror(errno));
        ::_exit(127);
    }

    ::close(out_pipe[1]);
    ::close(err_pipe[1]);

    // Reading stdout to completion first is safe here because the test programs
    // produce little output, and isolate writes diagnostics to stderr only.
    result.out = drain(out_pipe[0]);
    result.err = drain(err_pipe[0]);
    ::close(out_pipe[0]);
    ::close(err_pipe[0]);

    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    if (WIFEXITED(status)) result.status = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) result.status = 128 + WTERMSIG(status);
    return result;
}

Environment::Environment() {
    // Deliberately short: a capsule's scratch directory holds Unix domain
    // sockets, and sockaddr_un allows only 104 bytes of path. The per-user
    // temporary directory is already long enough that adding the store layout
    // to it would push a socket over the limit.
    std::string base = "/private/tmp";
    std::string pattern = path::join(base, "iso-it.XXXXXX");
    std::vector<char> buf(pattern.begin(), pattern.end());
    buf.push_back('\0');
    if (!::mkdtemp(buf.data())) {
        std::perror("mkdtemp");
        std::abort();
    }
    root_ = path::resolve(buf.data());
    store_ = path::join(root_, "store");
    work_ = path::join(root_, "work");
    fs::mkdir_p(store_, 0700);
    fs::mkdir_p(work_, 0755);
    // mkdtemp gives 0700; --user needs to traverse down to the store.
    ::chmod(root_.c_str(), 0711);
}

Environment::~Environment() {
    // Stop anything a test left running before taking the store away, or a
    // stray process could keep writing into a deleted directory.
    try {
        Run listed = isolate({"ls", "--json"});
        if (listed.ok()) {
            Run all = isolate({"destroy", "--all", "--force"});
            (void)all;
        }
    } catch (...) {
    }
    try {
        fs::remove_tree(root_);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "warning: could not clean up %s: %s\n", root_.c_str(), e.what());
    }
}

Run Environment::isolate(const std::vector<std::string>& args) const {
    return execute(isolate_binary(), args, {"ISOLATE_HOME=" + store_});
}

Run Environment::run_shell(const std::vector<std::string>& options,
                           const std::string& script) const {
    std::vector<std::string> args = {"run", "--ephemeral"};
    for (const std::string& option : options) args.push_back(option);
    args.push_back("--");
    args.push_back("/bin/sh");
    args.push_back("-c");
    args.push_back(script);
    return isolate(args);
}

std::string Environment::path(const std::string& relative) const {
    return path::join(work_, relative);
}

std::string Environment::write(const std::string& relative,
                               const std::string& contents) const {
    std::string full = path(relative);
    fs::mkdir_p(path::dirname(full), 0755);
    fs::write_file_atomic(full, contents, 0644);
    return full;
}

std::string Environment::read(const std::string& relative) const {
    return fs::read_file(path(relative));
}

bool Environment::exists(const std::string& relative) const {
    return fs::exists(path(relative));
}

std::string Environment::only_capsule() const {
    std::vector<std::string> ids;
    for (const std::string& name : fs::list_dir(path::join(store_, "capsules"))) {
        ids.push_back(name);
    }
    if (ids.size() != 1) {
        fail_at(__FILE__, __LINE__,
                "expected exactly one capsule in the store, found " +
                    std::to_string(ids.size()));
    }
    return ids.front();
}

} // namespace isolate::test
