# macOS and Linux isolation primitives

This is the comparison the project exists to answer: if you are used to Linux
namespaces and cgroups, what is actually available on macOS, and how close does
it get?

The short version: macOS has one strong primitive Linux does not have in the
same form: a path-based, operation-granular, inherited-and-immutable sandbox
policy applied by the kernel, and is missing most of the rest. Filesystem,
network and IPC isolation come out as good or better. Process and resource
isolation come out considerably worse. Identity isolation is absent without
root.

## Side by side

| Linux | macOS | What `isolate` does |
|---|---|---|
| mount namespace | nothing equivalent unprivileged | APFS clone, denied original, capsule's cwd set to the clone. Path identity is lost; containment is not. |
| overlayfs | nothing equivalent | `clonefile(2)`, which shares blocks until written. Comparable cost, no union mount. |
| PID namespace | nothing | kernel-attested membership via `sandbox_check`, which survives reparenting but does not hide the host's processes |
| network namespace | nothing | sandbox `network-*` rules. No separate stack, no veth, no per-capsule addresses; enforced via a kernel denial. |
| user namespace | nothing | `--user` with root: a real uid drop, verified. Unprivileged, nothing: the capsule runs as you. |
| IPC namespace | nothing for Mach | sandbox `mach-lookup` by service name, which is finer-grained than an IPC namespace |
| UTS namespace | nothing | nothing. A capsule sees the host's hostname. |
| cgroup memory limit | nothing unprivileged | sampled footprint, 50ms, with the overshoot reported |
| cgroup cpu quota | nothing unprivileged | QoS clamp: a priority ceiling, not a share |
| cgroup pids limit | nothing usable | supervisor counts and kills the excess |
| `RLIMIT_AS` | not implemented by Darwin | nothing; see above |
| seccomp | sandbox `syscall-unix` filtering exists but is not usable for third-party binaries | not used; the file, service and network rules are the boundary |
| `no_new_privs` | nothing | the standard setuid helpers are denied by path |
| capabilities | nothing comparable | nothing |
| LSM hooks (SELinux, AppArmor) | TrustedBSD MAC, which is what the sandbox is | used, through the supported `sandbox_init_with_parameters` interface |

## Where macOS is better

**Policy granularity.** A sandbox profile distinguishes `file-read-data` from
`file-read-metadata` from `file-write-create` from `file-map-executable`, per
path, with `literal`, `subpath`, `prefix`, `regex` and `path-ancestors`
matchers. Expressing "readable but not executable, and its existence may be
tested but its contents may not be read" takes one rule. On Linux that is a
mount option, a bind mount, an ACL and an LSM policy between them.

**IPC granularity.** A Linux IPC namespace isolates System V IPC and POSIX
message queues. It says nothing about which D-Bus services a process may reach;
that needs a separate policy layer. On macOS the equivalent of D-Bus is the Mach
bootstrap namespace, and the sandbox mediates lookups by name natively. A
capsule can be given `com.apple.logd` and nothing else.

**Inheritance that cannot be shed.** A sandbox profile is attached to the
process and inherited across `fork` and `exec`, and there is no operation to
remove or relax it. A process cannot escape its own profile, which is what makes
`sandbox_check` usable as a membership test.

**Copy-on-write that is a filesystem feature, not a mount.** `clonefile(2)` on
APFS clones a directory tree recursively in one syscall, sharing every block.
No mount, no privilege, no union filesystem, no upper and lower directory. The
clone is an ordinary directory that happens to share storage, and the sharing
is visible through `F_LOG2PHYS_EXT`, which is what makes change detection free.

## Where macOS is worse

**No path virtualisation.** This is the big one. Without a mount namespace there
is no way to present different content at the same path, so a capsule's private
copy of `/Users/you/project` lives somewhere else and the program sees a
different absolute path. Most build systems do not care, because they use
relative paths from the working directory. Anything with an absolute path baked
into it does care. There is no unprivileged fix.

**No process hiding.** `proc_listallpids` and `proc_pidinfo` are not mediated by
the sandbox. A capsule can enumerate the host's processes. Denying the
`kern.proc` sysctls removes one route and leaves `proc_info` open. This cannot
be closed from userspace, and it is the reason the project does not claim to
provide a PID namespace.

**No memory accounting an unprivileged process can use.** Jetsam has close to
the mechanism wanted (a per-process physical footprint limit), and both
interfaces to it are privileged:

```
task_set_phys_footprint_limit(mach_task_self(), 128, &old)  ->  KERN_NO_ACCESS
memorystatus_control(MEMORYSTATUS_CMD_SET_MEMLIMIT_PROPERTIES, getpid(), ...)  ->  EPERM
```

`RLIMIT_AS` is rejected with `EINVAL`. `RLIMIT_DATA` does not bound
`mmap`-backed allocation, which is how `libmalloc` gets its memory. So
unprivileged the options are sampling or nothing.

As root both Jetsam calls succeed, but they bound one process, and a capsule is
a set of processes: `--memory 4G` means the capsule's total, not 4G each. A
per-process limit would have to be applied to every member as it appears, which
has the same sampling window as just measuring the total. So isolate samples in
both cases, and `doctor` reports which situation you are in. A Linux cgroup
bounds the whole group in the kernel, with no equivalent on either side of this.

**No CPU quota.** A QoS clamp caps the priority a thread may request and is
inherited, which keeps a capsule from competing with interactive work. It does
not cap CPU consumption. A clamped capsule alone on the machine uses every core.

**No uid isolation without root.** A Linux user namespace lets an unprivileged
user become root inside a namespace and map it to themselves outside. macOS has
nothing like it. Unprivileged, a capsule runs as the invoking user and the
sandbox profile is the only thing between the workload and that user's files.
With root, `--user` gives a real uid boundary, but it is an ordinary account on
the same system, not a mapped namespace: the capsule can read whatever that uid
can read.

**Service deny-listing is name-based.** A capsule reaches a service by name, and
`isolate` decides which names resolve. What the service then does with the
message is outside the policy. A vulnerability in a granted service is an escape.

## What that adds up to

The closest native equivalent to a container on macOS is a kernel sandbox
profile plus an APFS clone plus a supervising process. That gets you real
filesystem, network, IPC and signal isolation, a process set that can be tracked
but not hidden, and resource limits that are partly kernel-enforced and partly
sampled.

Good enough for running unfamiliar software, isolating a build, keeping an
installer off the rest of the disk, and reversible experiments. In the
filesystem and IPC dimensions it is finer-grained than a bare Linux namespace
setup, and the copy-on-write is cheaper to set up.

Not good enough for multi-tenancy, for untrusted code that must not learn about
the host, or for hard resource guarantees. Those need a VM.
