#include "cli/commands.h"

#include "cli/format.h"
#include "core/ids.h"
#include "fsview/layer.h"
#include "proc/launch.h"
#include "proc/limits.h"
#include "proc/pty.h"
#include "proc/supervisor.h"
#include "sb/apply.h"
#include "sb/tag.h"
#include "util/error.h"
#include "util/fs.h"
#include "util/log.h"
#include "util/path.h"
#include "util/str.h"

#include <unistd.h>

#include <cstdio>
#include <ctime>

namespace isolate::cli {
namespace {

std::string current_tty() {
    // Only the terminal on standard input is granted, and only when there is
    // one. A capsule started from a script gets no terminal access at all.
    if (::isatty(STDIN_FILENO) != 1) return {};
    const char* name = ::ttyname(STDIN_FILENO);
    if (!name) return {};
    return path::resolve_lenient(name);
}

// sun_path is 104 bytes on Darwin, and the capsule's scratch dir
// is where a prog puts its socket. A deep store makes sockets
// fail w/ ENAMETOOLONG inside a capsule and nowhere else.
void warn_if_scratch_path_is_long(const Capsule& capsule) {
    if (!capsule.spec().private_tmp) return;

    constexpr size_t kSunPathLimit = 104;
    // Room for a plausible socket name.
    constexpr size_t kReservedForSocketName = 24;

    size_t length = capsule.tmp_dir().size();
    if (length + kReservedForSocketName < kSunPathLimit) return;

    log::warn("the capsule's scratch directory is " + std::to_string(length) +
              " characters long, and a Unix domain socket path cannot exceed " +
              std::to_string(kSunPathLimit) +
              ". A program that creates a socket under TMPDIR may fail with "
              "'File name too long'. Set ISOLATE_HOME to a shorter path to avoid it.");
}

void announce_capsule(const Capsule& capsule, const std::string& program) {
    const Spec& spec = capsule.spec();
    std::string line = "capsule " + spec.id;
    if (!spec.name.empty()) line += " (" + spec.name + ")";
    if (spec.ephemeral) line += ", ephemeral";
    log::verbose(line);
    log::verbose("program: " + program);
    log::verbose("network: " + std::string(to_string(spec.network)) +
                 ", services: " + to_string(spec.services));
    for (const Mount& mount : spec.mounts) {
        std::string detail = std::string(to_string(mount.access));
        if (mount.storage == Storage::Clone) detail += " clone at " + mount.guest;
        log::verbose("  " + mount.host + ": " + detail);
    }
    for (const proc::LimitNote& note : proc::describe(spec.limits)) {
        log::verbose("  limit " + note.name + " = " + note.value +
                     (note.kernel_enforced ? " (kernel)" : " (supervisor)"));
    }
}

void record_outcome(Capsule& capsule, const proc::RunReport& report) {
    State& state = capsule.mutable_state();
    state.status = Status::Stopped;
    state.supervisor_pid = 0;
    state.finished_at = static_cast<int64_t>(std::time(nullptr));
    state.last_exit_code = report.exit_code;

    std::string termination = proc::to_string(report.termination);
    if (report.termination == proc::Termination::Signalled && report.signal_number > 0) {
        termination += " (" + str::signal_name(report.signal_number) + ")";
    }
    state.last_termination = termination;
    capsule.save_state();
}

void report_summary(const proc::RunReport& report, const Capsule& capsule) {
    if (log::level() < log::Level::Verbose && report.termination == proc::Termination::Exited &&
        report.leftover_processes == 0 && !report.attestation_degraded) {
        return; // clean run says nothing
    }

    std::string line = "capsule " + capsule.label() + " " +
                       proc::to_string(report.termination);
    if (report.termination == proc::Termination::Exited) {
        line += " with status " + std::to_string(report.exit_code);
    } else if (report.termination == proc::Termination::Signalled) {
        line += " on " + str::signal_name(report.signal_number);
    }
    line += " after " + str::human_duration(report.wall_seconds);
    if (report.peak_footprint > 0) {
        line += ", peak " + str::human_bytes(report.peak_footprint);
    }
    if (report.peak_process_count > 1) {
        line += ", " + std::to_string(report.peak_process_count) + " processes";
    }
    log::info(line);

    if (report.memory_overshoot > 0) {
        log::warn("the capsule was " + str::human_bytes(report.memory_overshoot) +
                  " over its ceiling by the time isolate saw it. Without root, macOS has "
                  "no per-process memory limit isolate can set, so the ceiling is enforced "
                  "by sampling and a fast allocation can overshoot it.");
    }
    if (report.forced_kill) {
        log::warn("some capsule processes had to be killed; they did not exit on SIGTERM");
    }
    if (report.attestation_degraded) {
        log::warn("the kernel did not confirm capsule membership during this run, so the "
                  "process list rests on parentage alone; run 'isolate doctor'");
    }
}

} // namespace

sb::ProfileInputs profile_inputs_for(const Capsule& capsule, const std::string& program_path,
                                     const std::string& tty_path, bool log_denials) {
    sb::ProfileInputs inputs;
    inputs.spec = &capsule.spec();
    inputs.capsule_dir = capsule.dir();
    inputs.capsule_tmp = capsule.spec().private_tmp ? capsule.tmp_dir() : "";
    inputs.capsule_home = capsule.spec().private_home ? capsule.home_dir() : "";
    inputs.tty_path = tty_path;
    inputs.program_path = program_path;
    if (capsule.spec().redirect_paths) {
        inputs.shim_path = proc::find_shim_library();
    }
    inputs.membership_name = sb::membership_name(capsule.spec().id);
    inputs.log_denials = log_denials;
    return inputs;
}

const std::string& require_target(const Options& options, const char* what) {
    if (options.targets.empty()) throw Error(std::string("expected ") + what);
    return options.targets.front();
}

int cmd_run(const Options& options, Store& store) {
    if (sb::self_is_sandboxed()) {
        throw Error("isolate is itself running inside a sandbox. A capsule created from "
                    "here would be confined by both profiles at once, which is not the "
                    "policy you asked for, so isolate refuses rather than quietly "
                    "applying something stricter.");
    }

    Spec spec = build_spec(options, store.root());

    // Before the capsule exists, so a typo is a typo rather than
    // an exec failure in a capsule we then have to clean up.
    std::vector<std::string> env_preview;
    for (const auto& [name, value] : spec.env.set) env_preview.push_back(name + "=" + value);
    std::string program = proc::resolve_program(spec.command.front(), env_preview);

    std::unique_ptr<Capsule> capsule = store.create(spec);

    // Every exit path, incl. failure and interrupt.
    struct Cleanup {
        Store& store;
        Capsule* capsule;
        bool armed;
        ~Cleanup() {
            if (!armed || !capsule) return;
            try {
                store.destroy(*capsule);
            } catch (const Error& e) {
                log::warn(std::string("could not remove the ephemeral capsule: ") + e.what());
            }
        }
    } cleanup{store, capsule.get(), spec.ephemeral && !options.keep};

    fsview::PrepareResult prepared = fsview::prepare_layers(*capsule);
    if (prepared.layers_created > 0) {
        log::verbose("prepared " + std::to_string(prepared.layers_created) + " layer" +
                     (prepared.layers_created == 1 ? "" : "s") + " using " +
                     str::human_bytes(prepared.bytes_on_disk) + " of disk");
    }
    if (prepared.any_copied) {
        log::warn("at least one layer had to be copied rather than cloned, because the "
                  "source is not on an APFS volume that supports cloning. This costs full "
                  "disk space and time proportional to the data.");
    }

    // Primary clone, so relative paths hit the capsule's copy.
    Spec& live = capsule->mutable_spec();
    if (live.cwd.empty()) {
        if (const Mount* primary = live.primary_mount()) {
            live.cwd = primary->guest;
        } else if (live.private_tmp) {
            live.cwd = capsule->tmp_dir();
        } else {
            live.cwd = "/";
        }
    }
    live.command.front() = program;
    capsule->save_spec();

    if (!live.credentials.user.empty()) {
        store.open_traversal();
        capsule->grant_to_user(live.credentials.user);
    }

    announce_capsule(*capsule, program);
    warn_if_scratch_path_is_long(*capsule);

    bool use_pty = options.use_pty.value_or(::isatty(STDIN_FILENO) == 1);
    std::unique_ptr<proc::PtySession> pty;
    std::string tty_for_profile = current_tty();
    if (use_pty) {
        pty = std::make_unique<proc::PtySession>();
        if (pty->open()) {
            tty_for_profile = pty->slave_name();
        } else {
            pty.reset();
        }
    }

    std::string profile = sb::build_profile(profile_inputs_for(
        *capsule, program, tty_for_profile, log::level() >= log::Level::Verbose));
    log::debug("sandbox profile is " + std::to_string(profile.size()) + " bytes");

    if (log::level() >= log::Level::Debug) {
        fs::write_file_atomic(path::join(capsule->dir(), "profile.sb"), profile, 0600);
        log::debug("profile written to " + path::join(capsule->dir(), "profile.sb"));
    }

    sb::ValidationResult validation = sb::validate_profile(profile);
    if (!validation.ok) {
        throw Error("the generated sandbox profile was rejected by the kernel: " +
                    validation.error +
                    "\nThis is a bug in isolate. Re-run with --debug to keep a copy of the "
                    "profile for a report.");
    }

    State& state = capsule->mutable_state();
    state.status = Status::Running;
    state.supervisor_pid = ::getpid();
    state.started_at = static_cast<int64_t>(std::time(nullptr));
    ++state.run_count;
    capsule->save_state();

    proc::SupervisorOptions supervisor_options;
    supervisor_options.announce = log::level() > log::Level::Quiet;
    supervisor_options.trace_denials = options.trace_denials;
    proc::Supervisor supervisor(*capsule, supervisor_options);

    proc::RunReport report = supervisor.run(live.command, profile, pty.get());
    record_outcome(*capsule, report);
    report_summary(report, *capsule);

    // Once, after the first run.
    if (!spec.ephemeral && state.run_count == 1 && log::level() > log::Level::Quiet) {
        std::string reference = spec.name.empty() ? spec.id : spec.name;
        log::info("capsule kept as " + reference + "; 'isolate diff " + reference +
                  "' shows what changed, 'isolate destroy " + reference + "' removes it");
    }

    return report.exit_status_for_isolate();
}

int cmd_enter(const Options& options, Store& store) {
    if (sb::self_is_sandboxed()) {
        throw Error("isolate is itself running inside a sandbox; it cannot enter a capsule "
                    "from here");
    }

    const std::string& reference = require_target(options, "a capsule to enter");

    // Exclusive, because entering a capsule writes to its layers and its state.
    std::unique_ptr<Capsule> capsule = store.open(reference, true);

    // Accepting policy here would let a later run widen what the
    // capsule may reach, and inspect would stop describing it.
    if (!options.mounts.empty() || options.network.has_value() ||
        options.services.has_value() || !options.extra_services.empty() ||
        options.toolchain || options.permissive_metadata.has_value()) {
        throw Error("a capsule's policy is fixed when it is created, so 'enter' does not "
                    "take filesystem, network, or service options. Use 'isolate inspect " +
                    reference + "' to see the policy, or create a new capsule.");
    }

    Spec& spec = capsule->mutable_spec();
    fsview::prepare_layers(*capsule);
    if (!spec.credentials.user.empty()) {
        store.open_traversal();
        capsule->grant_to_user(spec.credentials.user);
    }

    std::vector<std::string> argv = options.program;
    if (argv.empty()) {
        // Useful default for re-entering a dev capsule.
        argv = {"/bin/sh"};
        if (::isatty(STDIN_FILENO) == 1) argv.push_back("-i");
    }

    std::vector<std::string> env_preview;
    for (const auto& [name, value] : spec.env.set) env_preview.push_back(name + "=" + value);
    std::string program = proc::resolve_program(argv.front(), env_preview);
    argv.front() = program;

    if (spec.cwd.empty()) {
        if (const Mount* primary = spec.primary_mount()) spec.cwd = primary->guest;
        else if (spec.private_tmp) spec.cwd = capsule->tmp_dir();
        else spec.cwd = "/";
    }

    // Per-run only; recorded policy is left alone.
    Spec run_spec = spec;
    if (options.limits.any()) {
        Limits merged = spec.limits;
        if (options.limits.memory_bytes.has_value()) {
            merged.memory_bytes = options.limits.memory_bytes;
        }
        if (options.limits.max_processes.has_value()) {
            merged.max_processes = options.limits.max_processes;
        }
        if (options.limits.wall_seconds.has_value()) {
            merged.wall_seconds = options.limits.wall_seconds;
        }
        if (options.limits.cpu_seconds.has_value()) {
            merged.cpu_seconds = options.limits.cpu_seconds;
        }
        if (options.limits.cpu_multiple.has_value()) {
            merged.cpu_multiple = options.limits.cpu_multiple;
        }
        if (options.limits.background_priority) merged.background_priority = true;
        run_spec.limits = merged;
    }
    run_spec.command = argv;

    announce_capsule(*capsule, program);

    bool use_pty = options.use_pty.value_or(::isatty(STDIN_FILENO) == 1);
    std::unique_ptr<proc::PtySession> pty;
    std::string tty_for_profile = current_tty();
    if (use_pty) {
        pty = std::make_unique<proc::PtySession>();
        if (pty->open()) {
            tty_for_profile = pty->slave_name();
        } else {
            pty.reset();
        }
    }

    // From the run spec, so per-run limits apply, but the stored
    // policy is unchanged.
    Capsule& capsule_ref = *capsule;
    Spec saved = capsule_ref.spec();
    if (options.redirect_paths) {
        run_spec.redirect_paths = true;
    }
    capsule_ref.mutable_spec() = run_spec;
    std::string profile = sb::build_profile(profile_inputs_for(
        capsule_ref, program, tty_for_profile, log::level() >= log::Level::Verbose));

    sb::ValidationResult validation = sb::validate_profile(profile);
    if (!validation.ok) {
        capsule_ref.mutable_spec() = saved;
        throw Error("the generated sandbox profile was rejected by the kernel: " +
                    validation.error);
    }

    State& state = capsule_ref.mutable_state();
    state.status = Status::Running;
    state.supervisor_pid = ::getpid();
    state.started_at = static_cast<int64_t>(std::time(nullptr));
    ++state.run_count;
    capsule_ref.save_state();

    proc::SupervisorOptions supervisor_options;
    supervisor_options.announce = log::level() > log::Level::Quiet;
    supervisor_options.trace_denials = options.trace_denials;
    proc::Supervisor supervisor(capsule_ref, supervisor_options);
    proc::RunReport report = supervisor.run(argv, profile, pty.get());

    capsule_ref.mutable_spec() = saved;
    capsule_ref.mutable_spec().cwd = run_spec.cwd;
    capsule_ref.save_spec();
    record_outcome(capsule_ref, report);
    report_summary(report, capsule_ref);

    return report.exit_status_for_isolate();
}

} // namespace isolate::cli
