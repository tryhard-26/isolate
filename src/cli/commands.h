#pragma once

#include "cli/args.h"
#include "core/store.h"
#include "sb/profile.h"

namespace isolate::cli {

// Return the process exit status; throw for user-visible errors.
int cmd_run(const Options& options, Store& store);
int cmd_enter(const Options& options, Store& store);
int cmd_list(const Options& options, Store& store);
int cmd_inspect(const Options& options, Store& store);
int cmd_ps(const Options& options, Store& store);
int cmd_diff(const Options& options, Store& store);
int cmd_commit(const Options& options, Store& store);
int cmd_snapshot(const Options& options, Store& store);
int cmd_kill(const Options& options, Store& store);
int cmd_destroy(const Options& options, Store& store);
int cmd_doctor(const Options& options, Store& store);
int cmd_gc(const Options& options, Store& store);
int cmd_export(const Options& options, Store& store);
int cmd_import(const Options& options, Store& store);

// Shared by run/enter/inspect, so inspect shows what run applies.
sb::ProfileInputs profile_inputs_for(const Capsule& capsule, const std::string& program_path,
                                     const std::string& tty_path, bool log_denials);

// Throws naming what was expected.
const std::string& require_target(const Options& options, const char* what);

} // namespace isolate::cli
