#include "cli/commands.h"

#include "cli/format.h"
#include "fsview/layer.h"
#include "util/log.h"
#include "util/str.h"

#include <cstdio>

namespace isolate::cli {

int cmd_list(const Options& options, Store& store) {
    std::vector<std::unique_ptr<Capsule>> capsules = store.list();

    if (options.json) {
        json::Array array;
        for (const std::unique_ptr<Capsule>& capsule : capsules) {
            array.push_back(capsule_to_json(*capsule, false));
        }
        print_json(json::Value(std::move(array)));
        return 0;
    }

    if (capsules.empty()) {
        std::printf("no capsules\n");
        return 0;
    }

    std::vector<std::vector<std::string>> rows;
    for (const std::unique_ptr<Capsule>& capsule : capsules) {
        const Spec& spec = capsule->spec();

        // More identifying than the command, usually a shell.
        std::string sources;
        for (const Mount& mount : spec.mounts) {
            if (mount.storage != Storage::Clone) continue;
            if (!sources.empty()) sources += ", ";
            sources += mount.host;
        }
        if (sources.empty() && !spec.command.empty()) sources = spec.command.front();
        if (sources.size() > 48) sources = "..." + sources.substr(sources.size() - 45);

        std::string status = to_string(capsule->state().status);
        if (spec.ephemeral) status += ", ephemeral";

        rows.push_back({
            spec.id,
            spec.name.empty() ? "-" : spec.name,
            status,
            str::human_bytes(fsview::capsule_apparent_size(*capsule)),
            format_relative(spec.created_at),
            sources,
        });
    }

    print_table({"ID", "NAME", "STATUS", "SIZE", "CREATED", "SOURCE"}, rows);
    return 0;
}

} // namespace isolate::cli
