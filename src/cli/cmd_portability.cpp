#include "cli/commands.h"

#include "cli/format.h"
#include "core/ids.h"
#include "fsview/layer.h"
#include "util/error.h"
#include "util/fs.h"
#include "util/log.h"
#include "util/path.h"
#include "util/str.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

namespace isolate::cli {
namespace {

int run_tar(const std::vector<std::string>& args) {
    pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        std::vector<char*> raw;
        for (const std::string& a : args) raw.push_back(const_cast<char*>(a.c_str()));
        raw.push_back(nullptr);
        ::execv("/usr/bin/tar", raw.data());
        ::_exit(127);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

} // namespace

int cmd_export(const Options& options, Store& store) {
    const std::string& reference = require_target(options, "a capsule to export");
    std::unique_ptr<Capsule> capsule = store.open(reference, false);

    std::string archive_path;
    if (options.targets.size() >= 2) {
        archive_path = options.targets[1];
    } else {
        std::string base = capsule->spec().name.empty() ? capsule->spec().id : capsule->spec().name;
        archive_path = base + ".tar.gz";
    }

    archive_path = path::resolve_lenient(archive_path);
    if (fs::exists(archive_path) && !options.force) {
        throw Error("archive '" + archive_path + "' already exists; use --force to overwrite");
    }

    log::info("exporting capsule " + capsule->label() + " to " + archive_path);
    int rc = run_tar({"/usr/bin/tar", "-czf", archive_path, "-C", capsule->dir(), "."});
    if (rc != 0) {
        throw Error("failed to create archive '" + archive_path + "' with /usr/bin/tar");
    }

    uint64_t size = 0;
    if (auto st = fs::lstat_opt(archive_path); st.has_value()) {
        size = static_cast<uint64_t>(st->st_size);
    }

    if (options.json) {
        json::Object root;
        root.insert_or_assign("capsule", json::Value(capsule->spec().id));
        root.insert_or_assign("archive", json::Value(archive_path));
        root.insert_or_assign("size_bytes", json::Value(size));
        print_json(json::Value(std::move(root)));
        return 0;
    }

    std::printf("exported capsule %s to %s (%s)\n", capsule->label().c_str(),
                archive_path.c_str(), str::human_bytes(size).c_str());
    return 0;
}

int cmd_import(const Options& options, Store& store) {
    const std::string& archive_path = require_target(options, "an archive file to import");
    std::string resolved_archive = path::resolve_lenient(archive_path);
    if (!fs::exists(resolved_archive)) {
        throw Error("archive file '" + archive_path + "' does not exist");
    }

    std::string staging =
        path::join(store.root(), "import-tmp-" + std::to_string(::getpid()) + "-" +
                                     generate_capsule_id());
    fs::mkdir_p(staging, 0700);

    struct Cleanup {
        std::string dir;
        ~Cleanup() {
            if (!dir.empty() && fs::exists(dir)) {
                try {
                    fs::remove_tree(dir);
                } catch (...) {
                }
            }
        }
    } cleanup{staging};

    int rc = run_tar({"/usr/bin/tar", "-xzf", resolved_archive, "-C", staging});
    if (rc != 0) {
        throw Error("failed to extract archive '" + resolved_archive + "' with /usr/bin/tar");
    }

    std::string spec_file = path::join(staging, "capsule.json");
    if (!fs::exists(spec_file)) {
        throw Error("archive '" + archive_path + "' is missing capsule.json; not a valid capsule archive");
    }

    std::string json_text = fs::read_file(spec_file);
    json::Value parsed = json::parse(json_text);
    Spec spec = Spec::from_json(parsed);

    // Assign fresh unique ID
    spec.id = generate_capsule_id();
    spec.created_at = static_cast<int64_t>(std::time(nullptr));
    if (options.name.has_value()) {
        spec.name = *options.name;
    }

    // Write updated spec
    std::string updated_spec = json::dump(spec.to_json());
    fs::write_file_atomic(spec_file, updated_spec, 0600);

    std::string final_dir = path::join(path::join(store.root(), "capsules"), spec.id);
    if (fs::exists(final_dir)) {
        throw Error("capsule directory '" + final_dir + "' already exists");
    }

    fs::mkdir_p(path::dirname(final_dir), 0700);
    if (::rename(staging.c_str(), final_dir.c_str()) != 0) {
        throw Error("cannot move imported capsule to store", errno);
    }
    cleanup.dir.clear(); // Successfully moved

    if (options.json) {
        json::Object root;
        root.insert_or_assign("id", json::Value(spec.id));
        root.insert_or_assign("name", json::Value(spec.name));
        root.insert_or_assign("directory", json::Value(final_dir));
        print_json(json::Value(std::move(root)));
        return 0;
    }

    std::string label = spec.name.empty() ? spec.id : spec.id + " (" + spec.name + ")";
    std::printf("imported capsule %s from %s\n", label.c_str(), archive_path.c_str());
    return 0;
}

} // namespace isolate::cli
