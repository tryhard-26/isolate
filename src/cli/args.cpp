#include "cli/args.h"

#include "core/ids.h"
#include "util/error.h"
#include "util/fs.h"
#include "util/log.h"
#include "util/path.h"
#include "util/str.h"

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <set>

namespace isolate::cli {
namespace {

// Parsing stops at the first non-option for these.
bool takes_program(const std::string& command) {
    return command == "run" || command == "enter";
}

struct CommandHelp {
    const char* name;
    const char* summary;
    const char* usage;
    const char* detail;
};

const CommandHelp kCommands[] = {
    {"run", "run a program in a new capsule",
     "isolate run [options] <program> [arguments...]",
     "  Creates a capsule, runs the program inside it, and waits for it and\n"
     "  everything it starts to finish.\n"
     "\n"
     "  With no filesystem options the capsule can read the system and the base\n"
     "  tools, and write only to its own private temporary and home directories.\n"
     "  Networking is off. Nothing under /Users is reachable.\n"},
    {"enter", "run another program in an existing capsule",
     "isolate enter [options] <capsule> [program] [arguments...]",
     "  Starts a program in a capsule that already exists, under the policy that\n"
     "  capsule was created with. With no program, runs an interactive shell.\n"
     "  The filesystem changes from earlier runs are still there.\n"},
    {"ls", "list capsules", "isolate ls [--all] [--json]",
     "  Shows each capsule's identifier, name, status, and disk use. Disk use is\n"
     "  the space actually allocated, which for an untouched clone is close to\n"
     "  zero because its blocks are shared with the original.\n"},
    {"inspect", "show a capsule's policy and state",
     "isolate inspect [--profile] [--json] <capsule>",
     "  Prints the capsule's filesystem policy, service bundle, network setting,\n"
     "  and resource limits, marking for each limit whether the kernel enforces\n"
     "  it or isolate does. With --profile, prints the generated sandbox profile.\n"},
    {"ps", "list a capsule's processes", "isolate ps [--json] <capsule>",
     "  Lists the processes the kernel confirms belong to this capsule, including\n"
     "  any that were reparented away from it.\n"},
    {"diff", "show what a capsule changed",
     "isolate diff [--snapshot <name>] [--exclude <glob>] [--no-metadata] [--json] <capsule>",
     "  Compares each cloned mount against the baseline or a specified snapshot.\n"
     "  Files that still share their blocks with the original are proven unchanged\n"
     "  without reading them. Exclude patterns and .isolateignore/.gitignore apply.\n"},
    {"commit", "write a capsule's changes back to the host",
     "isolate commit [--dry-run] [--force] <capsule>",
     "  Applies the capsule's changes to the original paths. Paths that changed\n"
     "  on the host as well are reported and withheld unless --force is given.\n"},
    {"snapshot", "create, list, restore, or delete snapshots",
     "isolate snapshot <create|ls|restore|rm> <capsule> [name]",
     "  A snapshot is a clone of the capsule's layers, so taking one costs\n"
     "  almost no space and restoring one returns the capsule to that state.\n"},
    {"kill", "stop a capsule or one of its processes",
     "isolate kill [--signal NAME] <capsule> [pid]",
     "  With no pid, signals every process in the capsule. With a pid, signals\n"
     "  just that process, after checking it belongs to the capsule.\n"},
    {"destroy", "delete a capsule", "isolate destroy [--force] <capsule>...",
     "  Removes the capsule directory, including its layers and snapshots. The\n"
     "  host paths it was cloned from are not touched. Refuses to run while the\n"
     "  capsule has uncommitted changes unless --force is given.\n"},
    {"doctor", "check what this system supports", "isolate doctor [--json]",
     "  Tests each facility isolate depends on against the running kernel and\n"
     "  reports what works, what does not, and what that means for isolation.\n"},
    {"gc", "garbage-collect abandoned or stale capsules",
     "isolate gc [--older-than <dur>] [--dry-run] [--all] [--json]",
     "  Prunes abandoned ephemeral capsules and old stopped capsules from disk,\n"
     "  reclaiming storage space. Only stopped and unlocked capsules are pruned.\n"},
    {"export", "export a capsule to a portable archive",
     "isolate export [--force] [--json] <capsule> [archive.tar.gz]",
     "  Packs capsule specification, layers, and snapshots into a compressed\n"
     "  tar.gz archive suitable for archiving or transfer.\n"},
    {"import", "import a capsule from a portable archive",
     "isolate import [--name <name>] [--json] <archive.tar.gz>",
     "  Unpacks and registers an exported capsule archive, assigning a fresh\n"
     "  unique identifier while preserving layers and configuration.\n"},
};

const CommandHelp* find_command(const std::string& name) {
    for (const CommandHelp& c : kCommands) {
        if (name == c.name) return &c;
    }
    return nullptr;
}

// "--opt=value" or "--opt value".
std::string take_value(const std::string& option, const std::string& inline_value,
                       bool has_inline, int& index, int argc, char** argv) {
    if (has_inline) return inline_value;
    if (index + 1 >= argc) {
        throw Error("option " + option + " needs a value");
    }
    return argv[++index];
}

void add_mount(Options& options, const std::string& raw, Access access, Storage storage) {
    if (raw.empty()) throw Error("empty path in a filesystem option");

    Mount mount;
    mount.access = access;
    mount.storage = storage;
    // The sandbox matches resolved paths: a rule for /tmp/x
    // never matches, the kernel sees /private/tmp/x.
    mount.host = path::resolve_lenient(raw);
    options.mounts.push_back(std::move(mount));
}

void parse_env_assignment(Options& options, const std::string& raw) {
    size_t eq = raw.find('=');
    if (eq == std::string::npos) {
        // Bare NAME passes the caller's value through.
        if (raw.empty()) throw Error("--env needs a name");
        options.env.passthrough.push_back(raw);
        return;
    }
    if (eq == 0) throw Error("--env needs a name before '='");
    options.env.set.emplace_back(raw.substr(0, eq), raw.substr(eq + 1));
}

// atoi would make "abc" 0, then the validator would complain
// about a limit the user never asked for.
int parse_count(const std::string& option, const std::string& raw) {
    char* end = nullptr;
    long value = std::strtol(raw.c_str(), &end, 10);
    if (!end || *end != '\0' || raw.empty() || value <= 0 || value > 1000000) {
        throw Error(option + " needs a positive whole number, not '" + raw + "'");
    }
    return static_cast<int>(value);
}

int signal_from_name(const std::string& raw) {
    std::string name = str::lower(raw);
    if (str::starts_with(name, "sig")) name = name.substr(3);

    struct Entry {
        const char* name;
        int number;
    };
    static const Entry kSignals[] = {
        {"hup", SIGHUP},   {"int", SIGINT},   {"quit", SIGQUIT}, {"kill", SIGKILL},
        {"term", SIGTERM}, {"usr1", SIGUSR1}, {"usr2", SIGUSR2}, {"stop", SIGSTOP},
        {"cont", SIGCONT},
    };
    for (const Entry& e : kSignals) {
        if (name == e.name) return e.number;
    }
    // Bare number too.
    char* end = nullptr;
    long value = std::strtol(raw.c_str(), &end, 10);
    if (end && *end == '\0' && value > 0 && value < NSIG) return static_cast<int>(value);
    throw Error("unknown signal '" + raw + "'");
}

// The /usr/bin shims need this to find the real clang/git/python.
std::vector<std::string> developer_tool_paths() {
    std::vector<std::string> paths;
    // Which link holds it has changed across releases.
    const char* kLinks[] = {"/private/var/select/developer_dir",
                            "/private/var/db/xcode_select_link"};
    for (const char* link : kLinks) {
        if (!fs::is_symlink(link)) continue;
        try {
            std::string target = path::resolve(link);
            if (fs::is_dir(target)) paths.push_back(target);
        } catch (const Error&) {
            // Dangling == no toolchain selected.
        }
    }
    for (const char* fallback : {"/Library/Developer/CommandLineTools"}) {
        if (fs::is_dir(fallback)) paths.emplace_back(fallback);
    }
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    return paths;
}

} // namespace

Options parse(int argc, char** argv) {
    Options options;
    if (argc < 2) {
        options.help = true;
        return options;
    }

    int index = 1;
    std::string first = argv[index];
    if (first == "-h" || first == "--help") {
        options.help = true;
        return options;
    }
    if (first == "--version" || first == "-V" || first == "version") {
        options.version = true;
        return options;
    }
    if (str::starts_with(first, "-")) {
        throw Error("expected a command before options; try 'isolate --help'");
    }
    options.command = first;
    ++index;

    const bool stop_at_program = takes_program(options.command);
    bool seen_program = false;

    for (; index < argc; ++index) {
        std::string arg = argv[index];

        if (seen_program) {
            options.program.push_back(arg);
            continue;
        }

        if (arg == "--") {
            // Rest goes to the program, or to the cmd as positionals.
            for (++index; index < argc; ++index) {
                if (stop_at_program) options.program.emplace_back(argv[index]);
                else options.targets.emplace_back(argv[index]);
            }
            break;
        }

        if (!str::starts_with(arg, "-")) {
            if (stop_at_program && options.command == "run") {
                seen_program = true;
                options.program.push_back(arg);
                continue;
            }
            if (stop_at_program && options.command == "enter" && !options.targets.empty()) {
                seen_program = true;
                options.program.push_back(arg);
                continue;
            }
            options.targets.push_back(arg);
            continue;
        }

        std::string name = arg;
        std::string inline_value;
        bool has_inline = false;
        if (size_t eq = arg.find('='); eq != std::string::npos && str::starts_with(arg, "--")) {
            name = arg.substr(0, eq);
            inline_value = arg.substr(eq + 1);
            has_inline = true;
        }

        auto value = [&]() {
            return take_value(name, inline_value, has_inline, index, argc, argv);
        };

        if (name == "-h" || name == "--help") {
            options.help = true;
        } else if (name == "-v" || name == "--verbose") {
            ++options.verbosity;
        } else if (name == "-vv" || name == "--debug") {
            options.verbosity += 2;
        } else if (name == "-q" || name == "--quiet") {
            options.verbosity = -1;
        } else if (name == "--json") {
            options.json = true;
        } else if (name == "--force" || name == "-f") {
            options.force = true;
        } else if (name == "--dry-run" || name == "-n") {
            options.dry_run = true;
        } else if (name == "--all" || name == "-a") {
            options.all = true;
        } else if (name == "--keep") {
            options.keep = true;
        } else if (name == "--profile") {
            options.show_profile = true;
        } else if (name == "--no-metadata") {
            options.no_metadata = true;

        } else if (name == "--ro" || name == "--read-only") {
            for (const std::string& p : str::split(value(), ',')) {
                add_mount(options, p, Access::ReadOnly, Storage::Host);
            }
        } else if (name == "--rw" || name == "--read-write") {
            for (const std::string& p : str::split(value(), ',')) {
                add_mount(options, p, Access::ReadWrite, Storage::Host);
            }
        } else if (name == "--deny") {
            for (const std::string& p : str::split(value(), ',')) {
                add_mount(options, p, Access::Deny, Storage::Host);
            }
        } else if (name == "--cow" || name == "--clone") {
            for (const std::string& p : str::split(value(), ',')) {
                add_mount(options, p, Access::ReadWrite, Storage::Clone);
            }

        } else if (name == "--network" || name == "--net") {
            options.network = parse_network(value());
        } else if (name == "--services") {
            options.services = parse_service_bundle(value());
        } else if (name == "--service") {
            for (const std::string& s : str::split(value(), ',')) {
                if (s.empty()) throw Error("--service needs a Mach service name");
                options.extra_services.push_back(s);
            }
        } else if (name == "--toolchain") {
            options.toolchain = true;

        } else if (name == "--memory" || name == "-m") {
            options.limits.memory_bytes = str::parse_bytes(value());
        } else if (name == "--cpu") {
            options.limits.cpu_multiple = str::parse_cpu(value());
        } else if (name == "--cpu-time") {
            options.limits.cpu_seconds = str::parse_duration(value());
        } else if (name == "--processes" || name == "--max-processes") {
            options.limits.max_processes = parse_count(name, value());
        } else if (name == "--timeout" || name == "-t") {
            options.limits.wall_seconds = str::parse_duration(value());
        } else if (name == "--max-file-size") {
            options.limits.file_bytes = str::parse_bytes(value());
        } else if (name == "--max-open-files") {
            options.limits.open_files = parse_count(name, value());
        } else if (name == "--background") {
            options.limits.background_priority = true;
        } else if (name == "--core-dumps") {
            options.limits.core_dumps = true;

        } else if (name == "--env" || name == "-e") {
            parse_env_assignment(options, value());
        } else if (name == "--inherit-env") {
            options.env.inherit = true;

        } else if (name == "--user" || name == "-u") {
            options.credentials.user = value();
        } else if (name == "--allow-setuid") {
            options.credentials.block_setuid_exec = false;
        } else if (name == "--umask") {
            std::string text = value();
            char* end = nullptr;
            unsigned long parsed = std::strtoul(text.c_str(), &end, 8);
            if (!end || *end != '\0' || text.empty() || parsed > 0777) {
                throw Error("--umask needs an octal value up to 777, not '" + text + "'");
            }
            options.credentials.umask = static_cast<uint32_t>(parsed);

        } else if (name == "--ephemeral") {
            options.ephemeral = true;
        } else if (name == "--persistent") {
            options.ephemeral = false;
        } else if (name == "--name") {
            options.name = value();
        } else if (name == "--cwd" || name == "-C") {
            options.cwd = value();
        } else if (name == "--no-private-tmp") {
            options.private_tmp = false;
        } else if (name == "--no-private-home") {
            options.private_home = false;
        } else if (name == "--permissive-metadata") {
            options.permissive_metadata = true;
        } else if (name == "--snapshot") {
            options.snapshot = value();
        } else if (name == "--note") {
            options.note = value();
        } else if (name == "--signal" || name == "-s") {
            options.signal_number = signal_from_name(value());
        } else if (name == "--exclude") {
            for (const std::string& p : str::split(value(), ',')) {
                if (!p.empty()) options.excludes.push_back(p);
            }
        } else if (name == "--redirect-paths") {
            options.redirect_paths = true;
        } else if (name == "--pty") {
            options.use_pty = true;
        } else if (name == "--no-pty") {
            options.use_pty = false;
        } else if (name == "--trace-denials") {
            options.trace_denials = true;
        } else if (name == "--older-than") {
            options.older_than = str::parse_duration(value());
        } else {
            throw Error("unknown option '" + name + "'; try 'isolate " + options.command +
                        " --help'");
        }
    }

    return options;
}

Spec build_spec(const Options& options, const std::string& store_root) {
    Spec spec;
    spec.id = generate_capsule_id();
    spec.created_at = static_cast<int64_t>(std::time(nullptr));

    spec.mounts = options.mounts;

    if (options.toolchain) {
        for (const std::string& dir : developer_tool_paths()) {
            Mount mount;
            mount.host = dir;
            mount.access = Access::ReadOnly;
            mount.storage = Storage::Host;
            spec.mounts.push_back(std::move(mount));
        }
        if (!options.services.has_value()) spec.services = ServiceBundle::Development;
    }

    if (options.network.has_value()) spec.network = *options.network;
    if (options.services.has_value()) spec.services = *options.services;
    spec.extra_services = options.extra_services;
    spec.limits = options.limits;
    spec.env = options.env;
    spec.credentials = options.credentials;

    if (options.ephemeral.has_value()) spec.ephemeral = *options.ephemeral;
    if (options.private_tmp.has_value()) spec.private_tmp = *options.private_tmp;
    if (options.private_home.has_value()) spec.private_home = *options.private_home;
    if (options.permissive_metadata.has_value()) {
        spec.permissive_metadata = *options.permissive_metadata;
    }
    spec.redirect_paths = options.redirect_paths;
    if (options.name.has_value()) {
        if (!is_valid_capsule_name(*options.name)) {
            throw Error("'" + *options.name +
                        "' is not a valid capsule name; use letters, digits, '.', '_', "
                        "or '-', and do not start with '.' or '-'");
        }
        spec.name = *options.name;
    }

    spec.command = options.program;
    if (spec.command.empty()) throw Error("expected a program to run");

    // Else a capsule w/ write access to an enclosing dir could
    // edit another capsule's policy, or its own baseline, and
    // make diff lie.
    std::string store = path::resolve_lenient(store_root);
    for (const Mount& mount : spec.mounts) {
        if (path::is_within(mount.host, store) || path::is_within(store, mount.host)) {
            if (mount.access != Access::Deny) {
                throw Error("'" + mount.host + "' overlaps the capsule store at " + store +
                            "; a capsule cannot be given access to capsule storage");
            }
        }
    }

    // Named after the src dir; disambiguate so two "src" mounts
    // don't share a layer.
    std::set<std::string> used;
    int primaries = 0;
    for (Mount& mount : spec.mounts) {
        if (mount.storage != Storage::Clone) continue;

        if (!fs::is_dir(mount.host)) {
            throw Error("--cow needs a directory, but '" + mount.host + "' is " +
                        (fs::exists(mount.host) ? "not a directory" : "missing"));
        }

        std::string base = name_from_path(mount.host);
        if (base.empty()) base = "layer";
        std::string candidate = base;
        for (int n = 2; used.count(candidate) != 0; ++n) {
            candidate = base + "-" + std::to_string(n);
        }
        used.insert(candidate);
        mount.clone_name = candidate;

        // First clone becomes the cwd, so `--cow .` resolves
        // relative paths inside the clone.
        if (primaries == 0 && !options.cwd.has_value()) {
            mount.primary = true;
            ++primaries;
        }
    }

    if (options.cwd.has_value()) {
        spec.cwd = path::resolve_lenient(*options.cwd);
    }

    spec.validate();
    return spec;
}

void print_usage() {
    std::printf("isolate %s - process capsules for macOS\n\n", ISOLATE_VERSION);
    std::printf("usage: isolate <command> [options]\n\n");
    std::printf("commands:\n");
    for (const CommandHelp& c : kCommands) {
        std::printf("  %-10s %s\n", c.name, c.summary);
    }
    std::printf("\nfilesystem options for run:\n");
    std::printf("  --ro PATH          readable, not writable\n");
    std::printf("  --rw PATH          readable and writable on the host itself\n");
    std::printf("  --cow PATH         work on a private clone; the original is untouched\n");
    std::printf("  --deny PATH        no access, overriding any enclosing option\n");
    std::printf("  --toolchain        read-only access to the installed developer tools\n");
    std::printf("\nother common options:\n");
    std::printf("  --network MODE     none (default), outbound, or all\n");
    std::printf("  --services BUNDLE  none, minimal (default), development, graphical\n");
    std::printf("  --memory SIZE      stop the capsule above this total footprint\n");
    std::printf("  --processes N      cap the number of processes\n");
    std::printf("  --timeout TIME     stop the capsule after this long\n");
    std::printf("  --ephemeral        delete the capsule when the program exits\n");
    std::printf("  --name NAME        give the capsule a name to refer to it by\n");
    std::printf("  --redirect-paths   intercept host absolute paths to COW layers\n");
    std::printf("  --pty              proxy terminal through PTY to prevent TIOCSTI injection\n");
    std::printf("  --trace-denials    live-stream unified log hints for Mach service denials\n");
    std::printf("  --exclude GLOB     exclude pattern from diff and commit operations\n");
    std::printf("\nRun 'isolate <command> --help' for a command's own options.\n");
    std::printf("Paths are resolved before use, so /tmp becomes /private/tmp.\n");
}

void print_command_help(const std::string& command) {
    const CommandHelp* help = find_command(command);
    if (!help) {
        print_usage();
        return;
    }
    std::printf("%s\n\n", help->usage);
    std::printf("%s\n", help->detail);
}

} // namespace isolate::cli
