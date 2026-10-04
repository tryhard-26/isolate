#include "fsview/layer.h"

#include "core/ids.h"
#include "util/error.h"
#include "util/fs.h"
#include "util/json.h"
#include "util/log.h"
#include "util/path.h"
#include "util/str.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <ctime>
#include <set>

namespace isolate::fsview {
namespace {

std::string snapshot_meta_path(const Capsule& capsule, const std::string& name) {
    return path::join(capsule.snapshot_dir(name), "meta.json");
}

void require_clone_mount(const Mount& mount) {
    if (mount.storage != Storage::Clone) {
        throw Error("'" + mount.host + "' is a direct host mount; it has no layer to "
                    "compare or commit. Only --cow mounts have one.");
    }
    if (mount.clone_name.empty()) {
        throw Error("mount '" + mount.host + "' has no layer name in capsule metadata");
    }
}

// Replaces dst w/ a clone of src.
void reclone_over(const std::string& src, const std::string& dst) {
    if (fs::exists(dst)) fs::remove_tree(dst);
    clone_tree(src, dst);
}

void ensure_parent_dirs(const std::string& root, const std::string& relative,
                        const std::string& source_root) {
    std::string parent = path::dirname(relative);
    if (parent == "." || parent.empty()) return;

    // Top down, copying modes from the capsule's version.
    std::vector<std::string> missing;
    std::string cursor = parent;
    while (cursor != "." && !cursor.empty()) {
        std::string full = path::join(root, cursor);
        if (fs::exists(full)) break;
        missing.push_back(cursor);
        cursor = path::dirname(cursor);
    }
    for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
        std::string full = path::join(root, *it);
        if (::mkdir(full.c_str(), 0700) != 0 && errno != EEXIST) {
            throw Error("cannot create '" + full + "'", errno);
        }
        if (auto st = fs::lstat_opt(path::join(source_root, *it)); st.has_value()) {
            fs::copy_metadata(*st, full);
        }
    }
}

// Host owner to keep: its own, else its parent's. A --user
// layer is owned by that uid; don't hand files to nobody.
bool host_owner(const std::string& to, uid_t& uid, gid_t& gid) {
    auto st = fs::lstat_opt(to);
    if (!st.has_value()) st = fs::lstat_opt(path::dirname(to));
    if (!st.has_value()) return false;
    uid = st->st_uid;
    gid = st->st_gid;
    return true;
}

void apply_change(const Change& change, const std::string& from_root,
                  const std::string& to_root) {
    std::string from = path::join(from_root, change.path);
    std::string to = path::join(to_root, change.path);

    uid_t want_uid = 0;
    gid_t want_gid = 0;
    bool keep_owner = host_owner(to, want_uid, want_gid);
    // Fails w/o root, where the layer owner is already right.
    auto restore_owner = [&] {
        if (keep_owner) ::lchown(to.c_str(), want_uid, want_gid);
    };

    switch (change.kind) {
        case ChangeKind::Deleted:
            if (fs::exists(to)) fs::remove_tree(to);
            return;

        case ChangeKind::Metadata: {
            struct stat st = fs::lstat_or_throw(from);
            fs::copy_metadata(st, to);
            restore_owner();
            return;
        }

        case ChangeKind::Added:
        case ChangeKind::Modified:
        case ChangeKind::TypeChanged:
            break;
    }

    ensure_parent_dirs(to_root, change.path, from_root);
    struct stat st = fs::lstat_or_throw(from);

    if (S_ISDIR(st.st_mode)) {
        // Diff collapses a new dir to one change, so clone it in
        // one call; on APFS that shares blocks.
        if (change.kind == ChangeKind::Added && change.contained_entries > 0) {
            if (fs::exists(to)) fs::remove_tree(to);
            clone_tree(from, to);
            fs::copy_metadata(st, to);
            if (keep_owner) fs::chown_tree(to, want_uid, want_gid);
            return;
        }
        if (fs::exists(to) && !fs::is_dir(to)) fs::remove_tree(to);
        if (!fs::exists(to)) {
            if (::mkdir(to.c_str(), 0700) != 0 && errno != EEXIST) {
                throw Error("cannot create '" + to + "'", errno);
            }
        }
        fs::copy_metadata(st, to);
        restore_owner();
        return;
    }

    if (S_ISLNK(st.st_mode)) {
        std::string target = fs::read_link(from);
        if (fs::exists(to)) fs::remove_tree(to);
        fs::write_link(target, to);
        fs::copy_metadata(st, to);
        restore_owner();
        return;
    }

    if (!S_ISREG(st.st_mode)) {
        throw Error("cannot commit '" + change.path + "': it is a " +
                    std::string(S_ISFIFO(st.st_mode) ? "named pipe" : "special file") +
                    ", which isolate does not copy");
    }

    if (fs::exists(to) && fs::is_dir(to)) fs::remove_tree(to);
    clone_file_over(from, to);
    fs::copy_metadata(st, to);
    restore_owner();
}

int64_t now_seconds() {
    return static_cast<int64_t>(std::time(nullptr));
}

} // namespace

void resolve_guest_paths(Spec& spec, const Capsule& capsule) {
    for (Mount& mount : spec.mounts) {
        if (mount.storage == Storage::Clone) {
            mount.guest = capsule.layer_dir(mount.clone_name);
        } else {
            mount.guest = mount.host;
        }
    }
}

PrepareResult prepare_layers(Capsule& capsule) {
    PrepareResult result;
    Spec& spec = capsule.mutable_spec();

    for (Mount& mount : spec.mounts) {
        if (mount.storage != Storage::Clone) {
            mount.guest = mount.host;
            continue;
        }
        require_clone_mount(mount);

        std::string work = capsule.layer_dir(mount.clone_name);
        std::string base = capsule.base_layer_dir(mount.clone_name);
        mount.guest = work;

        if (fs::exists(work)) {
            ++result.layers_reused;
            result.bytes_on_disk += fs::tree_apparent_size(work);
            if (!fs::exists(base)) {
                // No base means no answer to "what changed", so
                // rebuild from the layer. Existing changes become
                // baseline; anything else invents history.
                log::warn("capsule " + capsule.label() + " was missing its baseline for '" +
                          mount.host + "'; rebuilding it from the current layer, so earlier "
                          "changes will not appear in `isolate diff`");
                clone_tree(work, base);
            }
            continue;
        }

        if (!fs::exists(mount.host)) {
            throw Error("cannot create a layer for '" + mount.host + "': no such path");
        }

        CloneResult cloned = clone_tree(mount.host, work);
        clone_tree(work, base);

        ++result.layers_created;
        result.bytes_on_disk += cloned.bytes_on_disk;
        if (cloned.method == CloneMethod::Copy) result.any_copied = true;

        log::verbose("layer '" + mount.clone_name + "' " +
                     (cloned.method == CloneMethod::Clone ? "cloned" : "copied") + " from " +
                     mount.host + " (" + std::to_string(cloned.entries) + " entries, " +
                     str::human_bytes(cloned.bytes_logical) + " logical, " +
                     str::human_bytes(cloned.bytes_on_disk) + " on disk)");
    }
    return result;
}

namespace {

std::vector<std::string> collect_excludes(const std::string& host_root,
                                          const std::string& layer_root,
                                          const std::vector<std::string>& extra) {
    std::vector<std::string> excludes = extra;
    for (const std::string& root : {layer_root, host_root}) {
        if (root.empty()) continue;
        for (const char* name : {".isolateignore", ".gitignore"}) {
            std::string ignore_path = path::join(root, name);
            std::vector<std::string> patterns = str::load_ignore_file(ignore_path);
            excludes.insert(excludes.end(), patterns.begin(), patterns.end());
        }
    }
    return excludes;
}

} // namespace

Diff layer_diff(const Capsule& capsule, const Mount& mount,
                const std::vector<std::string>& extra_excludes) {
    require_clone_mount(mount);
    DiffOptions o;
    o.ignore_ownership = !capsule.spec().credentials.user.empty();
    std::string work = capsule.layer_dir(mount.clone_name);
    o.exclude = collect_excludes(mount.host, work, extra_excludes);
    return diff_trees(capsule.base_layer_dir(mount.clone_name), work, o);
}

Diff host_drift(const Capsule& capsule, const Mount& mount,
                const std::vector<std::string>& extra_excludes) {
    require_clone_mount(mount);
    DiffOptions o;
    o.ignore_ownership = !capsule.spec().credentials.user.empty();
    o.exclude = collect_excludes(mount.host, capsule.layer_dir(mount.clone_name), extra_excludes);
    return diff_trees(capsule.base_layer_dir(mount.clone_name), mount.host, o);
}

Diff snapshot_layer_diff(const Capsule& capsule, const Mount& mount,
                         const std::string& snapshot_name,
                         const std::vector<std::string>& extra_excludes) {
    require_clone_mount(mount);
    DiffOptions o;
    o.ignore_ownership = !capsule.spec().credentials.user.empty();
    std::string snap_layer =
        path::join(path::join(capsule.snapshot_dir(snapshot_name), "layers"), mount.clone_name);
    if (!fs::exists(snap_layer)) {
        throw Error("snapshot '" + snapshot_name + "' has no layer for mount '" + mount.host + "'");
    }
    std::string current_layer = capsule.layer_dir(mount.clone_name);
    o.exclude = collect_excludes(mount.host, current_layer, extra_excludes);
    return diff_trees(snap_layer, current_layer, o);
}

Diff snapshots_diff(const Capsule& capsule, const Mount& mount,
                   const std::string& snap1, const std::string& snap2,
                   const std::vector<std::string>& extra_excludes) {
    require_clone_mount(mount);
    DiffOptions o;
    o.ignore_ownership = !capsule.spec().credentials.user.empty();
    std::string snap1_layer =
        path::join(path::join(capsule.snapshot_dir(snap1), "layers"), mount.clone_name);
    std::string snap2_layer =
        path::join(path::join(capsule.snapshot_dir(snap2), "layers"), mount.clone_name);
    if (!fs::exists(snap1_layer)) {
        throw Error("snapshot '" + snap1 + "' has no layer for mount '" + mount.host + "'");
    }
    if (!fs::exists(snap2_layer)) {
        throw Error("snapshot '" + snap2 + "' has no layer for mount '" + mount.host + "'");
    }
    o.exclude = collect_excludes(mount.host, snap2_layer, extra_excludes);
    return diff_trees(snap1_layer, snap2_layer, o);
}

CommitPlan plan_commit(const Capsule& capsule, const Mount& mount,
                       const std::vector<std::string>& extra_excludes) {
    CommitPlan plan;
    Diff capsule_changes = layer_diff(capsule, mount, extra_excludes);
    Diff drift = host_drift(capsule, mount, extra_excludes);

    std::set<std::string> host_changed;
    for (const Change& c : drift.changes) host_changed.insert(c.path);

    for (const Change& c : capsule_changes.changes) {
        if (host_changed.count(c.path) != 0) {
            plan.conflicts.push_back(c);
        } else {
            plan.apply.push_back(c);
        }
    }
    return plan;
}

CommitResult commit_layer(Capsule& capsule, const Mount& mount, const CommitPlan& plan,
                          bool force) {
    require_clone_mount(mount);
    CommitResult result;

    std::vector<Change> todo = plan.apply;
    if (force) {
        todo.insert(todo.end(), plan.conflicts.begin(), plan.conflicts.end());
    } else {
        result.skipped = static_cast<int>(plan.conflicts.size());
    }

    // Creates parents first, deletes children first.
    std::vector<Change> creates;
    std::vector<Change> deletes;
    for (const Change& c : todo) {
        if (c.kind == ChangeKind::Deleted) deletes.push_back(c);
        else creates.push_back(c);
    }
    std::sort(creates.begin(), creates.end(),
              [](const Change& a, const Change& b) { return a.path < b.path; });
    std::sort(deletes.begin(), deletes.end(),
              [](const Change& a, const Change& b) { return a.path > b.path; });

    std::string work = capsule.layer_dir(mount.clone_name);
    for (const std::vector<Change>* batch : {&creates, &deletes}) {
        for (const Change& c : *batch) {
            try {
                apply_change(c, work, mount.host);
                ++result.applied;
            } catch (const Error& e) {
                result.failures.push_back(c.path + ": " + e.what());
            }
        }
    }

    // Re-clone base so the next diff reports only what follows.
    // On partial failure it still matches the host as it now is.
    reclone_over(mount.host, capsule.base_layer_dir(mount.clone_name));
    return result;
}

void snapshot_create(Capsule& capsule, const std::string& name, const std::string& note) {
    if (!is_valid_snapshot_name(name)) {
        throw Error("'" + name + "' is not a valid snapshot name; use letters, digits, "
                    "'.', '_', or '-'");
    }
    std::string dir = capsule.snapshot_dir(name);
    if (fs::exists(dir)) throw Error("snapshot '" + name + "' already exists");

    fs::mkdir_p(dir, 0700);
    bool ok = false;
    // A half-written snapshot would restore a tree that never
    // existed, so drop it unless every clone succeeded.
    struct Cleanup {
        const std::string& dir;
        const bool& ok;
        ~Cleanup() {
            if (!ok) {
                try {
                    fs::remove_tree(dir);
                } catch (const Error&) {
                }
            }
        }
    } cleanup{dir, ok};

    clone_tree(capsule.layers_dir(), path::join(dir, "layers"));
    clone_tree(capsule.base_dir(), path::join(dir, "base"));
    if (capsule.spec().private_home && fs::exists(capsule.home_dir())) {
        clone_tree(capsule.home_dir(), path::join(dir, "home"));
    }

    json::Object meta;
    meta.insert_or_assign("name", json::Value(name));
    meta.insert_or_assign("created_at", json::Value(now_seconds()));
    meta.insert_or_assign("note", json::Value(note));
    meta.insert_or_assign("capsule_id", json::Value(capsule.spec().id));
    fs::write_file_atomic(snapshot_meta_path(capsule, name),
                          json::dump(json::Value(std::move(meta))), 0600);
    ok = true;
}

void snapshot_restore(Capsule& capsule, const std::string& name) {
    std::string dir = capsule.snapshot_dir(name);
    if (!fs::is_dir(dir)) throw Error("no snapshot '" + name + "'");

    std::string layers = path::join(dir, "layers");
    std::string base = path::join(dir, "base");
    if (!fs::is_dir(layers) || !fs::is_dir(base)) {
        throw Error("snapshot '" + name + "' is incomplete and cannot be restored");
    }

    reclone_over(layers, capsule.layers_dir());
    reclone_over(base, capsule.base_dir());

    std::string home = path::join(dir, "home");
    if (fs::is_dir(home)) {
        reclone_over(home, capsule.home_dir());
    } else if (capsule.spec().private_home) {
        if (fs::exists(capsule.home_dir())) fs::remove_tree(capsule.home_dir());
        fs::mkdir_p(capsule.home_dir(), 0700);
    }

    // Not snapshotted; empty it so no stale sockets or locks.
    if (capsule.spec().private_tmp) {
        if (fs::exists(capsule.tmp_dir())) fs::remove_tree(capsule.tmp_dir());
        fs::mkdir_p(capsule.tmp_dir(), 0700);
    }
}

void snapshot_delete(Capsule& capsule, const std::string& name) {
    std::string dir = capsule.snapshot_dir(name);
    if (!fs::is_dir(dir)) throw Error("no snapshot '" + name + "'");
    fs::remove_tree(dir);
}

std::vector<SnapshotInfo> snapshot_list(const Capsule& capsule) {
    std::vector<SnapshotInfo> out;
    for (const std::string& name : capsule.snapshots()) {
        SnapshotInfo info;
        info.name = name;
        std::string meta = snapshot_meta_path(capsule, name);
        if (fs::exists(meta)) {
            try {
                json::Value v = json::parse(fs::read_file(meta));
                info.created_at = v.get_int("created_at", 0);
                info.note = v.get_string("note", "");
            } catch (const Error& e) {
                log::warn("snapshot '" + name + "' has unreadable metadata: " + e.what());
            }
        }
        info.bytes_on_disk = fs::tree_apparent_size(capsule.snapshot_dir(name));
        out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(), [](const SnapshotInfo& a, const SnapshotInfo& b) {
        if (a.created_at != b.created_at) return a.created_at > b.created_at;
        return a.name < b.name;
    });
    return out;
}

uint64_t capsule_apparent_size(const Capsule& capsule) {
    return fs::tree_apparent_size(capsule.dir());
}

} // namespace isolate::fsview
