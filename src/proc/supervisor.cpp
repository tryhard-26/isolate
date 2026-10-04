#include "proc/supervisor.h"

#include "proc/denial_tracer.h"
#include "proc/launch.h"
#include "proc/limits.h"
#include "proc/pty.h"
#include "sb/tag.h"
#include "util/error.h"
#include "util/fs.h"
#include "util/log.h"
#include "util/str.h"

#include <signal.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>

namespace isolate::proc {
namespace {

constexpr int kSampleTimerIdent = 1;
constexpr int kGraceTimerIdent = 2;

double monotonic_seconds() {
    timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

// Block sigs; handle via kqueue
const int kForwardedSignals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT};

class SignalMask {
public:
    SignalMask() {
        sigset_t block;
        sigemptyset(&block);
        for (int signum : kForwardedSignals) sigaddset(&block, signum);
        sigaddset(&block, SIGCHLD);
        sigaddset(&block, SIGWINCH);
        ::sigprocmask(SIG_BLOCK, &block, &saved_);
        // Ignore SIGPIPE
        previous_pipe_ = ::signal(SIGPIPE, SIG_IGN);
        active_ = true;
    }
    SignalMask(const SignalMask&) = delete;
    SignalMask& operator=(const SignalMask&) = delete;
    ~SignalMask() {
        if (!active_) return;
        ::sigprocmask(SIG_SETMASK, &saved_, nullptr);
        ::signal(SIGPIPE, previous_pipe_);
    }

private:
    sigset_t saved_ {};
    void (*previous_pipe_)(int) = SIG_DFL;
    bool active_ = false;
};

void add_event(int kq, uintptr_t ident, int16_t filter, uint16_t flags, uint32_t fflags,
               intptr_t data) {
    struct kevent change {};
    EV_SET(&change, ident, filter, flags, fflags, data, nullptr);
    if (::kevent(kq, &change, 1, nullptr, 0, nullptr) != 0 && errno != ENOENT) {
        // ESRCH means already exited; waitpid finds that.
        if (errno != ESRCH) {
            log::debug(std::string("kevent registration failed: ") + std::strerror(errno));
        }
    }
}

} // namespace

const char* to_string(Termination t) {
    switch (t) {
        case Termination::Exited: return "exited";
        case Termination::Signalled: return "signalled";
        case Termination::MemoryLimit: return "memory limit";
        case Termination::ProcessLimit: return "process limit";
        case Termination::TimeLimit: return "time limit";
        case Termination::Interrupted: return "interrupted";
        case Termination::LaunchFailed: return "launch failed";
    }
    return "?";
}

int RunReport::exit_status_for_isolate() const {
    switch (termination) {
        case Termination::Exited:
            return exit_code;
        case Termination::Signalled:
            return 128 + signal_number; // shell convention
        case Termination::MemoryLimit:
        case Termination::ProcessLimit:
        case Termination::TimeLimit:
            return 125;
        case Termination::Interrupted:
            return 130;
        case Termination::LaunchFailed:
            return 127;
    }
    return 1;
}

Supervisor::Supervisor(Capsule& capsule, SupervisorOptions options)
    : capsule_(capsule), options_(options) {}

RunReport Supervisor::run(const std::vector<std::string>& argv, const std::string& profile,
                          PtySession* pty) {
    RunReport report;
    const Spec& spec = capsule_.spec();

    std::unique_ptr<DenialTracer> tracer;
    if (options_.trace_denials) {
        tracer = std::make_unique<DenialTracer>();
        tracer->start();
    }

    LaunchRequest request;
    request.argv = argv;
    request.envp = build_environment(spec, capsule_);
    request.cwd = spec.cwd;
    request.profile = profile;
    request.limits = spec.limits;
    request.credentials = spec.credentials;
    request.new_session = true;

    if (pty && pty->is_open()) {
        request.stdin_fd = pty->slave_fd();
        request.stdout_fd = pty->slave_fd();
        request.stderr_fd = pty->slave_fd();
        pty->enter_raw_mode();
    }

    // Before the child exists, so an interrupt between fork and
    // the event loop isn't lost.
    SignalMask mask;

    fs::Fd kq(::kqueue());
    if (!kq.valid()) throw_errno("cannot create a kernel event queue");

    pid_t child = launch(request);

    if (pty && pty->is_open()) {
        pty->close_slave();
        add_event(kq.get(), STDIN_FILENO, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0);
        add_event(kq.get(), static_cast<uintptr_t>(pty->master_fd()), EVFILT_READ,
                  EV_ADD | EV_ENABLE, 0, 0);
        add_event(kq.get(), static_cast<uintptr_t>(SIGWINCH), EVFILT_SIGNAL, EV_ADD | EV_ENABLE,
                  0, 0);
    }

    Tracker tracker(sb::membership_name(spec.id), child);

    add_event(kq.get(), static_cast<uintptr_t>(child), EVFILT_PROC, EV_ADD | EV_ENABLE,
              NOTE_EXIT, 0);
    for (int signum : kForwardedSignals) {
        add_event(kq.get(), static_cast<uintptr_t>(signum), EVFILT_SIGNAL,
                  EV_ADD | EV_ENABLE, 0, 0);
    }
    // A mem ceiling is only as tight as its sampling.
    std::chrono::milliseconds interval = spec.limits.memory_bytes.has_value()
                                             ? options_.memory_sample_interval
                                             : options_.sample_interval;
    add_event(kq.get(), kSampleTimerIdent, EVFILT_TIMER, EV_ADD | EV_ENABLE, NOTE_USECONDS,
              static_cast<intptr_t>(interval.count() * 1000));

    const double started = monotonic_seconds();
    bool child_exited = false;
    int child_status = 0;
    bool shutting_down = false;
    Termination reason = Termination::Exited;
    std::string detail;

    auto begin_shutdown = [&](Termination why, const std::string& message) {
        if (shutting_down) return;
        shutting_down = true;
        reason = why;
        detail = message;
        if (options_.announce && !message.empty()) log::info(message);

        tracker.scan();
        tracker.signal_all(SIGTERM);
        add_event(kq.get(), kGraceTimerIdent, EVFILT_TIMER, EV_ADD | EV_ENABLE | EV_ONESHOT,
                  NOTE_USECONDS,
                  static_cast<intptr_t>(options_.grace_period.count() * 1000));
    };

    while (true) {
        struct kevent event {};
        int n = ::kevent(kq.get(), nullptr, 0, &event, 1, nullptr);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw_errno("cannot wait for capsule events");
        }
        if (n == 0) continue;

        if (event.filter == EVFILT_PROC) {
            child_exited = true;
            while (::waitpid(child, &child_status, 0) < 0 && errno == EINTR) {
            }
            if (!shutting_down) {
                if (WIFSIGNALED(child_status)) {
                    reason = Termination::Signalled;
                    report.signal_number = WTERMSIG(child_status);
                } else {
                    reason = Termination::Exited;
                    report.exit_code = WEXITSTATUS(child_status);
                }
            }
            break;
        }

        if (event.filter == EVFILT_SIGNAL) {
            int signum = static_cast<int>(event.ident);
            if (signum == SIGINT || signum == SIGTERM || signum == SIGHUP ||
                signum == SIGQUIT) {
                if (shutting_down) {
                    // Second interrupt: done waiting.
                    tracker.scan();
                    tracker.signal_all(SIGKILL);
                    report.forced_kill = true;
                } else {
                    begin_shutdown(Termination::Interrupted,
                                   "received " + str::signal_name(signum) +
                                       "; stopping the capsule");
                }
            } else if (signum == SIGWINCH) {
                if (pty) pty->update_window_size();
            }
            continue;
        }

        if (event.filter == EVFILT_READ) {
            if (event.ident == STDIN_FILENO) {
                if (pty && !pty->pump_to_slave()) {
                    add_event(kq.get(), STDIN_FILENO, EVFILT_READ, EV_DELETE, 0, 0);
                }
            } else if (pty && event.ident == static_cast<uintptr_t>(pty->master_fd())) {
                pty->pump_from_slave();
                if (event.flags & EV_EOF) {
                    while (pty->pump_from_slave()) {
                    }
                    add_event(kq.get(), static_cast<uintptr_t>(pty->master_fd()), EVFILT_READ,
                              EV_DELETE, 0, 0);
                }
            }
            continue;
        }

        if (event.filter != EVFILT_TIMER) continue;

        if (event.ident == kGraceTimerIdent) {
            tracker.scan();
            if (tracker.count() > 0) {
                int killed = tracker.signal_all(SIGKILL);
                if (killed > 0) {
                    report.forced_kill = true;
                    log::verbose("sent SIGKILL to " + std::to_string(killed) +
                                 " capsule processes that did not stop");
                }
            }
            continue;
        }

        // Sample tick.
        tracker.scan();
        report.peak_footprint = std::max(report.peak_footprint, tracker.total_footprint());
        report.peak_process_count = std::max(report.peak_process_count, tracker.count());
        // Readable only while alive, so keep the max seen.
        report.cpu_seconds = std::max(report.cpu_seconds, tracker.total_cpu_seconds());
        if (!tracker.attestation_working() && tracker.count() > 0) {
            report.attestation_degraded = true;
        }

        if (shutting_down) continue;

        const Limits& limits = spec.limits;
        if (limits.memory_bytes.has_value() &&
            tracker.total_footprint() > *limits.memory_bytes) {
            uint64_t observed = tracker.total_footprint();
            report.memory_overshoot = observed - *limits.memory_bytes;
            begin_shutdown(Termination::MemoryLimit,
                           "capsule exceeded its memory limit (" +
                               str::human_bytes(observed) + " of " +
                               str::human_bytes(*limits.memory_bytes) + "); stopping it");
            continue;
        }

        if (limits.max_processes.has_value() &&
            tracker.count() > static_cast<size_t>(*limits.max_processes)) {
            size_t keep = static_cast<size_t>(*limits.max_processes);
            int killed = tracker.signal_extra_processes(SIGKILL, keep);
            if (options_.announce) {
                log::warn("capsule reached its process limit of " +
                          std::to_string(*limits.max_processes) + "; killed " +
                          std::to_string(killed) + " newer " +
                          (killed == 1 ? "process" : "processes"));
            }
            // Kill excess procs; keep root
            if (killed == 0 && keep <= 1) {
                begin_shutdown(Termination::ProcessLimit,
                               "capsule exceeded its process limit; stopping it");
            }
            continue;
        }

        if (limits.wall_seconds.has_value() &&
            monotonic_seconds() - started > *limits.wall_seconds) {
            begin_shutdown(Termination::TimeLimit,
                           "capsule reached its time limit of " +
                               str::human_duration(*limits.wall_seconds) + "; stopping it");
            continue;
        }
    }

    // Cleanup procs reparented to launchd
    tracker.scan();
    report.leftover_processes = tracker.count();
    if (report.leftover_processes > 0) {
        if (options_.announce) {
            log::info("cleaning up " + std::to_string(report.leftover_processes) +
                      " process" + (report.leftover_processes == 1 ? "" : "es") +
                      " left behind by the capsule");
            for (const ProcInfo& info : tracker.members()) {
                log::verbose("  leftover: pid " + std::to_string(info.pid) + " ppid " +
                             std::to_string(info.ppid) + " " + info.name + " (" +
                             to_string(info.evidence) + ")");
            }
        }
        tracker.signal_all(SIGTERM);

        // Grace period, then stop waiting.
        const double deadline =
            monotonic_seconds() +
            static_cast<double>(options_.grace_period.count()) / 1000.0;
        while (monotonic_seconds() < deadline) {
            tracker.scan();
            if (tracker.count() == 0) break;
            usleep(50 * 1000);
        }
        tracker.scan();
        if (tracker.count() > 0) {
            tracker.signal_all(SIGKILL);
            report.forced_kill = true;
            // Reap our own direct children.
            while (::waitpid(-1, nullptr, WNOHANG) > 0) {
            }
        }
    }

    if (shutting_down) {
        if (!child_exited) {
            while (::waitpid(child, &child_status, 0) < 0 && errno == EINTR) {
            }
        }
        if (WIFSIGNALED(child_status)) report.signal_number = WTERMSIG(child_status);
        else if (WIFEXITED(child_status)) report.exit_code = WEXITSTATUS(child_status);
    }

    report.termination = reason;
    report.detail = detail;
    report.wall_seconds = monotonic_seconds() - started;

    // No zombies left behind.
    while (::waitpid(-1, nullptr, WNOHANG) > 0) {
    }

    if (pty && pty->is_open()) {
        while (pty->pump_from_slave()) {
        }
        pty->restore_mode();
        pty->close_all();
    }

    if (tracer) {
        tracer->stop();
    }

    return report;
}

} // namespace isolate::proc
