#include "cli/commands.h"

#include "cli/format.h"
#include "proc/tracker.h"
#include "sb/tag.h"
#include "util/log.h"
#include "util/str.h"

#include <cstdio>

namespace isolate::cli {

int cmd_ps(const Options& options, Store& store) {
    const std::string& reference = require_target(options, "a capsule");
    std::unique_ptr<Capsule> capsule = store.open(reference, false);

    // No root pid: the supervisor isn't a member, and an idle
    // capsule should report whatever it left behind.
    proc::Tracker tracker(sb::membership_name(capsule->spec().id), 0);
    std::vector<proc::ProcInfo> processes = tracker.scan();

    if (options.json) {
        json::Array array;
        for (const proc::ProcInfo& info : processes) array.push_back(process_to_json(info));
        json::Object root_object;
        root_object.insert_or_assign("capsule", json::Value(capsule->spec().id));
        root_object.insert_or_assign("processes", json::Value(std::move(array)));
        root_object.insert_or_assign("attestation_working",
                                     json::Value(tracker.attestation_working()));
        print_json(json::Value(std::move(root_object)));
        return 0;
    }

    if (processes.empty()) {
        std::printf("no processes in capsule %s\n", capsule->label().c_str());
        return 0;
    }

    std::vector<std::vector<std::string>> rows;
    for (const proc::ProcInfo& info : processes) {
        std::string name = info.name;
        if (!info.path.empty() && log::level() >= log::Level::Verbose) name = info.path;
        rows.push_back({
            std::to_string(info.pid),
            std::to_string(info.ppid),
            std::to_string(info.thread_count),
            str::human_bytes(info.footprint),
            str::human_duration(info.cpu_seconds),
            proc::to_string(info.evidence),
            name,
        });
    }
    print_table({"PID", "PPID", "THR", "MEMORY", "CPU", "EVIDENCE", "COMMAND"}, rows);

    std::printf("\n%zu process%s, %s total\n", processes.size(),
                processes.size() == 1 ? "" : "es",
                str::human_bytes(tracker.total_footprint()).c_str());

    if (!tracker.attestation_working()) {
        log::warn("the kernel did not confirm membership for any of these processes; the "
                  "list may be incomplete. Run 'isolate doctor'.");
    }
    return 0;
}

} // namespace isolate::cli
