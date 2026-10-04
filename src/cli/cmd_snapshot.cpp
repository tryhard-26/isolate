#include "cli/commands.h"

#include "cli/format.h"
#include "fsview/layer.h"
#include "util/error.h"
#include "util/fs.h"
#include "util/log.h"
#include "util/str.h"

#include <cstdio>
#include <ctime>

namespace isolate::cli {
namespace {

std::string default_snapshot_name() {
    std::time_t now = std::time(nullptr);
    std::tm tm {};
    localtime_r(&now, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
    return buf;
}

} // namespace

int cmd_snapshot(const Options& options, Store& store) {
    if (options.targets.empty()) {
        throw Error("expected a subcommand: create, ls, restore, or rm");
    }

    const std::string& action = options.targets[0];
    auto capsule_reference = [&]() -> const std::string& {
        if (options.targets.size() < 2) throw Error("expected a capsule");
        return options.targets[1];
    };
    auto snapshot_name = [&](bool required) -> std::string {
        if (options.snapshot.has_value()) return *options.snapshot;
        if (options.targets.size() >= 3) return options.targets[2];
        if (required) throw Error("expected a snapshot name");
        return default_snapshot_name();
    };

    if (action == "ls" || action == "list") {
        std::unique_ptr<Capsule> capsule = store.open(capsule_reference(), false);
        std::vector<fsview::SnapshotInfo> snapshots = fsview::snapshot_list(*capsule);

        if (options.json) {
            json::Array array;
            for (const fsview::SnapshotInfo& info : snapshots) {
                json::Object o;
                o.insert_or_assign("name", json::Value(info.name));
                o.insert_or_assign("created_at", json::Value(info.created_at));
                o.insert_or_assign("disk_bytes", json::Value(info.bytes_on_disk));
                o.insert_or_assign("note", json::Value(info.note));
                array.emplace_back(std::move(o));
            }
            print_json(json::Value(std::move(array)));
            return 0;
        }

        if (snapshots.empty()) {
            std::printf("capsule %s has no snapshots\n", capsule->label().c_str());
            return 0;
        }
        std::vector<std::vector<std::string>> rows;
        for (const fsview::SnapshotInfo& info : snapshots) {
            rows.push_back({info.name, format_timestamp(info.created_at),
                            str::human_bytes(info.bytes_on_disk), info.note});
        }
        print_table({"NAME", "CREATED", "DISK", "NOTE"}, rows);
        return 0;
    }

    if (action == "create" || action == "new") {
        std::unique_ptr<Capsule> capsule = store.open(capsule_reference(), true);
        if (capsule->state().status == Status::Running) {
            throw Error("capsule " + capsule->label() +
                        " is running; a snapshot taken now would capture its files "
                        "mid-write. Stop it first.");
        }
        std::string name = snapshot_name(false);
        fsview::snapshot_create(*capsule, name, options.note.value_or(""));

        uint64_t referenced = fs::tree_apparent_size(capsule->snapshot_dir(name));
        log::info("snapshot '" + name + "' references " + str::human_bytes(referenced) +
                  ", shared with the capsule's layers rather than copied; it only starts "
                  "costing space as those layers are written");
        return 0;
    }

    if (action == "restore") {
        std::unique_ptr<Capsule> capsule = store.open(capsule_reference(), true);
        if (capsule->state().status == Status::Running) {
            throw Error("capsule " + capsule->label() +
                        " is running; stop it before restoring a snapshot");
        }
        std::string name = snapshot_name(true);

        // Say what is about to be lost unless already forced.
        Spec& spec = capsule->mutable_spec();
        fsview::resolve_guest_paths(spec, *capsule);
        size_t pending = 0;
        for (const Mount& mount : spec.mounts) {
            if (mount.storage != Storage::Clone) continue;
            pending += fsview::layer_diff(*capsule, mount).changes.size();
        }
        if (pending > 0 && !options.force) {
            throw Error("capsule " + capsule->label() + " has " + std::to_string(pending) +
                        " uncommitted change" + (pending == 1 ? "" : "s") +
                        " that restoring '" + name +
                        "' would discard. Commit them, or pass --force.");
        }

        fsview::snapshot_restore(*capsule, name);
        log::info("capsule " + capsule->label() + " restored to snapshot '" + name + "'");
        return 0;
    }

    if (action == "rm" || action == "delete") {
        std::unique_ptr<Capsule> capsule = store.open(capsule_reference(), true);
        std::string name = snapshot_name(true);
        fsview::snapshot_delete(*capsule, name);
        log::info("snapshot '" + name + "' deleted");
        return 0;
    }

    throw Error("unknown snapshot subcommand '" + action +
                "'; expected create, ls, restore, or rm");
}

} // namespace isolate::cli
