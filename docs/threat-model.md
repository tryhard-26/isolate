# Threat model

This document says what a capsule contains, what it does not, and how an
adversarial program inside one could get out. It is written on the assumption
that the workload is hostile and knows it is in a capsule.

## What a capsule is

A capsule is three things:

1. A kernel sandbox profile, applied before `exec` and inherited by every
   descendant. This is the boundary. A process cannot read, modify, relax or
   remove the profile it runs under.
2. A directory holding the capsule's private `HOME` and `TMPDIR`, its APFS
   clones of any `--cow` mounts, and the baseline those clones are compared
   against.
3. A supervisor process outside the capsule that samples it, enforces the
   ceilings the kernel cannot, and cleans up afterwards.

Only the first of these is a security boundary. The second is a convenience
built on the first. The third is a policy mechanism that an adversary cannot
attack directly, but can outrun.

## Assets

In rough order of what a user would mind losing:

- Files under `$HOME` that were not granted to the capsule, in particular SSH
  keys, cloud credentials, browser profiles and `~/Library`.
- The keychain, and anything reachable through it.
- The user's other running processes, and anything they hold.
- The contents of a `--cow` mount's original, which the capsule is supposed to
  be unable to modify.
- The integrity of what `isolate diff` and `isolate commit` report. A capsule
  that could edit its own baseline could hide what it did.
- The network, and anything reachable from this machine's position on it.
- The machine's availability: CPU, memory and disk.

## Four kinds of isolation

Every message that mentions a limit says which of these it is.

### 1. Enforced by the operating system

Checked in the kernel, on the syscall path, against a profile the process cannot
change. These hold against an actively hostile workload.

| Property | Mechanism |
|---|---|
| Filesystem reads, writes, creation, deletion, execution, metadata | sandbox `file-*` rules, per path |
| Outbound and inbound network access | sandbox `network-*` rules; `connect` and `bind` fail with `EPERM` |
| Mach service lookups | sandbox `mach-lookup` rules, per service name |
| Signalling processes outside the capsule | `(allow signal (target same-sandbox))` |
| Executing the named privilege helpers | sandbox `process-exec*` denials by path |
| CPU time, file size, open files, core dumps | `setrlimit` before `exec` |
| Scheduling priority ceiling | QoS clamp via `posix_spawn`, inherited, not raisable |
| uid, with `--user` and root | `initgroups`/`setgid`/`setuid`, drop verified |

### 2. Enforced by the isolate runtime

The supervisor samples the capsule's members and acts. An adversary that is
faster than the sampling interval wins, temporarily.

| Property | How | What an adversary gets |
|---|---|---|
| Memory ceiling | sum of `ri_phys_footprint` every 50ms | can allocate as fast as the machine allows between samples; the overshoot is reported |
| | (as root the kernel would take a per-process Jetsam limit, but `--memory` is per-capsule, so sampling is used either way) | |
| Process count | count of attested members | can create processes faster than they are killed, briefly |
| Wall-clock limit | timer on the supervisor's kqueue | up to one sample interval of extra time |
| Cleanup of leftover processes | membership scan, `SIGTERM` then `SIGKILL` | can ignore `SIGTERM`, cannot ignore `SIGKILL` |

### 3. A user-facing abstraction only

- **The capsule's working directory being the clone.** Convenience, not
  containment. The original path is separately denied by the kernel, and that
  denial is what protects it.
- **Capsule names.** A label in a JSON file.
- **`isolate ps` showing "the capsule's processes".** The membership test is
  real and kernel-backed, but the capsule is not a kernel object; there is no
  entity to be a member of, only a profile to carry.

### 4. Not isolated at all

- **The process table.** `proc_listallpids` and `proc_pidinfo` are not mediated
  by the sandbox. A capsule can enumerate every process on the system and read
  names, executable paths, start times and resource usage. It cannot signal them
  or read their memory. This is the single largest difference from a Linux PID
  namespace and it cannot be closed from userspace.
- **The user identity, without root.** The capsule runs as the invoking user.
  Anything it is allowed to touch, it touches as you. With root, `--user` gives
  a real uid boundary; see below.
- **The kernel.** Shared. The sandbox reduces the reachable syscall and IOKit
  surface but does not eliminate it.
- **Hardware side channels.** Timing, caches, thermal and power state.
- **System-wide resources that are not per-process.** Disk space outside the
  capsule's own writable paths is bounded by the filesystem policy, but a
  capsule with a writable mount can fill that volume.

## Escape surface

Ways out, roughly by how likely they are to matter.

### Policy the user wrote

By far the most likely. A capsule is exactly as contained as its policy.

- `--rw ~` gives the capsule your entire home directory, which includes your SSH
  keys and your shell startup files. Writing to `~/.zshrc` is persistence on
  your account, executed the next time you open a terminal.
- `--rw` on anything that is later executed outside the capsule is code
  execution outside the capsule, deferred.
- `--network all` or `--network outbound` lets a capsule exfiltrate whatever it
  can read, and reach hosts that trust this machine's position on the network.
- `--services graphical` gives the window server and the pasteboard. The capsule
  can read your clipboard, draw over other windows and synthesise events.
- `--permissive-metadata` lets the capsule learn which files exist everywhere,
  including under paths it cannot read.
- `--allow-setuid` removes the denials on `sudo`, `su`, `launchctl`, `osascript`
  and the rest.
- `--inherit-env` passes your environment in, which routinely holds tokens.

`isolate inspect` prints the policy, and `isolate inspect --profile` prints the
profile that will be enforced, so a policy can be reviewed before it is trusted.

### --user, and what it changes

With root, `--user NAME` runs the capsule as another account. The drop is
`initgroups` then `setgid` then `setuid`, in that order, and isolate then checks
that the uid actually changed and that `setuid(0)` fails. Either check failing
is an error, not a warning: running the workload as root while the metadata
claims otherwise is worse than refusing.

A root-created capsule is `0700 root`, which the dropped user cannot traverse,
so isolate also hands over the parts it is supposed to reach:

- `home/`, `tmp/` and each `layers/<name>/` are `chown -R` to the target uid.
- The capsule directory and `layers/` become `0711`: traverse, not list.
- `capsule.json`, `state.json`, `base/` and `snapshots/` stay root-owned at
  `0700`/`0600`.

So the integrity property survives the drop. A `--user` capsule still cannot
read the policy being enforced on it, and still cannot edit the baseline that
`isolate diff` compares against (checked by
`tests/integration/test_credentials.cpp`).

Two consequences worth knowing:

- Committing a `--user` layer writes files whose layer copy is owned by the
  target uid. `commit` restores each host path's own previous owner, or its
  parent's for a new path, so committing a capsule that ran as `nobody` does not
  hand your files to `nobody`.
- The capsule store must sit somewhere the target user can traverse. isolate
  only changes permissions on directories it owns, so a store under
  `/var/root/.local/share` cannot be reached by `nobody` and isolate says which
  directory blocks it rather than failing with `EACCES` inside the capsule.

What `--user` does not do: it is not a user namespace. The capsule gets a
different real uid on the same system, so it can still read anything that uid
can read, which for `nobody` is most of a default macOS install's world-readable
files. The sandbox profile is still what bounds it; `--user` removes the
capsule's ability to touch *your* files even if the profile were wrong.

### setuid binaries

Denying `sudo`, `su`, `launchctl`, `osascript`, `open`, `security`, `crontab`,
`at`, `login`, `newgrp` and `authopen` by path blocks the usual escalation
routes. It is not a general defence: macOS has no `no_new_privs`, so a setuid
binary inside a path the capsule may execute still runs with its owner's
identity. Its capabilities remain bounded by the sandbox profile, which is
inherited, so this is a privilege increase inside the capsule rather than an
escape from it, but it is not nothing. Do not grant execute access to a
directory containing setuid binaries you did not audit.

### Mach services

Every service name granted is an interface to code running outside the capsule
with its own privileges. `minimal` is small for that reason. `development` and
`graphical` are larger because the tools need them, and each added name is
attack surface in a process the capsule does not control.
A vulnerability in a granted service is an escape, and `isolate` can only
decide which names are reachable, not what the service does with the message.

### The supervisor

The supervisor enforces the sampled ceilings and the cleanup. A capsule cannot
signal it, because `(allow signal (target same-sandbox))` denies signals to
anything outside the capsule's profile, and it cannot write to the capsule
directory. If the supervisor is killed from outside (by the user, or by the
system under memory pressure), the capsule's processes keep running with their
profile intact and nothing enforces the sampled limits any more. `isolate ls`
reports such a capsule as stopped with "supervisor exited without cleanup", and
`isolate kill` will still find and stop its processes, because membership does
not depend on the supervisor being alive.

### Filesystem races

`commit` writes the capsule's changes to host paths. It reads the capsule's
layer, which only the capsule and `isolate` can write, and writes to host paths
the user named. A capsule that is still running during a commit could change a
file between the comparison and the copy, so `commit` refuses to run while the
capsule is running. Snapshots refuse for the same reason.

Path policy is built from resolved paths, and the sandbox evaluates rules
against resolved paths, so a symlink inside a granted directory cannot widen
access: following it is checked against the policy again at the target. Tree
walks never follow symlinks, so a capsule cannot make `isolate diff` or
`isolate commit` traverse out of the layer.

### Capsule metadata

The capsule directory holds `capsule.json`, the baseline clones and the
snapshots. None of those paths appear in the generated profile, so the capsule
cannot read the policy being enforced on it or edit the baseline that
`isolate diff` compares against. `isolate` also refuses to create a capsule with
any non-denying mount that overlaps the capsule store, which closes the
indirect route.

### Resource exhaustion

A capsule can exhaust the machine faster than the supervisor samples. The
memory ceiling is the clearest case: a program that allocates as fast as it can
will exceed it, and the overshoot is reported rather than hidden. A capsule can
also fill any writable mount, and can spawn processes faster than they are
killed for a short time. None of these are contained in the way a cgroup
contains them, and nothing in the interface claims otherwise.

## Assumptions

- The kernel sandbox enforces what it compiles. If `sandbox_init_with_parameters`
  accepts a profile, the kernel applies it. `isolate doctor` verifies the
  specific behaviours relied on rather than assuming them.
- `sandbox_check` reports the target's own profile and cannot be influenced by
  the target. This is why membership cannot be forged. It is also why the
  exec-ordering detail matters: before `exec`, the answer is not yet the child's.
- The `isolate` binary and the capsule store are not writable by the workload.
  If an attacker can write to either, none of this holds.
- `arc4random_uniform` gives unguessable capsule identifiers. A guessable
  identifier would let one capsule test for another's presence, which is minor,
  but there is no reason to allow it.

## Reporting

The most useful finding is one where `isolate` claims a property the kernel does
not enforce. `isolate doctor --json` and `isolate inspect --profile <capsule>`
together print everything being asserted on a given machine.
