#include "cli/args.h"
#include "cli/commands.h"
#include "core/store.h"
#include "util/error.h"
#include "util/log.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

isolate::log::Level level_from_verbosity(int verbosity) {
    using isolate::log::Level;
    if (verbosity < 0) return Level::Quiet;
    if (verbosity == 0) return Level::Normal;
    if (verbosity == 1) return Level::Verbose;
    return Level::Debug;
}

} // namespace

int main(int argc, char** argv) {
    using namespace isolate;

    try {
        cli::Options options = cli::parse(argc, argv);

        if (options.version) {
            std::printf("isolate %s\n", ISOLATE_VERSION);
            return 0;
        }
        if (options.help) {
            if (options.command.empty()) cli::print_usage();
            else cli::print_command_help(options.command);
            return 0;
        }

        log::set_level(level_from_verbosity(options.verbosity));

        Store store;

        const std::string& command = options.command;
        if (command == "run") return cli::cmd_run(options, store);
        if (command == "enter") return cli::cmd_enter(options, store);
        if (command == "ls" || command == "list") return cli::cmd_list(options, store);
        if (command == "inspect" || command == "show") {
            return cli::cmd_inspect(options, store);
        }
        if (command == "ps") return cli::cmd_ps(options, store);
        if (command == "diff") return cli::cmd_diff(options, store);
        if (command == "commit") return cli::cmd_commit(options, store);
        if (command == "snapshot" || command == "snap") {
            return cli::cmd_snapshot(options, store);
        }
        if (command == "kill" || command == "stop") return cli::cmd_kill(options, store);
        if (command == "destroy" || command == "rm") return cli::cmd_destroy(options, store);
        if (command == "doctor") return cli::cmd_doctor(options, store);
        if (command == "gc" || command == "prune") return cli::cmd_gc(options, store);
        if (command == "export") return cli::cmd_export(options, store);
        if (command == "import") return cli::cmd_import(options, store);

        std::fprintf(stderr, "isolate: unknown command '%s'\n", command.c_str());
        std::fprintf(stderr, "Try 'isolate --help' for the list of commands.\n");
        return 2;

    } catch (const Unsupported& e) {
        std::fprintf(stderr, "isolate: %s\n", e.what());
        return 3;
    } catch (const Error& e) {
        std::fprintf(stderr, "isolate: %s\n", e.what());
        return 1;
    } catch (const std::exception& e) {
        // A bug, not a user error.
        std::fprintf(stderr, "isolate: internal error: %s\n", e.what());
        return 70;
    }
}
