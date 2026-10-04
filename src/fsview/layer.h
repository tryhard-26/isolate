#pragma once

#include "core/store.h"
#include "fsview/clone.h"
#include "fsview/diff.h"

#include <string>
#include <vector>

namespace isolate::fsview {

struct PrepareResult {
    int layers_created = 0;
    int layers_reused = 0;
    uint64_t bytes_on_disk = 0;
    bool any_copied = false; // had to copy, not clone
};

// Clones working + base layers for CoW mounts; idempotent.
PrepareResult prepare_layers(Capsule& capsule);

// Compute guest-visible paths without disk writes.
void resolve_guest_paths(Spec& spec, const Capsule& capsule);

// Diff base vs layer (capsule modifications).
Diff layer_diff(const Capsule& capsule, const Mount& mount,
                const std::vector<std::string>& extra_excludes = {});

// Diff base vs host (outside drift since capsule creation).
Diff host_drift(const Capsule& capsule, const Mount& mount,
                const std::vector<std::string>& extra_excludes = {});

// Diff snapshot vs layer.
Diff snapshot_layer_diff(const Capsule& capsule, const Mount& mount,
                         const std::string& snapshot_name,
                         const std::vector<std::string>& extra_excludes = {});

// Diff snap1 vs snap2.
Diff snapshots_diff(const Capsule& capsule, const Mount& mount,
                   const std::string& snap1, const std::string& snap2,
                   const std::vector<std::string>& extra_excludes = {});

struct CommitPlan {
    std::vector<Change> apply;
    std::vector<Change> conflicts; // Changed in both capsule and host

    bool empty() const { return apply.empty() && conflicts.empty(); }
};

CommitPlan plan_commit(const Capsule& capsule, const Mount& mount,
                       const std::vector<std::string>& extra_excludes = {});

struct CommitResult {
    int applied = 0;
    int skipped = 0;
    std::vector<std::string> failures;
};

// Write changes back to host; re-clones base layer.
CommitResult commit_layer(Capsule& capsule, const Mount& mount, const CommitPlan& plan,
                          bool force);

struct SnapshotInfo {
    std::string name;
    int64_t created_at = 0;
    uint64_t bytes_on_disk = 0;
    std::string note;
};

// Snapshot capsule state (layers + base + home).
void snapshot_create(Capsule& capsule, const std::string& name, const std::string& note);

// Restore capsule state from snapshot (proc must be stopped).
void snapshot_restore(Capsule& capsule, const std::string& name);

void snapshot_delete(Capsule& capsule, const std::string& name);

std::vector<SnapshotInfo> snapshot_list(const Capsule& capsule);

// Apparent byte size across all layers.
uint64_t capsule_apparent_size(const Capsule& capsule);

} // namespace isolate::fsview
