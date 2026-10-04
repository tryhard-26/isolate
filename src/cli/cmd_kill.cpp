#include "cli/commands.h"

#include "cli/format.h"
#include "proc/tracker.h"
#include "sb/tag.h"
#include "util/error.h"
#include "util/log.h"
#include "util/str.h"

#include <signal.h>

#include <cstdio>
#include <cstdlib>

namespace isolate::cli {

int cmd_kill(const Options& options, Store& store) {
    const std::string& reference = require_target(options, "a capsule");
    std::unique_ptr<Capsule> capsule = store.open(reference, false);

    int signum = options.signal_number.value_or(SIGTERM);
    proc::Tracker tracker(sb::membership_name(capsule->spec().id), 0);
    std::vector<proc::ProcInfo> processes = tracker.scan();

    if (options.targets.size() >= 2) {
        // Membership check is the point: naming a pid next to a
        // capsule is not permission to signal it.
        char* end = nullptr;
        long parsed = std::strtol(options.targets[1].c_str(), &end, 10);
        if (!end || *end != '\0' || parsed <= 0) {
            throw Error("'" + options.targets[1] + "' is not a process id");
        }
        pid_t pid = static_cast<pid_t>(parsed);

        bool belongs = false;
        for (const proc::ProcInfo& info : processes) {
            if (info.pid == pid) belongs = true;
        }
        if (!belongs) {
            throw Error("process " + std::to_string(pid) + " does not belong to capsule " +
                        capsule->label() + "; isolate will not signal it");
        }
        if (::kill(pid, signum) != 0) {
            throw Error("cannot signal process " + std::to_string(pid), errno);
        }
        log::info("sent " + str::signal_name(signum) + " to process " + std::to_string(pid));
        return 0;
    }

    if (processes.empty()) {
        std::printf("no processes in capsule %s\n", capsule->label().c_str());
        return 0;
    }

    int delivered = tracker.signal_all(signum);
    log::info("sent " + str::signal_name(signum) + " to " + std::to_string(delivered) +
              " process" + (delivered == 1 ? "" : "es") + " in capsule " +
              capsule->label());

    // Supervisor gone == nothing else will update state.
    if (signum == SIGKILL || signum == SIGTERM) {
        capsule->reload_state();
        const State& state = capsule->state();
        if (state.status == Status::Running && state.supervisor_pid > 0 &&
            ::kill(state.supervisor_pid, 0) != 0) {
            capsule->mutable_state().status = Status::Stopped;
            capsule->mutable_state().last_termination =
                "killed with " + str::signal_name(signum);
            capsule->save_state();
        }
    }
    return 0;
}

} // namespace isolate::cli
