#pragma once

#include "util/json.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace isolate {

// Kernel sb enforced access.
enum class Access {
    Deny,      // Deny read/write/stat/exec
    ReadOnly,  // Read, stat, exec, map
    ReadWrite, // + create, write, unlink
};

enum class Storage {
    Host,  // Host path directly
    Clone, // APFS CoW clone in capsule storage
};

// Mount cfg. CoW mounts use APFS clone; guest sees clone path.
struct Mount {
    std::string host; // Resolved host path
    Access access = Access::ReadOnly;
    Storage storage = Storage::Host;
    bool primary = false;   // Capsule cwd
    std::string clone_name; // Subdir under <capsule>/layers

    std::string guest; // Path visible to guest proc
};

enum class Network {
    None,     // Loopback only
    Outbound, // Outbound connect ok, listen denied
    All,
};

// Mach service bundle presets (see sb/services.h).
enum class ServiceBundle {
    None,
    Minimal,
    Development,
    Graphical,
};

// Resource limits: kernel rlimits vs supervisor sampling.
struct Limits {
    // Kernel rlimits pre-exec
    std::optional<double> cpu_seconds;  // RLIMIT_CPU
    std::optional<uint64_t> file_bytes; // RLIMIT_FSIZE
    std::optional<int> open_files;      // RLIMIT_NOFILE
    bool core_dumps = false;            // RLIMIT_CORE

    // Supervised limits (polled)
    std::optional<uint64_t> memory_bytes; // Summed phys_footprint
    std::optional<int> max_processes;
    std::optional<double> wall_seconds;

    // Proc priority clamps
    std::optional<double> cpu_multiple; // Drives posix_spawn clamp
    bool background_priority = false;

    bool any() const;
    bool any_kernel_enforced() const;
    bool any_supervised() const;
};

struct Environment {
    bool inherit = false;
    std::vector<std::string> passthrough; // Env vars copied from host
    std::vector<std::pair<std::string, std::string>> set;
};

// Creds cfg; non-root cannot change uid.
struct Credentials {
    std::string user;              // Needs root; empty = unchanged
    bool block_setuid_exec = true; // Block setuid escalation helpers
    uint32_t umask = 0077;
};

// Capsule spec, serialized to <capsule>/capsule.json.
struct Spec {
    int format_version = 1;

    std::string id;
    std::string name;
    int64_t created_at = 0; // Unix epoch sec

    bool ephemeral = false;

    std::vector<Mount> mounts;
    Network network = Network::None;
    ServiceBundle services = ServiceBundle::Minimal;
    std::vector<std::string> extra_services; // Extra Mach service names
    Limits limits;
    Environment env;
    Credentials credentials;

    // Capsule private dirs (TMPDIR and HOME)
    bool private_tmp = true;
    bool private_home = true;

    // Permissive stat for symlink traversal
    bool permissive_metadata = false;

    // Dyld shim: rewrite host paths to guest clone paths
    bool redirect_paths = false;

    std::vector<std::string> command;
    std::string cwd; // Guest cwd

    // Validate mount/policy consistency; throws on conflict
    void validate() const;

    const Mount* primary_mount() const;

    json::Value to_json() const;
    static Spec from_json(const json::Value& v);
};

const char* to_string(Access a);
const char* to_string(Storage s);
const char* to_string(Network n);
const char* to_string(ServiceBundle b);

// Each throws w/ the accepted set.
Access parse_access(std::string_view s);
Network parse_network(std::string_view s);
ServiceBundle parse_service_bundle(std::string_view s);

} // namespace isolate
