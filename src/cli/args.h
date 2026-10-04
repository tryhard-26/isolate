#pragma once

#include "core/spec.h"

#include <optional>
#include <string>
#include <vector>

namespace isolate::cli {

// Own parser, not getopt: run has to stop at the program name
// and pass the rest through untouched, incl. args that look like
// isolate's own options.
struct Options {
    std::string command;              // "run", "ls", "diff", ...
    std::vector<std::string> targets; // positional arguments for the command

    // In argv order; the profile sorts by depth, not by position.
    std::vector<Mount> mounts;
    std::optional<Network> network;
    std::optional<ServiceBundle> services;
    std::vector<std::string> extra_services;
    Limits limits;
    Environment env;
    Credentials credentials;

    std::optional<bool> ephemeral;
    std::optional<bool> private_tmp;
    std::optional<bool> private_home;
    std::optional<bool> permissive_metadata;
    std::optional<std::string> name;
    std::optional<std::string> cwd;
    bool toolchain = false;

    // Everything after the first non-option.
    std::vector<std::string> program;

    bool json = false;
    bool force = false;
    bool dry_run = false;
    bool all = false;
    bool keep = false;          // run: keep an ephemeral capsule after it exits
    bool show_profile = false;  // inspect: print the generated sandbox profile
    bool no_metadata = false;   // diff: hide metadata-only changes
    std::vector<std::string> excludes; // diff, commit: exclude patterns
    bool redirect_paths = false; // run, enter: path virtualization shim
    std::optional<bool> use_pty; // run, enter: pty proxy
    bool trace_denials = false;  // run, enter: mach denial tracing
    std::optional<double> older_than; // gc: prune older than threshold
    std::optional<std::string> snapshot;
    std::optional<std::string> note;
    std::optional<int> signal_number;

    int verbosity = 0; // -v counts up, -q sets it to -1
    bool help = false;
    bool version = false;
};

// Throws w/ a usage hint on anything malformed.
Options parse(int argc, char** argv);

// Resolves paths, names clones, validates. Needs store_root to
// refuse mounting capsule storage into a capsule.
Spec build_spec(const Options& options, const std::string& store_root);

void print_usage();
void print_command_help(const std::string& command);

} // namespace isolate::cli
