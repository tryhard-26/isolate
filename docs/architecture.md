# Technical Architecture of isolate

This document describes the internal engineering mechanics, operating system interfaces, and security model underlying isolate.

## Overview

Isolate is built directly on native macOS subsystems: the Apple Sandbox (Seatbelt) kernel extension, the Apple File System (APFS), the POSIX process management APIs, the kernel event notification facility (`kqueue`), and the Darwin Unified Logging architecture. Isolate does not use virtual machines, hypervisors, or Linux compatibility layers. Every sandboxed workload runs as a native macOS Mach-O process subject to kernel-level access mediation.

## Kernel Sandboxing with Seatbelt

The core isolation mechanism on macOS is the Seatbelt sandbox (`com.apple.security.sandbox`). When a program runs inside a capsule, isolate compiles a declarative Sandbox Profile Language (SBPL) profile tailored specifically to the capsule specification and hands it to the kernel via `sandbox_init_with_parameters`.

### Sandbox Rule Precedence and Operation Expansion

Apple's Sandbox Profile Language does not follow a simplistic last-matching-rule evaluation order. Instead, the kernel prioritizes rules matching narrower operation tokens over rules that specify broader umbrella tokens. Rule ordering in the profile only acts as a tie-breaker between rules of identical operation specificity.

For example, consider a profile that contains the following rules:

```scheme
(allow file-read* (subpath "/Users/example/project"))
(deny  file*      (subpath "/Users/example/project/secret"))
```

In this scenario, the kernel permits file reads inside `/Users/example/project/secret` despite the subsequent denial rule, because `file-read*` is a more specific operation filter than `file*`.

To guarantee deterministic, predictable policy enforcement where explicit denials consistently override broader grants, isolate expands all path rules into identical sets of explicit operation tokens. These include `file-read-data`, `file-write-data`, `file-read-metadata`, `file-write-metadata`, `file-test-existence`, `file-map-executable`, and `process-exec*`. Furthermore, isolate sorts path rules from shallowest directory depth to deepest directory depth. This structure ensures that nested path denials retain equal token specificity with enclosing allowances and take final precedence in kernel evaluation.

### Safe Spawn Sequencing

Applying a sandbox profile to a process is an irreversible one-way transition. Once a profile is applied, privileges cannot be widened, and operations required to initialize runtime environments or apply resource limits become unavailable.

To avoid race conditions and ensure that untrusted code never executes without full policy enforcement, isolate executes the following sequence:

1. The supervisor process creates a child process via `fork()`.
2. The child process establishes a new session via `setsid()` to decouple its process group from the terminal.
3. The child resets signal handlers to their default actions and clears inherited signal masks.
4. If a working directory was requested, the child changes directory via `chdir()`.
5. If privilege dropping was configured (`--user`), user and group credentials are applied via `initgroups()`, `setgid()`, and `setuid()`.
6. POSIX resource limits (`rlimits`) are applied via `setrlimit()`.
7. The compiled sandbox profile is applied to the child process via `sandbox_init_with_parameters()`.
8. The child invokes `posix_spawn()` with the `POSIX_SPAWN_SETEXEC` flag, passing the target program path, arguments, environment array, and a Quality-of-Service scheduling clamp attribute (`posix_spawnattr_set_qos_clamp_np`).

Because `POSIX_SPAWN_SETEXEC` replaces the calling child process with the target executable, the executable image begins running with the sandbox profile and scheduling clamps already enforced at the very first instruction. Any failure during preparation is communicated back to the supervisor through a close-on-exec status pipe before execution commences.

## Process Lifecycle and Unified Event Loop

The supervisor process monitors the running capsule using a single Darwin kernel event queue (`kqueue`). This unified event architecture eliminates polling loops, avoids signal handler race conditions, and coordinates multiple disparate asynchronous events:

- `EVFILT_PROC` with `NOTE_EXIT`: Triggers immediate notification when the initial root process of the capsule terminates.
- `EVFILT_SIGNAL`: Captures forwarded POSIX signals (such as `SIGINT`, `SIGTERM`, `SIGHUP`, `SIGQUIT`, and `SIGWINCH`) synchronously as queue events rather than executing asynchronous signal handler functions.
- `EVFILT_TIMER`: Executes periodic ticks to sample memory footprint and process count ceilings, as well as wall-clock execution deadlines.
- `EVFILT_READ`: Polls file descriptors for terminal proxy input and output without blocking.

When the primary workload process exits, the supervisor enters a cleanup sequence. It scans for any leftover processes belonging to the capsule that were reparented to `launchd`, delivers `SIGTERM`, waits for a configurable grace period, and delivers `SIGKILL` to any surviving processes before reaping zombie entries.

## Process Membership Attestation

Unlike Linux, macOS does not provide container PID namespaces. Any process on macOS can detach from its original process hierarchy by calling `setsid()` or `setpgid()`, causing orphaned child processes to be adopted directly by the root initialization daemon (`launchd`, PID 1). Under conventional process tracking, an inspection tool relying solely on parent process identifiers would lose visibility into detached background processes.

To solve this challenge without requiring kernel privileges or third-party kernel extensions, isolate embeds a unique, unforgeable synthetic Mach service lookup rule into every generated sandbox profile:

```scheme
(allow mach-lookup (global-name "dev.isolate.capsule.<id>"))
```

No actual system service is registered under this identifier name, so lookups by the capsule never connect to a daemon. However, because sandboxed processes inherit their sandbox profile across all future `fork()` and `exec()` calls, every descendant process carries this exact permission.

From outside the sandbox, the supervisor inspects candidate processes in the system process table and invokes the Darwin private system call `sandbox_check(pid, "mach-lookup", SANDBOX_CHECK_NO_REPORT, name)`. If the target process possesses permission to query that specific identifier, the kernel returns 0, definitively confirming membership in the capsule. Because the sandbox profile cannot be modified or replaced from within the capsule, this attestation mechanism cannot be forged or evaded by the untrusted workload.

## APFS Copy-on-Write and Extent Sharing

Capsule workspace storage relies on features specific to the Apple File System (APFS).

### Instantaneous Block Sharing (`clonefile`)

When `--cow` is used, isolate duplicates the specified host directory using the `clonefile(2)` system call. Rather than copying bytes across storage media, APFS creates new file system metadata records that point directly to the existing physical disk extents of the source files. This cloning operation completes in milliseconds regardless of the directory size and consumes no additional physical storage at creation.

When processes inside the capsule modify a file, APFS allocates new physical blocks only for the modified regions, leaving the host original unmodified and preserving shared blocks for all untouched regions.

### Zero-IO Extent Verification (`F_LOG2PHYS_EXT`)

When computing differences between a capsule layer and the host baseline (`isolate diff`), reading gigabytes of data from disk to calculate checksums would impose significant I/O overhead.

Isolate inspects the physical disk extent allocations of each file by issuing the `fcntl` command `F_LOG2PHYS_EXT`. If every logical block of a file in the capsule layer maps to the identical physical device offset as the baseline file, isolate mathematically proves that the file has not been altered without reading a single byte from the storage medium. If extent sharing is broken (for example, if an editor wrote to a file and rewrote identical content), isolate falls back to a byte-by-byte comparison to verify whether the contents actually differ.

## Terminal Keystroke Injection Protection (PTY Proxy)

When a sandboxed program runs in an interactive terminal session, granting it direct write access to the host controlling terminal device (`/dev/ttys*`) creates a significant security escape vulnerability.

On standard POSIX and Darwin systems, any process with write access to its controlling terminal can issue `ioctl(fd, TIOCSTI, &byte)` to inject arbitrary keystrokes into the input queue of the terminal. A malicious sandboxed program could queue shell commands into the terminal buffer while running; once isolate exits and returns control to the user's host shell, the queued commands would execute directly in the host user's unsandboxed session.

To completely neutralize this attack vector, isolate implements an internal pseudoterminal proxy:

1. The supervisor allocates a fresh pseudoterminal master and slave pair using `openpty(3)`.
2. The supervisor captures the host terminal dimensions (`TIOCGWINSZ`) and applies them to the allocated slave terminal.
3. The supervisor configures the host standard input descriptor into raw mode using `cfmakeraw` and `tcsetattr(TCSANOW)`.
4. The generated sandbox profile grants access only to the secondary slave terminal device (for example, `/dev/ttys008`), while access to the host terminal device is strictly denied.
5. The child process receives the secondary slave file descriptor duped to standard input, standard output, and standard error, and acquires it as its controlling terminal via `ioctl(STDIN_FILENO, TIOCSCTTY, 0)`.
6. During execution, the supervisor's `kqueue` loop pumps bytes between the host terminal and the master pseudoterminal descriptor.

If a sandboxed process attempts to call `ioctl(TIOCSTI)` on its terminal descriptor, the injected keystrokes are placed exclusively into the internal slave buffer. They never reach the host terminal buffer, preventing any post-execution escape. When the child terminates, the supervisor drains remaining buffered output, restores original terminal settings, and closes the pseudoterminal pair.

## Path Virtualization via Preload Interposition

Because macOS does not provide unprivileged mount namespaces or chroot environments, copy-on-write mounts cannot be overlaid at the identical host path without root privileges. By default, a `--cow ~/project` directory is presented to the capsule at its private layer directory inside `~/.local/share/isolate/capsules/<id>/layers/<name>/`.

While relative paths function normally because the layer directory is set as the working directory, build scripts or compilers that reference hardcoded absolute host paths would normally encounter an access denial.

To resolve this limitation, isolate provides an optional path virtualization shim: `libisolate_shim.dylib`. When `--redirect-paths` is specified, isolate configures the capsule environment with:

- `DYLD_INSERT_LIBRARIES`: Points to `libisolate_shim.dylib`.
- `DYLD_FORCE_FLAT_NAMESPACE`: Set to 1 to ensure symbol interposition binds globally.
- `ISOLATE_REDIRECT_MAP`: Contains source and destination prefix mappings formatted as `host_path=guest_path:...`.

The shim uses macOS dynamic linker function interposition (`__DATA,__interpose`) to intercept file system entry points in `libSystem.dylib`:

- `open` and `openat`
- `stat` and `lstat`
- `access` and `faccessat`
- `unlink` and `unlinkat`
- `rename` and `renameat`
- `opendir`
- `chdir`
- `execve`

When any intercepted function receives a path starting with a mapped host directory, the path prefix is rewritten to the corresponding capsule layer directory before being passed to the operating system kernel.

### System Integrity Protection (SIP) Considerations

On macOS, System Integrity Protection (SIP) automatically sanitizes dynamic linker environment variables such as `DYLD_INSERT_LIBRARIES` when launching Apple-signed binaries located in `/bin`, `/sbin`, `/usr/bin`, and `/usr/sbin`.

Consequently, path virtualization applies to third-party tools, compilers, interpreters, and developer binaries (such as Homebrew tools, node, python, rustc, and custom build scripts). When Apple-signed system tools run under `--redirect-paths`, path redirection does not occur within the Apple binary itself. Where Apple system tools require access to project files, passing paths as relative arguments allows the tools to resolve files inside the working directory.

## Automated Mach Service Denial Tracing

When running third-party tools under restricted service bundles (`--services minimal`), a program may fail because it requires a specific Mach service that was not included in the bundle. Traditionally, diagnosing these failures required developers to manually execute and filter system log streams.

When `--trace-denials` is enabled, the supervisor launches a background helper that subscribes to the macOS Unified Logging system:

```sh
/usr/bin/log stream --style ndjson --predicate 'subsystem == "com.apple.sandbox" AND eventMessage CONTAINS "mach-lookup"'
```

The tracer processes log messages asynchronously, identifies sandbox denial events matching Mach service lookups, extracts the requested global Mach service name, deduplicates identical notices, and outputs an actionable suggestion to standard error:

```
isolate: notice: program requested Mach service 'com.apple.coreservices.launchservicesd'; retry with '--service com.apple.coreservices.launchservicesd'
```

This discovery mechanism enables developers to systematically identify the minimal set of Mach services required by their specific workloads.

## On-Disk Storage Organization

Capsule metadata and working trees reside in the capsule store located at `~/.local/share/isolate/capsules/<id>/` (or `$ISOLATE_HOME/capsules/<id>/`):

```
~/.local/share/isolate/capsules/<id>/
├── capsule.json          JSON specification defining mounts, services, network, and limits
├── state.json            Execution lifecycle status, run counts, and termination details
├── lock                  Advisory flock descriptor held during active execution
├── layers/<name>/        Writable working layer for each cloned mount
├── base/<name>/          Original baseline clone used for extent and byte comparisons
├── snapshots/<name>/     APFS cloned snapshots of layers and state metadata
├── home/                 Private HOME directory isolated from host user profile
└── tmp/                  Private TMPDIR directory for Unix domain sockets and scratch files
```

Every capsule directory is restricted to file mode `0700` to prevent access from other local users. Advisory file locking (`flock(LOCK_EX | LOCK_NB)`) prevents concurrent execution or premature deletion while a capsule is running.
