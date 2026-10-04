# Design

How `isolate` is put together, and why each piece is the way it is.

## Layers

```
cli/       argument parsing, commands, output formatting
proc/      launching, resource limits, process tracking, supervision
sb/        sandbox profile generation, service bundles, membership checks
fsview/    APFS cloning, extent comparison, tree diffing, layers, snapshots
core/      capsule identity, policy, on-disk store
util/      errors, paths, filesystem, JSON, logging, locking
```

Dependencies point downward only. `core` does not know about sandboxes;
`fsview` does not know about processes; `sb` does not know where capsules are
stored. The CLI is the only layer that knows about all of them, which is what
keeps the rest unit-testable without a capsule.

Errors propagate as exceptions (`isolate::Error`), caught in `main`. The one
exception is the window between `fork` and `exec`, which must not throw,
allocate or run destructors; those functions are named `*_nothrow` and return a
message pointer instead.

## The capsule

A capsule is a directory and a policy.

```
<store>/capsules/<id>/
├── capsule.json          the policy, as the user gave it, resolved
├── state.json            run count, last exit status, last termination
├── lock                  flock held for the duration of a run
├── layers/<name>/         working copy of a --cow mount
├── base/<name>/           clone of the original when the capsule was created
├── snapshots/<name>/      layers + base + home at a point in time
├── home/                  private HOME
└── tmp/                   private TMPDIR
```

The policy and the state are separate files because a run has to record its
outcome without rewriting the policy it is enforcing. Both are JSON so that a
capsule stays inspectable with ordinary tools, and the format is the only thing
that has to stay compatible across versions. `format_version` is checked on
read, and an unknown version is refused rather than guessed at.

Writes to both go through a write-to-temporary-and-rename, with an `fsync`
before the rename, so a reader never sees half a file and a crash cannot leave
state pointing at a tree that does not exist.

### Identity

A capsule identifier is twelve characters of base32 from `arc4random_uniform`,
using an alphabet without `i`, `l`, `o` or `u` so it can be retyped from a
screenshot. It has to be unguessable because it appears in a Mach service name
that is the capsule's membership marker: a guessable identifier would let one
capsule test for another's existence.

Names are optional labels. A name that looks like an identifier is rejected, so
that `isolate ps <x>` is never ambiguous.

### Locking

A run takes an exclusive `flock` on `<capsule>/lock` for its duration. Read-only
commands do not take it, so `isolate ps` and `isolate inspect` work on a running
capsule. `commit`, `destroy`, `snapshot` and `enter` all take it, because they
write. The lock lives on an open file descriptor, so the kernel releases it if
`isolate` dies and there is no stale lock to clean up.

## Filesystem layers

`--cow PATH` creates two clones at capsule creation:

- `layers/<name>/`: what the capsule works on
- `base/<name>/`: a pristine copy, never written by the capsule

Both are `clonefile(2)` calls, which handle a directory tree recursively in one
syscall and share every block, so this is nearly free in time and space.

Keeping the baseline is what makes three-way reasoning possible:

```
base  vs  layer   =  what the capsule changed
base  vs  host    =  what changed outside the capsule since
```

A path that appears in both is a conflict, and `commit` withholds it unless
forced. Without the baseline, `commit` could only overwrite and hope. After a
successful commit the baseline is re-cloned from the host, so the next diff
reports only what happens from then on.

The baseline is also why a snapshot has to capture both trees: restoring the
working layer without restoring the baseline would leave the capsule reporting a
diff against a baseline from a different point in time.

### Change detection

The comparison is tiered, cheapest first, and correctness never depends on the
cheap tiers:

1. Present on one side only, or a different kind of entry: answer immediately.
2. Different size: modified.
3. Identical extent maps, read with `fcntl(F_LOG2PHYS_EXT)`: unchanged, with no
   data read. This is the tier that makes diffing a cloned tree nearly free: an
   untouched file still shares every block with its origin, and two files
   mapping to the same physical blocks are the same bytes.
4. Otherwise, compare the bytes. A clone whose sharing was broken may still hold
   identical contents; a build that rewrites a file without changing it is
   common, and reporting that as a modification would make every build look
   destructive.

An extent map that cannot be read is treated as unknown, never as unchanged.
Compressed files (`UF_COMPRESSED`) have no extents of their own, and pathological
fragmentation gives up after 4096 extents and falls through to tier 4.

Directories that appeared or disappeared whole collapse to a single change with
a count, because a new build directory is one thing that happened. `commit` then
applies that change with one `clonefile`, which is both faster and simpler than
walking it.

## Sandbox profile generation

The profile is generated fresh for every run, laid out general to specific, with
a comment above each section explaining why the rules are there. It is
deterministic for a given input, which is what lets `isolate inspect --profile`
show exactly what will be enforced.

### Rule precedence

The ordering scheme rests on one piece of observed behaviour, which is not what
the shape of a profile suggests. For a given concrete operation, the governing
rule is the one named with the **most specific operation token**. Only when two
rules name the same token does their order decide, and then the last wins. Path
specificity is not consulted at all.

```scheme
(allow file-read* (subpath "/a"))
(deny  file*      (subpath "/a/b"))
```

Reading `/a/b/f` is allowed, because `file-read*` is narrower than `file*`,
regardless of order. A generator that used `file-read*` for `--ro` and `file*`
for `--deny` would silently drop denials (and an earlier version of this one
did exactly that, which is why the behaviour is now asserted by a test that runs
against the kernel.

Every path rule therefore names the same explicit tokens:

```
read:   file-read-data file-read-metadata file-read-xattr file-test-existence
        file-map-executable
write:  file-write-data file-write-create file-write-unlink file-write-mode
        file-write-owner file-write-setugid file-write-times file-write-xattr
        file-write-flags file-ioctl
exec:   process-exec*
```

With the tokens equal, precedence reduces to order, and rules are emitted
shallowest first, so a deeper rule wins. `file*` and `file-read*` appear nowhere
in a generated profile except in comments.

### Sections

1. Header, `(version 1)`, `(deny default)`.
2. Process operations: fork, syscall classes, `sysctl-read`, `mach-bootstrap`,
   and `signal`/`process-info` limited to `same-sandbox`.
3. Read-only system paths: `/System`, `/usr/lib`, `/usr/share`, the cryptex
   graft points, `/private/var/select`, and the root-level symlinks `/etc`,
   `/tmp` and `/var` whose link contents have to be readable for any path
   through them to resolve.
4. Base tool directories, readable and executable.
5. Device nodes, the controlling terminal (only the capsule's own).
6. Mach services, from the bundle, plus the membership marker.
7. Network, per policy, plus Unix sockets inside the capsule's scratch
   directory so capsule processes can talk to each other with networking off.
8. The capsule's private `tmp` and `home`.
9. The user's mounts, shallowest first.
10. The originals behind `--cow` mounts, denied.
11. The privilege helpers, denied.
12. Metadata on the ancestor directories of every granted path, as literals, so
    the kernel can resolve those paths without the grant extending to the
    ancestors' contents.

The capsule directory itself is never granted, so `capsule.json`, the baselines
and the snapshots are unreachable from inside.

### Validation

Before a run, the profile is compiled and applied in a throwaway child process.
A profile that would be rejected is caught there, where it can be reported
clearly, rather than after `fork` where it would surface as a confusing exit
code.

## Launching

The order in the child is fixed, and the order is the point:

1. `setsid`, so a terminal hang-up or an interrupt aimed at `isolate`'s process
   group does not reach the capsule before the supervisor has decided what to do.
2. Reset the signal mask and dispositions the supervisor changed.
3. `chdir`, so the profile's working-directory rule is the one in force.
4. Credentials: `umask`, and dropping uid while still privileged enough to do
   it. A drop that does not take effect, or that can be undone, is treated as a
   failure rather than ignored.
5. `setrlimit`.
6. Apply the sandbox profile. Nothing can be relaxed after this, and everything
   above needs access the profile does not grant.
7. `posix_spawn` with `POSIX_SPAWN_SETEXEC`.

Everything the child needs (argv, envp, the profile string, the spawn
attributes) is built before `fork`, because between `fork` and `exec` the child
holds a copy of the parent's allocator locks and must not allocate. Failures are
reported to the parent over a close-on-exec pipe, so a program that cannot start
produces a message rather than a bare exit code.

The last step is `posix_spawn` rather than `execve` because a QoS clamp can only
be set through spawn attributes. `POSIX_SPAWN_SETEXEC` makes `posix_spawn`
replace the calling process, like `exec`, while still applying them. Setting the
clamp from the parent afterwards would leave a window in which the capsule ran
unclamped.

## Dropping to another uid

`--user` needs root, since macOS has no user namespaces. The drop happens in the
child, before the sandbox profile: `initgroups`, `setgid`, `setuid`, then a
check that the uid changed and that `setuid(0)` fails. Either check failing is
an error rather than a warning.

A capsule created by root is `0700 root`, which the target user cannot traverse,
so `Capsule::grant_to_user` hands over exactly the writable parts:

- `chown -R` on `home/`, `tmp/` and each `layers/<name>/`
- `0711` on the capsule directory and on `layers/`: traverse, not list
- `capsule.json`, `state.json`, `base/` and `snapshots/` left root-owned

That keeps the integrity property across the drop: the capsule still cannot read
its own policy or edit the baseline `diff` compares against.

Two knock-on effects, both covered by tests:

- The chown makes every entry's uid differ from the baseline, which would make
  `diff` pure ownership noise. `DiffOptions::ignore_ownership` is set for
  `--user` capsules, since isolate did the chown, not the capsule.
- A layer owned by `nobody` must not be committed as `nobody`. `apply_change`
  records the host path's existing owner, or its parent's for a new path, and
  restores it after writing.

isolate only changes permissions on directories it owns, so a store the target
user still cannot reach is reported by naming the directory that blocks it.

## Process tracking

macOS offers no way to hold onto a set of processes: `setpgid` leaves the process
group, `setsid` leaves the session, and a process whose parent exits is
reparented to `launchd`. Anything built on parentage loses a process that wants
to be lost.

Each capsule's profile contains one rule permitting lookup of
`dev.isolate.capsule.<id>`. No such service exists, so the rule grants nothing.
It is there to be observed from outside with
`sandbox_check(pid, "mach-lookup", SANDBOX_FILTER_GLOBAL_NAME, name)`, which
asks the kernel whether that process's profile would permit that lookup. The
answer cannot be forged from inside the capsule, because a process cannot change
its profile.

Two details make this work in practice:

- `sandbox_check` with a null operation must be consulted first. A process with
  no profile is allowed everything, so without that check an unsandboxed process
  would answer "allowed" and look like a member of every capsule.
- The kernel only reports a process's own profile once it has `exec`'d. Between
  `fork` and `exec` a child answers as though it were its parent. The tracker
  therefore also counts the children of known members, which closes that window
  without weakening the attested case. Each process's evidence (`attested` or
  `descendant`) is recorded and shown by `isolate ps`.

Verdicts are cached by pid and start time together, so a reused pid is never
mistaken for the process that held it before. Zombies are excluded: a zombie is
still listed and still accepts `kill`, so counting one would make `isolate`
report a leftover process where a process had in fact finished.

## Supervision

One kqueue carries everything:

- `EVFILT_PROC` with `NOTE_EXIT` on the capsule's first process
- `EVFILT_SIGNAL` for `SIGINT`, `SIGTERM`, `SIGHUP`, `SIGQUIT`, which are
  blocked so they arrive as events rather than interrupting whatever the
  supervisor is doing
- `EVFILT_TIMER` for the sample tick

So there is no signal handler racing the main loop, and no polling while nothing
is happening.

Sampling runs every 250ms normally, and every 50ms when a memory ceiling is set,
since sampling is the only enforcement available and the interval is the window
in which a capsule can exceed its ceiling. The first interrupt starts a graceful
shutdown; a second one escalates to `SIGKILL`, because a user pressing it twice
has stopped waiting.

When the capsule's first process exits, anything still running was reparented
and would outlive the capsule, so the supervisor scans, sends `SIGTERM`, waits
out the grace period, and sends `SIGKILL` to whatever is left. Both the leftover
count and whether a forced kill was needed end up in the run report.

## Testing

Two executables, no third-party dependency.

`isolate_unit_tests` covers the pure logic (path handling, JSON, policy
validation, profile generation) and the macOS behaviour the project depends on:
that a clone shares its blocks, that writing to one breaks the sharing, that an
extent map can be read, that membership attestation distinguishes two capsules,
that a zombie is excluded, and that neither kernel memory-limit interface is
available unprivileged. Several of these are checks on the operating system
rather than on `isolate`, and they are written so that a future macOS release
changing one of them fails a test rather than silently invalidating a claim.

`isolate_integration_tests` drives the built binary the way a user would and
then asks the kernel what happened: that a write outside the policy fails, that
`--deny` beats an enclosing `--ro`, that the original behind a `--cow` mount is
unreachable, that a capsule can neither signal `launchd` nor read its own policy
file, that a double-forked daemon is still cleaned up, and that the same `curl`
which fails under `--network none` succeeds under `--network outbound`. Each
capsule store is a temporary directory, and tests that need the internet skip
themselves when it is unreachable so that a failure always means `isolate` is
wrong.

Both run clean under AddressSanitizer and UndefinedBehaviorSanitizer
(`-DISOLATE_ASAN=ON`).
