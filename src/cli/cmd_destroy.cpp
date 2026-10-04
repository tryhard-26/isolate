#include "cli/commands.h"

#include "cli/format.h"
#include "fsview/layer.h"
#include "proc/tracker.h"
#include "sb/tag.h"
#include "util/error.h"
#include "util/log.h"
#include "util/str.h"

#include <signal.h>

#include <cstdio>

namespace isolate::cli {

int cmd_destroy(const Options& options, Store& store) {
    if (options.targets.empty() && !options.all) {
        throw Error("expected a capsule to destroy, or --all");
    }

    std::vector<std::string> references = options.targets;
    if (options.all) {
        for (const std::unique_ptr<Capsule>& capsule : store.list()) {
            references.push_back(capsule->spec().id);
        }
        if (references.empty()) {
            std::printf("no capsules\n");
            return 0;
        }
    }

    int failures = 0;
    for (const std::string& reference : references) {
        try {
            std::unique_ptr<Capsule> capsule = store.open(reference, true);

            // Else they run on w/ a profile whose capsule is gone,
            // and nothing ever cleans them up.
            proc::Tracker tracker(sb::membership_name(capsule->spec().id), 0);
            std::vector<proc::ProcInfo> processes = tracker.scan();
            if (!processes.empty()) {
                if (!options.force) {
                    throw Error("capsule " + capsule->label() + " still has " +
                                std::to_string(processes.size()) + " running process" +
                                (processes.size() == 1 ? "" : "es") +
                                "; stop them with 'isolate kill " + reference +
                                "' or pass --force");
                }
                tracker.signal_all(SIGKILL);
            }

            // The point of a persistent capsule is that work survives.
            if (!capsule->spec().ephemeral && !options.force) {
                Spec& spec = capsule->mutable_spec();
                fsview::resolve_guest_paths(spec, *capsule);
                size_t pending = 0;
                for (const Mount& mount : spec.mounts) {
                    if (mount.storage != Storage::Clone) continue;
                    pending += fsview::layer_diff(*capsule, mount).changes.size();
                }
                if (pending > 0) {
                    throw Error("capsule " + capsule->label() + " has " +
                                std::to_string(pending) + " uncommitted change" +
                                (pending == 1 ? "" : "s") +
                                ". Review with 'isolate diff " + reference +
                                "', keep them with 'isolate commit " + reference +
                                "', or discard them with --force.");
                }
            }

            std::string label = capsule->label();
            store.destroy(*capsule);
            log::info("destroyed capsule " + label);
        } catch (const Error& e) {
            ++failures;
            log::warn(e.what());
        }
    }

    return failures == 0 ? 0 : 1;
}

} // namespace isolate::cli
