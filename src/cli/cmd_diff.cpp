#include "cli/commands.h"

#include "cli/format.h"
#include "fsview/layer.h"
#include "proc/tracker.h"
#include "sb/tag.h"
#include "util/error.h"
#include "util/fs.h"
#include "util/log.h"
#include "util/str.h"

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace isolate::cli {

int cmd_diff(const Options& options, Store& store) {
    const std::string& reference = require_target(options, "a capsule to compare");
    std::unique_ptr<Capsule> capsule = store.open(reference, false);
    Spec& spec = capsule->mutable_spec();
    fsview::resolve_guest_paths(spec, *capsule);

    std::vector<const Mount*> cloned;
    for (const Mount& mount : spec.mounts) {
        if (mount.storage == Storage::Clone) cloned.push_back(&mount);
    }

    std::optional<std::string> snap1;
    std::optional<std::string> snap2;
    if (options.snapshot.has_value()) {
        snap1 = options.snapshot;
        if (options.targets.size() >= 2) snap2 = options.targets[1];
    } else if (options.targets.size() >= 3) {
        snap1 = options.targets[1];
        snap2 = options.targets[2];
    } else if (options.targets.size() >= 2) {
        snap1 = options.targets[1];
    }

    auto compute_diff = [&](const Mount& mount) -> fsview::Diff {
        if (snap1.has_value() && snap2.has_value()) {
            return fsview::snapshots_diff(*capsule, mount, *snap1, *snap2, options.excludes);
        }
        if (snap1.has_value()) {
            return fsview::snapshot_layer_diff(*capsule, mount, *snap1, options.excludes);
        }
        return fsview::layer_diff(*capsule, mount, options.excludes);
    };

    if (options.json) {
        json::Object root;
        root.insert_or_assign("capsule", json::Value(spec.id));
        if (snap1.has_value()) root.insert_or_assign("snapshot1", json::Value(*snap1));
        if (snap2.has_value()) root.insert_or_assign("snapshot2", json::Value(*snap2));
        json::Array layers;
        for (const Mount* mount : cloned) {
            json::Object entry;
            entry.insert_or_assign("host", json::Value(mount->host));
            entry.insert_or_assign("layer", json::Value(mount->guest));
            entry.insert_or_assign("capsule_changes", diff_to_json(compute_diff(*mount)));
            if (!snap1.has_value()) {
                entry.insert_or_assign("host_changes",
                                       diff_to_json(fsview::host_drift(*capsule, *mount, options.excludes)));
            }
            layers.emplace_back(std::move(entry));
        }
        root.insert_or_assign("layers", json::Value(std::move(layers)));
        print_json(json::Value(std::move(root)));
        return 0;
    }

    if (cloned.empty()) {
        std::printf("capsule %s has no cloned mounts, so it has no filesystem changes to\n",
                    capsule->label().c_str());
        std::printf("report. Writes under --rw went straight to the host; writes to the\n");
        std::printf("capsule's private tmp and home are discarded when it is destroyed.\n");
    }

    if (snap1.has_value() && snap2.has_value()) {
        log::info("comparing snapshot '" + *snap1 + "' against snapshot '" + *snap2 + "'");
    } else if (snap1.has_value()) {
        log::info("comparing snapshot '" + *snap1 + "' against current layer");
    }

    bool any_changes = false;
    for (const Mount* mount : cloned) {
        fsview::Diff diff = compute_diff(*mount);
        if (options.no_metadata) {
            std::vector<fsview::Change> kept;
            for (const fsview::Change& change : diff.changes) {
                if (change.kind != fsview::ChangeKind::Metadata) kept.push_back(change);
            }
            diff.changes = std::move(kept);
        }
        if (!diff.empty()) any_changes = true;
        print_diff(mount->host, diff, true);

        if (!snap1.has_value()) {
            fsview::Diff drift = fsview::host_drift(*capsule, *mount, options.excludes);
            if (!drift.empty()) {
                std::printf("\n");
                log::warn(std::to_string(drift.changes.size()) + " path" +
                          (drift.changes.size() == 1 ? "" : "s") + " under " + mount->host +
                          " changed on the host since this capsule was created; "
                          "'isolate commit' will report them as conflicts");
            }
        }
        std::printf("\n");
    }

    if (!snap1.has_value()) {
        proc::Tracker tracker(sb::membership_name(spec.id), 0);
        std::vector<proc::ProcInfo> processes = tracker.scan();
        if (!processes.empty()) {
            print_heading("processes still running");
            for (const proc::ProcInfo& info : processes) {
                std::printf("  %d %s\n", info.pid,
                            info.path.empty() ? info.name.c_str() : info.path.c_str());
            }
            std::printf("\n");
            any_changes = true;
        }

        if (spec.private_tmp || spec.private_home) {
            uint64_t scratch = 0;
            if (spec.private_tmp) scratch += fs::tree_apparent_size(capsule->tmp_dir());
            if (spec.private_home) scratch += fs::tree_apparent_size(capsule->home_dir());
            if (scratch > 0) {
                std::printf("%s in the capsule's private tmp and home, discarded when the "
                            "capsule is destroyed\n",
                            str::human_bytes(scratch).c_str());
            }
        }
    }

    return any_changes ? 1 : 0;
}

} // namespace isolate::cli
