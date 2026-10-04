#include "cli/commands.h"

#include "cli/format.h"
#include "fsview/layer.h"
#include "util/error.h"
#include "util/fs.h"
#include "util/lock.h"
#include "util/log.h"
#include "util/path.h"
#include "util/str.h"

#include "proc/tracker.h"
#include "sb/tag.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <unistd.h>

#include <cstdio>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

namespace isolate::cli {
namespace {

struct PruneEntry {
    std::string id;
    std::string name;
    std::string status;
    int64_t created_at = 0;
    uint64_t bytes = 0;
};

bool is_locked(const std::string& lock_file) {
    int fd = ::open(lock_file.c_str(), O_RDWR);
    if (fd < 0) return false;
    int rc = ::flock(fd, LOCK_EX | LOCK_NB);
    ::close(fd);
    return rc != 0;
}

} // namespace

int cmd_gc(const Options& options, Store& store) {
    std::vector<std::unique_ptr<Capsule>> capsules = store.list();
    int64_t now = static_cast<int64_t>(std::time(nullptr));
    double threshold_seconds = options.older_than.value_or(0.0);

    std::vector<PruneEntry> to_prune;
    uint64_t total_reclaimed = 0;

    for (const std::unique_ptr<Capsule>& capsule : capsules) {
        std::string lock_path = path::join(capsule->dir(), "lock");
        if (is_locked(lock_path)) {
            continue; // Active
        }

        const Spec& spec = capsule->spec();
        const State& state = capsule->state();

        // If marked running but supervisor is dead, mark stopped
        if (state.status == Status::Running && state.supervisor_pid > 0 &&
            ::kill(state.supervisor_pid, 0) != 0) {
            capsule->mutable_state().status = Status::Stopped;
            capsule->save_state();
        }

        // Do not prune if processes belonging to capsule are still alive
        proc::Tracker tracker(sb::membership_name(spec.id), 0);
        if (!tracker.scan().empty()) {
            continue;
        }

        int64_t last_time = state.finished_at > 0 ? state.finished_at : spec.created_at;
        double age_seconds = static_cast<double>(now - last_time);

        bool eligible = false;
        if (spec.ephemeral) {
            // Abandoned ephemeral capsule
            if (options.older_than.has_value()) {
                eligible = age_seconds >= threshold_seconds;
            } else {
                eligible = true;
            }
        } else if (options.all) {
            eligible = age_seconds >= threshold_seconds;
        } else if (options.older_than.has_value()) {
            eligible = age_seconds >= threshold_seconds;
        }

        if (eligible) {
            uint64_t bytes = fsview::capsule_apparent_size(*capsule);

            if (!options.dry_run) {
                try {
                    std::unique_ptr<Capsule> locked_capsule = store.open(spec.id, true);
                    store.destroy(*locked_capsule);
                } catch (const Error& e) {
                    log::warn("could not prune capsule " + spec.id + ": " + e.what());
                    continue;
                }
            }

            PruneEntry entry;
            entry.id = spec.id;
            entry.name = spec.name;
            entry.status = to_string(capsule->state().status);
            entry.created_at = spec.created_at;
            entry.bytes = bytes;
            total_reclaimed += bytes;
            to_prune.push_back(std::move(entry));
        }
    }

    if (options.json) {
        json::Object root;
        json::Array array;
        for (const PruneEntry& p : to_prune) {
            json::Object o;
            o.insert_or_assign("id", json::Value(p.id));
            o.insert_or_assign("name", json::Value(p.name));
            o.insert_or_assign("status", json::Value(p.status));
            o.insert_or_assign("created_at", json::Value(p.created_at));
            o.insert_or_assign("bytes_reclaimed", json::Value(p.bytes));
            array.emplace_back(std::move(o));
        }
        root.insert_or_assign("pruned", json::Value(std::move(array)));
        root.insert_or_assign("total_bytes_reclaimed", json::Value(total_reclaimed));
        root.insert_or_assign("dry_run", json::Value(options.dry_run));
        print_json(json::Value(std::move(root)));
        return 0;
    }

    if (to_prune.empty()) {
        std::printf("no orphaned or stale capsules to clean\n");
        return 0;
    }

    std::vector<std::vector<std::string>> rows;
    for (const PruneEntry& p : to_prune) {
        rows.push_back({
            p.id,
            p.name.empty() ? "-" : p.name,
            p.status,
            format_relative(p.created_at),
            str::human_bytes(p.bytes),
        });
    }

    print_table({"ID", "NAME", "STATUS", "CREATED", "RECLAIMED"}, rows);
    std::printf("\n%s %zu capsule%s, reclaimed %s\n",
                options.dry_run ? "would prune" : "pruned", to_prune.size(),
                to_prune.size() == 1 ? "" : "s", str::human_bytes(total_reclaimed).c_str());
    return 0;
}

} // namespace isolate::cli
