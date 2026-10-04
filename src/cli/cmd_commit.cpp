#include "cli/commands.h"

#include "cli/format.h"
#include "fsview/layer.h"
#include "util/error.h"
#include "util/log.h"
#include "util/str.h"

#include <cstdio>

namespace isolate::cli {

int cmd_commit(const Options& options, Store& store) {
    const std::string& reference = require_target(options, "a capsule to commit");

    // Exclusive: committing writes to the host paths and refreshes the capsule's
    // baseline, and must not run while the capsule is running.
    std::unique_ptr<Capsule> capsule = store.open(reference, true);
    if (capsule->state().status == Status::Running) {
        throw Error("capsule " + capsule->label() + " is running; stop it before "
                    "committing, or its changes will be committed half-written");
    }

    Spec& spec = capsule->mutable_spec();
    fsview::resolve_guest_paths(spec, *capsule);

    std::vector<const Mount*> cloned;
    for (const Mount& mount : spec.mounts) {
        if (mount.storage == Storage::Clone) cloned.push_back(&mount);
    }
    if (cloned.empty()) {
        throw Error("capsule " + capsule->label() +
                    " has no cloned mounts, so there is nothing to commit. Only --cow "
                    "mounts keep their changes separate from the host.");
    }

    int total_applied = 0;
    int total_conflicts = 0;
    bool any_failure = false;

    for (const Mount* mount : cloned) {
        fsview::CommitPlan plan = fsview::plan_commit(*capsule, *mount, options.excludes);

        if (plan.empty()) {
            std::printf("%s: no changes\n", mount->host.c_str());
            continue;
        }

        print_heading(mount->host);
        for (const fsview::Change& change : plan.apply) {
            std::printf("  %c %s\n", fsview::marker(change.kind), change.path.c_str());
        }

        if (!plan.conflicts.empty()) {
            std::printf("\n");
            for (const fsview::Change& change : plan.conflicts) {
                std::printf("  %s %s  %s(also changed on the host)%s\n",
                            options.force ? "!" : "?", change.path.c_str(),
                            log::stdout_is_tty() ? "\033[90m" : "",
                            log::stdout_is_tty() ? "\033[0m" : "");
            }
            total_conflicts += static_cast<int>(plan.conflicts.size());
        }

        if (options.dry_run) {
            std::printf("\n  %zu to apply", plan.apply.size());
            if (!plan.conflicts.empty()) {
                std::printf(", %zu conflicting", plan.conflicts.size());
            }
            std::printf(" (dry run; nothing written)\n\n");
            continue;
        }

        fsview::CommitResult result = fsview::commit_layer(*capsule, *mount, plan,
                                                            options.force);
        total_applied += result.applied;

        std::printf("\n  applied %d change%s to %s\n", result.applied,
                    result.applied == 1 ? "" : "s", mount->host.c_str());
        if (result.skipped > 0) {
            std::printf("  skipped %d conflicting path%s; re-run with --force to overwrite "
                        "the host's version\n",
                        result.skipped, result.skipped == 1 ? "" : "s");
        }
        for (const std::string& failure : result.failures) {
            any_failure = true;
            log::warn("could not commit " + failure);
        }
        std::printf("\n");
    }

    if (options.dry_run) return total_conflicts > 0 ? 1 : 0;
    if (any_failure) return 1;
    if (total_conflicts > 0 && !options.force) return 1;

    if (total_applied > 0) {
        log::info("baseline refreshed; 'isolate diff " + reference +
                  "' now reports only what changes from here");
    }
    return 0;
}

} // namespace isolate::cli
