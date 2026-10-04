#include "cli/commands.h"

#include "cli/format.h"
#include "fsview/clone.h"
#include "fsview/layer.h"
#include "proc/limits.h"
#include "sb/profile.h"
#include "sb/services.h"
#include "sb/tag.h"
#include "util/error.h"
#include "util/log.h"
#include "util/str.h"

#include <cstdio>

namespace isolate::cli {

int cmd_inspect(const Options& options, Store& store) {
    const std::string& reference = require_target(options, "a capsule to inspect");
    std::unique_ptr<Capsule> capsule = store.open(reference, false);
    Spec& spec = capsule->mutable_spec();
    fsview::resolve_guest_paths(spec, *capsule);

    if (options.show_profile) {
        // Same inputs the next run uses.
        std::string program = spec.command.empty() ? "" : spec.command.front();
        std::string profile =
            sb::build_profile(profile_inputs_for(*capsule, program, "/dev/ttys000", false));
        std::fwrite(profile.data(), 1, profile.size(), stdout);
        return 0;
    }

    if (options.json) {
        print_json(capsule_to_json(*capsule, true));
        return 0;
    }

    print_heading("capsule " + spec.id);
    std::vector<std::pair<std::string, std::string>> fields;
    if (!spec.name.empty()) fields.emplace_back("name", spec.name);
    fields.emplace_back("status", to_string(capsule->state().status));
    fields.emplace_back("lifetime", spec.ephemeral ? "ephemeral" : "persistent");
    fields.emplace_back("created", format_timestamp(spec.created_at) + " (" +
                                       format_relative(spec.created_at) + ")");
    fields.emplace_back("runs", std::to_string(capsule->state().run_count));
    if (capsule->state().last_exit_code.has_value()) {
        std::string last = "status " + std::to_string(*capsule->state().last_exit_code);
        if (!capsule->state().last_termination.empty()) {
            last += ", " + capsule->state().last_termination;
        }
        fields.emplace_back("last run", last);
    }
    fields.emplace_back("directory", capsule->dir());
    fields.emplace_back("apparent size",
                        str::human_bytes(fsview::capsule_apparent_size(*capsule)) +
                            " (cloned blocks are shared with the originals and cost no "
                            "additional space)");
    std::string command;
    for (const std::string& arg : spec.command) {
        if (!command.empty()) command += " ";
        command += str::shell_quote(arg);
    }
    fields.emplace_back("command", command);
    if (!spec.cwd.empty()) fields.emplace_back("working directory", spec.cwd);
    print_fields(fields);

    std::printf("\n");
    print_heading("filesystem");
    if (spec.mounts.empty()) {
        std::printf("  nothing mounted: the capsule can read the system and the base tools,\n");
        std::printf("  and write only to its own private directories\n");
    } else {
        std::vector<std::vector<std::string>> rows;
        for (const Mount& mount : spec.mounts) {
            std::string mode = to_string(mount.access);
            if (mount.storage == Storage::Clone) mode = "cow";
            std::string seen_as = mount.storage == Storage::Clone ? mount.guest : "in place";
            std::string note;
            if (mount.primary) note = "working directory";
            if (mount.storage == Storage::Clone) {
                std::string fstype = fsview::filesystem_type(mount.host);
                if (fstype != "apfs") {
                    note += note.empty() ? "" : "; ";
                    note += "copied, not cloned (" + fstype + ")";
                }
            }
            rows.push_back({mode, mount.host, seen_as, note});
        }
        print_table({"MODE", "HOST PATH", "SEEN AS", "NOTES"}, rows);
    }

    std::vector<std::pair<std::string, std::string>> privacy;
    privacy.emplace_back("private tmp", spec.private_tmp ? capsule->tmp_dir() : "no");
    privacy.emplace_back("private home", spec.private_home ? capsule->home_dir() : "no");
    privacy.emplace_back("metadata outside policy",
                         spec.permissive_metadata ? "visible" : "hidden");
    std::printf("\n");
    print_fields(privacy);

    std::printf("\n");
    print_heading("services and network");
    std::vector<std::pair<std::string, std::string>> svc;
    svc.emplace_back("bundle", std::string(to_string(spec.services)) + " - " +
                                   sb::bundle_description(spec.services));
    std::vector<std::string> names = sb::services_for(spec);
    svc.emplace_back("services allowed", std::to_string(names.size()));
    svc.emplace_back("network", to_string(spec.network));
    print_fields(svc);
    if (log::level() >= log::Level::Verbose) {
        for (const std::string& name : names) std::printf("    %s\n", name.c_str());
    }

    std::printf("\n");
    print_heading("limits");
    std::vector<proc::LimitNote> notes = proc::describe(spec.limits);
    if (notes.empty()) {
        std::printf("  none\n");
    } else {
        std::vector<std::vector<std::string>> rows;
        for (const proc::LimitNote& note : notes) {
            rows.push_back({note.name, note.value,
                            note.kernel_enforced ? "kernel" : "isolate", note.mechanism});
        }
        print_table({"LIMIT", "VALUE", "ENFORCED BY", "MECHANISM"}, rows);
    }

    std::printf("\n");
    print_heading("credentials");
    std::vector<std::pair<std::string, std::string>> cred;
    cred.emplace_back("user", spec.credentials.user.empty()
                                  ? "unchanged (isolate cannot change uid without root)"
                                  : spec.credentials.user);
    char umask_text[8];
    std::snprintf(umask_text, sizeof umask_text, "%04o", spec.credentials.umask);
    cred.emplace_back("umask", umask_text);
    cred.emplace_back("privilege helpers",
                      spec.credentials.block_setuid_exec ? "denied by path" : "allowed");
    print_fields(cred);

    std::vector<fsview::SnapshotInfo> snapshots = fsview::snapshot_list(*capsule);
    if (!snapshots.empty()) {
        std::printf("\n");
        print_heading("snapshots");
        std::vector<std::vector<std::string>> rows;
        for (const fsview::SnapshotInfo& info : snapshots) {
            rows.push_back({info.name, format_relative(info.created_at),
                            str::human_bytes(info.bytes_on_disk), info.note});
        }
        print_table({"NAME", "CREATED", "DISK", "NOTE"}, rows);
    }

    return 0;
}

} // namespace isolate::cli
