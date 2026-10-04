# isolate

Isolate is a command-line tool for macOS that executes programs inside restricted execution environments called capsules. The program runs as a native macOS process without virtual machines or Linux emulation layers, enforcing fine-grained kernel sandbox policies, closed Mach service namespaces, network access controls, and resource limits.

```sh
isolate run --cow ~/project -- /bin/sh -c './configure && make'
```

When you pass `--cow`, isolate creates an APFS copy-on-write clone of the target directory and executes the build against the clone. The process cannot access any host paths or services that have not been explicitly granted. After execution finishes, `isolate diff` displays all filesystem changes made by the program, and `isolate commit` selectively applies those changes back to the original host directory.

Isolate is built directly on native macOS primitives: the kernel sandbox (Seatbelt), copy-on-write cloning (`clonefile`), block extent queries (`F_LOG2PHYS_EXT`), `sandbox_check`, `kqueue` process supervision, POSIX resource limits (`setrlimit`), and Quality-of-Service scheduling clamps (`posix_spawnattr_set_qos_clamp_np`).

## Quick Start

You can run an isolated command against a project clone and inspect its results with three commands:

```sh
# Run a build inside a temporary clone of your project
isolate run --cow ~/project -- make

# Inspect what files were modified, created, or deleted
isolate diff ~/project

# Apply the changes back to your original project directory
isolate commit ~/project
```

## Command Reference

| Command | Description |
|---|---|
| `run [options] <prog> [args...]` | Creates a new capsule, executes the specified program, and monitors all spawned processes until termination. |
| `enter <capsule> [prog] [args...]` | Executes another program inside an existing capsule, reusing its established filesystem and sandbox policies. |
| `ls [--all] [--json]` | Lists existing capsules, including identifier, status, allocated disk space, and creation time. |
| `inspect <capsule>` | Displays the complete policy for a capsule, identifying which constraints are enforced by the kernel and which are managed by the supervisor. |
| `inspect --profile <capsule>` | Prints the exact Scheme sandbox profile generated for the capsule, including descriptive comments. |
| `ps <capsule>` | Lists all active processes confirmed by the kernel to belong to the capsule. |
| `diff <capsule> [options]` | Inspects and displays all file modifications, additions, deletions, and metadata changes made within cloned mounts. |
| `commit <capsule> [options]` | Applies filesystem changes from cloned mounts back to the original host paths with conflict detection. |
| `snapshot <create\|ls\|restore\|rm>` | Captures, lists, reverts, or removes lightweight point-in-time states of a capsule. |
| `kill <capsule> [pid]` | Delivers a signal to all processes within a capsule, or to a single confirmed process. |
| `destroy <capsule>...` | Removes capsule metadata and layers from disk without altering original host files. |
| `gc [options]` | Prunes abandoned ephemeral capsules and dead stopped capsules from disk to reclaim storage space. |
| `export <capsule> [archive]` | Packages capsule specification, layers, and snapshots into a compressed archive. |
| `import <archive>` | Restores a capsule archive into the local store with a fresh identifier. |
| `doctor` | Probes host kernel capabilities and validates runtime assumptions. |

All commands accept the `--json` flag for structured machine-readable output. You can view detailed options for any command by running `isolate <command> --help`.

## Filesystem Configuration

Isolate provides several flags to configure access to host directories:

```
--ro PATH          Grants read-only access to a host directory or file.
--rw PATH          Grants read and write access directly to the host filesystem.
--cow PATH         Creates an APFS clone for isolated workspace access; the host original remains untouched.
--deny PATH        Explicitly denies all access, overriding any enclosing read or write options.
--toolchain        Grants read-only access to the installed Apple developer tools (Xcode or Command Line Tools).
```

Filesystem options can be nested freely. Explicit denials always take precedence over broader permissions:

```sh
isolate run --rw ~/project --deny ~/project/.env -- ./build.sh
```

When no filesystem options are specified, a capsule can read standard system libraries and base command-line tools, but can write only to its own private temporary directory (`TMPDIR`) and private home directory (`HOME`). Access to `/Users` is denied by default.

All paths are resolved to their canonical paths before sandbox rules are compiled. Because the macOS sandbox engine matches operations against resolved paths, a rule targeting a symlink path like `/tmp` would fail to match; isolate resolves this to `/private/tmp` automatically.

### Copy-on-Write Mounts (`--cow`)

The `--cow` option clones the specified directory using APFS block-level cloning (`clonefile`). Unmodified files share physical disk blocks with the original host files, meaning large repositories can be cloned rapidly with negligible initial storage consumption.

Because unprivileged macOS processes cannot create custom filesystem mount namespaces, the capsule accesses the clone at a dedicated guest path inside the capsule store, while the host original is denied. The first `--cow` mount specified on the command line becomes the initial working directory of the capsule. Relative paths function as expected:

```sh
cd ~/project
isolate run --cow . -- make
```

## Path Virtualization and Host Redirection (`--redirect-paths`)

Because macOS does not grant unprivileged mount namespaces or chroot, copy-on-write working layers reside in dedicated paths within `~/.local/share/isolate/capsules/<id>/layers/<name>/`. Build scripts or compilers with hardcoded absolute paths to the host project directory would normally fail when accessing those paths.

To resolve this limitation without requiring root privileges, isolate provides an optional path virtualization shim:

```sh
isolate run --cow ~/project --redirect-paths -- make
```

When `--redirect-paths` is specified, isolate injects `libisolate_shim.dylib` into the process environment using `DYLD_INSERT_LIBRARIES`. The shim intercepts POSIX filesystem calls (`open`, `openat`, `stat`, `lstat`, `access`, `faccessat`, `unlink`, `unlinkat`, `rename`, `renameat`, `opendir`, `chdir`, and `execve`). Any path argument starting with the original host directory path is rewritten in memory to point to the corresponding capsule layer directory before being dispatched to the kernel.

Because System Integrity Protection (SIP) strips dynamic linker variables for Apple-signed system binaries in `/bin` and `/usr/bin`, path virtualization applies to third-party tools, compilers, interpreters, and build systems (such as Homebrew binaries, Node.js, Python, Clang, and Rust).

## Terminal Keystroke Injection Protection (`--pty`)

When running interactive terminal sessions, granting a sandboxed process direct write access to the host controlling terminal device (`/dev/ttys*`) creates a privilege escape vector. On POSIX and Darwin systems, an unconfined process with write access to its terminal can invoke `ioctl(fd, TIOCSTI, &byte)` to inject arbitrary keystrokes into the host terminal input buffer. When isolate terminates, those injected commands would execute in the user's host shell.

To eliminate this vulnerability, isolate provides terminal keystroke injection protection via a pseudoterminal proxy:

```sh
isolate run --pty -- /bin/sh -i
```

When `--pty` is enabled (which is active by default during interactive terminal sessions), isolate allocates a dedicated pseudoterminal pair using `openpty(3)`. The supervisor configures the host terminal into raw mode and pumps input and output across the master descriptor. The sandboxed child receives access only to the secondary slave terminal device node. Any attempt by the sandboxed workload to issue `ioctl(TIOCSTI)` writes keystrokes into the internal slave buffer rather than the host terminal buffer, preventing sandbox escapes upon exit. You can pass `--no-pty` to disable the proxy if direct host character device access is explicitly desired.

## Exclude Patterns (`--exclude` and `.isolateignore`)

To keep diffs, extent scans, and commit calculations focused on relevant project source files, isolate supports file and directory exclusion patterns:

```sh
isolate diff project --exclude "node_modules/**" --exclude "*.log"
isolate commit project --exclude "build/**"
```

In addition to command-line flags, isolate automatically searches for `.isolateignore` and `.gitignore` files located in the root of each cloned workspace mount. Excluded patterns are skipped during recursive directory scans, preventing temporary build artifacts, large dependency caches, and version control metadata from cluttering diff outputs or triggering unneeded file synchronization.

## Disposable and Persistent Workflows

### Disposable Runs (`--ephemeral`)

You can create temporary capsules that are discarded automatically as soon as the program finishes:

```sh
isolate run --ephemeral --cow ~/project -- ./unknown-installer
```

If an ephemeral run fails or is interrupted, its directory and layers are cleaned up immediately. You can pass `--keep` if you want to retain the capsule state after an unexpected failure:

```
$ isolate diff work
/Users/you/project
  D README.old
  A build/  214 entries, 856B
  m configure  (mode 0644 -> 0755)
  M src/main.c

  1 added, 1 modified, 1 deleted, 1 metadata only
  278 entries examined; 60 proven unchanged from their extent maps, 1 compared byte for byte
```

Change indicators correspond to standard filesystem events:
- `A`: Entry added
- `D`: Entry deleted
- `M`: File contents modified
- `T`: File type changed (for example, a regular file replaced with a symlink)
- `m`: Metadata modified (permissions, flags, or ownership)

Unmodified files in an APFS clone retain their original block references. Isolate queries block extents via `F_LOG2PHYS_EXT` to prove that files are identical without having to read them from disk. If block sharing has broken, isolate performs a byte-by-byte comparison to confirm whether actual content was altered.

### Persistent Capsules

For multi-stage tasks such as installing dependencies and then running tests, you can create a named, persistent capsule:

```sh
isolate run --name api --cow ~/api --network outbound -- npm install
isolate enter api -- npm test
isolate enter api                       # Opens an interactive shell inside the capsule
isolate commit api
```

A capsule's security policy is locked at creation time. The `enter` command rejects filesystem, network, and Mach service flags to guarantee that the configuration shown by `isolate inspect` remains an accurate description of the capsule's entire execution history. Resource limits, however, may be specified per run since they can only restrict execution further.

The `isolate commit` command performs a three-way comparison between the baseline copy captured at capsule creation, the current capsule layer, and the current host directory. If a file was modified both inside the capsule and on the host, isolate flags it as a conflict:

```
$ isolate commit api
/Users/you/api
  A package-lock.json
  M package.json

  ? src/index.js  (also changed on the host)

  applied 2 changes to /Users/you/api
  skipped 1 conflicting path; re-run with --force to overwrite the host version
```

## Snapshot Management and Point-in-Time Diffing

You can capture and restore lightweight point-in-time states of a capsule using the snapshot command:

```sh
# Create a snapshot before running experimental commands
isolate snapshot create api clean --note "State before migration"

# Inspect changes after running code
isolate enter api -- ./run-migration.sh

# Revert back to the snapshot state if needed
isolate snapshot restore api clean --force
```

### Diffing Against Specific Snapshots

By default, `isolate diff` compares the active working layer against the initial creation baseline. You can also inspect incremental progress across specific points in time using the `--snapshot` flag:

```sh
# Compare the current working layer against a specific snapshot
isolate diff api --snapshot clean

# Compare two historical snapshots directly against each other
isolate diff api --snapshot stage1 --snapshot stage2
```

Snapshots leverage APFS cloning to preserve layer state with negligible storage overhead. Restoring a snapshot reverts both the working layer and diff baselines to the exact point in time when the snapshot was captured.

## Garbage Collection and Stale Lock Pruning (`isolate gc`)

If an ephemeral capsule run is interrupted abruptly (such as through `kill -9` or an unexpected operating system reboot), orphaned capsule directories can remain on disk.

The `isolate gc` command scans the capsule store, tests advisory locks on each directory with non-blocking file locking, and purges orphaned or abandoned capsules:

```sh
# Show which capsules would be cleaned up without deleting them
isolate gc --dry-run

# Prune unreferenced capsules older than seven days
isolate gc --older-than 7d

# Clean up all unlocked ephemeral or stopped capsules
isolate gc --all
```

Capsules that are actively running hold an exclusive lock on their `lock` file descriptor, ensuring that active workloads are never touched by garbage collection.

## Capsule Export and Portability (`isolate export` and `isolate import`)

You can export a complete capsule, including its policy configuration, creation baselines, working layers, and snapshots, into a single compressed archive:

```sh
# Export a capsule to an archive file
isolate export api api-cache.tar.gz

# Import the archive onto another machine or in a continuous integration environment
isolate import api-cache.tar.gz --name api-restored
```

When importing an archive, isolate assigns a fresh unique identifier to prevent collisions with existing local capsules while preserving the original filesystem layers and snapshot records. This capability simplifies sharing reproducible development environments and caching build environments in CI/CD pipelines.

## Automated Mach Denial Tracer (`--trace-denials`)

When running complex tools under a minimal Mach service bundle (`--services minimal`), a third-party process may fail unexpectedly because it requires a specific system Mach service that was not permitted by the policy.

The `--trace-denials` flag automatically streams sandbox violation events from the macOS Unified Logging subsystem:

```sh
isolate run --trace-denials --cow ~/project -- ./third-party-tool
```

When the sandboxed process attempts to look up an unauthorized Mach service, the tracer intercepts the kernel denial event and outputs a notice directly to standard error:

```
isolate: notice: program requested Mach service 'com.apple.coreservices.launchservicesd'; retry with '--service com.apple.coreservices.launchservicesd'
```

This discovery feature eliminates guesswork when tuning minimal security profiles for third-party build systems.

## Resource Ceilings and Priorities

You can specify resource limits when launching or entering a capsule:

```sh
isolate run --memory 4G --processes 100 --timeout 30m --cpu-time 20m \
            --cow ~/project -- make -j8
```

You can inspect active limits and their enforcement mechanisms using `isolate inspect`:

```
$ isolate inspect project
LIMIT       VALUE     ENFORCED BY  MECHANISM
memory      4.0G      isolate      supervisor samples total physical footprint (proc_pid_rusage)
cpu time    20m00s    kernel       RLIMIT_CPU; delivers SIGXCPU then SIGKILL
processes   100       isolate      supervisor monitors process count; signals newest members
wall time   30m00s    isolate      supervisor monotonic kqueue timer
core dumps  disabled  kernel       RLIMIT_CORE set to 0
```

The `--cpu 200%` and `--background` options configure the Quality-of-Service (QoS) scheduling clamp. This clamp sets an upper ceiling on thread priority and is inherited by all descendant processes. It does not limit core count or CPU time shares; instead, it prevents background builds from competing with foreground user tasks.

## Mach Service Bundles

Isolate groups Mach services into predefined bundles to balance application functionality against boundary surface area:

```
--services none          Disables Mach service lookups entirely.
--services minimal       Permits basic libSystem lookups: user information, preferences, and logging (default).
--services development   Includes minimal services plus LaunchServices, CoreServices, FSEvents, and Keychain.
--services graphical     Includes development services plus WindowServer, pasteboard, and audio output.
--service NAME           Permits lookup of an additional Mach service by name.
```

The graphical bundle grants access to the WindowServer and clipboard subsystems, allowing programs to display windows and read clipboard contents. This expands the security surface area and should only be granted when required by GUI or testing tools.

## Dropping Privileges (`--user`)

When executed as root, isolate can drop privileges to an unprivileged user:

```sh
sudo isolate run --user nobody --cow ~/project -- ./untrusted-build
```

Isolate changes the ownership of writable guest layers and temporary scratch directories to the specified user, and ensures enclosing parent paths have directory traversal permissions (`0711`). Configuration files such as `capsule.json`, baselines, and snapshot directories remain owned by root, preventing the unprivileged process from tampering with its own security policy or altering diff reports. During `isolate commit`, original host file ownership is preserved.

## Developer Toolchain Integration (`--toolchain`)

Command-line tools in `/usr/bin` (such as `clang`, `git`, and `python3`) are Apple wrapper shims that redirect to actual binaries located in the active developer directory. Passing `--toolchain` grants read-only access to the developer directory and selects the `development` service bundle:

```sh
isolate run --toolchain --cow ~/project -- make
```

Homebrew paths are not granted by default. You can include them explicitly by adding `--ro /opt/homebrew` (on Apple silicon) or `--ro /usr/local` (on Intel).

## System Verification with Doctor

You can verify whether your running kernel and system configuration support every isolation mechanism by running `isolate doctor`:

```
$ isolate doctor
ok       sandbox profiles         the kernel accepts and applies generated profiles
ok       filesystem rules         writes outside the policy are refused by the kernel
ok       network rules            a capsule with network: none cannot open outbound connections
ok       signal isolation         capsule processes cannot signal processes outside the capsule
ok       process attestation      the kernel confirms which processes carry a capsule's profile
limited  host process visibility  a capsule can list every process on the system ...
ok       APFS cloning             capsule storage is on apfs and supports clonefile
ok       extent comparison        a clone's blocks can be compared without reading the file
ok       resource limits          kernel-enforced: cpu time, file size, open files, core size
limited  memory limits            no kernel memory ceiling is available here ...
ok       scheduling clamp         a clamped process runs at priority 4 instead of 31
limited  credentials              isolate is not running as root, so a capsule runs as you
```

Checks that report `limited` describe inherent architectural constraints of macOS for unprivileged users rather than software defects. The doctor output prints the exact system calls that were attempted and the error codes returned by the kernel.

## Technical Architecture

For a comprehensive explanation of the underlying macOS kernel mechanisms, Seatbelt profile compilation, APFS extent verification, process attestation, PTY proxy mechanics, and path virtualization shims, see [Technical Architecture](docs/architecture.md).

For detailed threat analysis and boundary guarantees, see [Threat Model](docs/threat-model.md) and [macOS vs Linux Isolation Comparison](docs/macos-vs-linux.md).

## Building and Installation

### Prerequisites

Building isolate requires:
- macOS 14 (Sonoma) or newer (developed and tested on macOS 15 and macOS 26 on Apple silicon and Intel)
- CMake 3.20 or newer
- Xcode Command Line Tools (`xcode-select --install`)
- Ninja build system (recommended)

No third-party libraries or package managers are required. Root privileges are only necessary when using the `--user` option to execute capsules under a different user account.

### Build Commands

```sh
cmake -S . -B build -G Ninja
ninja -C build
ctest --test-dir build
sudo ninja -C build install
```

The test suite includes 281 unit tests and 150 integration tests that verify behavior directly against the running macOS kernel.

### Running Tests

You can execute the test suites using `ctest` or by running the test binaries directly:

```sh
# Run all unit and integration suites
ctest --test-dir build

# Run a specific unit test suite
./build/tests/isolate_unit_tests tracker

# Run integration tests (credentials suite requires sudo)
sudo ./build/tests/isolate_integration_tests credentials

# Build and run with AddressSanitizer and UndefinedBehaviorSanitizer
cmake -S . -B build-asan -DISOLATE_ASAN=ON
ninja -C build-asan
ctest --test-dir build-asan
```

Tests that require external network access automatically skip when an internet connection is unavailable. Tests that require root privileges skip automatically unless executed with superuser permissions.

## License

Isolate is licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE) for details.