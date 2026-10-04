#include "core/spec.h"

#include "core/ids.h"
#include "util/error.h"
#include "util/path.h"
#include "util/str.h"

#include <algorithm>
#include <utility>

namespace isolate {
namespace {

template <typename T>
void put_optional(json::Object& obj, const char* key, const std::optional<T>& value) {
    if (value.has_value()) obj.insert_or_assign(key, json::Value(*value));
}

std::optional<uint64_t> get_optional_u64(const json::Value& v, const char* key) {
    const json::Value* field = v.get(key);
    if (!field || field->is_null()) return std::nullopt;
    int64_t n = field->as_int();
    if (n < 0) throw Error(std::string("capsule.json: '") + key + "' must not be negative");
    return static_cast<uint64_t>(n);
}

std::optional<int> get_optional_int(const json::Value& v, const char* key) {
    const json::Value* field = v.get(key);
    if (!field || field->is_null()) return std::nullopt;
    int64_t n = field->as_int();
    if (n < 0) throw Error(std::string("capsule.json: '") + key + "' must not be negative");
    return static_cast<int>(n);
}

std::optional<double> get_optional_double(const json::Value& v, const char* key) {
    const json::Value* field = v.get(key);
    if (!field || field->is_null()) return std::nullopt;
    return field->as_double();
}

std::vector<std::string> string_array(const json::Value& v, const char* key) {
    std::vector<std::string> out;
    const json::Value* field = v.get(key);
    if (!field || field->is_null()) return out;
    for (const json::Value& item : field->as_array()) out.push_back(item.as_string());
    return out;
}

} // namespace

const char* to_string(Access a) {
    switch (a) {
        case Access::Deny: return "deny";
        case Access::ReadOnly: return "ro";
        case Access::ReadWrite: return "rw";
    }
    return "?";
}

const char* to_string(Storage s) {
    switch (s) {
        case Storage::Host: return "host";
        case Storage::Clone: return "clone";
    }
    return "?";
}

const char* to_string(Network n) {
    switch (n) {
        case Network::None: return "none";
        case Network::Outbound: return "outbound";
        case Network::All: return "all";
    }
    return "?";
}

const char* to_string(ServiceBundle b) {
    switch (b) {
        case ServiceBundle::None: return "none";
        case ServiceBundle::Minimal: return "minimal";
        case ServiceBundle::Development: return "development";
        case ServiceBundle::Graphical: return "graphical";
    }
    return "?";
}

Access parse_access(std::string_view s) {
    std::string v = str::lower(s);
    if (v == "deny" || v == "none") return Access::Deny;
    if (v == "ro" || v == "read-only" || v == "readonly") return Access::ReadOnly;
    if (v == "rw" || v == "read-write" || v == "readwrite") return Access::ReadWrite;
    throw Error("unknown access mode '" + std::string(s) + "'; expected deny, ro, or rw");
}

Network parse_network(std::string_view s) {
    std::string v = str::lower(s);
    if (v == "none" || v == "off" || v == "disabled") return Network::None;
    if (v == "outbound" || v == "out") return Network::Outbound;
    if (v == "all" || v == "host" || v == "on") return Network::All;
    throw Error("unknown network mode '" + std::string(s) +
                "'; expected none, outbound, or all");
}

ServiceBundle parse_service_bundle(std::string_view s) {
    std::string v = str::lower(s);
    if (v == "none") return ServiceBundle::None;
    if (v == "minimal" || v == "min") return ServiceBundle::Minimal;
    if (v == "development" || v == "dev") return ServiceBundle::Development;
    if (v == "graphical" || v == "gui") return ServiceBundle::Graphical;
    throw Error("unknown service bundle '" + std::string(s) +
                "'; expected none, minimal, development, or graphical");
}

bool Limits::any_kernel_enforced() const {
    return cpu_seconds.has_value() || file_bytes.has_value() || open_files.has_value();
}

bool Limits::any_supervised() const {
    return memory_bytes.has_value() || max_processes.has_value() || wall_seconds.has_value();
}

bool Limits::any() const {
    return any_kernel_enforced() || any_supervised() || cpu_multiple.has_value() ||
           background_priority;
}

const Mount* Spec::primary_mount() const {
    for (const Mount& m : mounts) {
        if (m.primary) return &m;
    }
    return nullptr;
}

void Spec::validate() const {
    if (!is_valid_capsule_id(id)) throw Error("capsule identifier '" + id + "' is malformed");
    if (!name.empty() && !is_valid_capsule_name(name)) {
        throw Error("capsule name '" + name + "' is malformed");
    }
    if (format_version != 1) {
        throw Error("capsule.json has format version " + std::to_string(format_version) +
                    ", which this build of isolate does not understand");
    }
    if (command.empty()) throw Error("a capsule needs a command to run");

    int primaries = 0;
    for (const Mount& m : mounts) {
        if (m.host.empty() || m.host.front() != '/') {
            throw Error("mount path '" + m.host + "' is not absolute");
        }
        if (m.host != path::normalize(m.host)) {
            throw Error("mount path '" + m.host + "' is not normalised");
        }
        if (m.access == Access::Deny && m.storage == Storage::Clone) {
            throw Error("mount '" + m.host + "' cannot be both denied and cloned");
        }
        if (m.storage == Storage::Clone && m.access != Access::ReadWrite) {
            throw Error("cloned mount '" + m.host + "' must be writable; a read-only clone "
                        "has no purpose over a read-only host mount");
        }
        if (m.primary) {
            ++primaries;
            if (m.storage != Storage::Clone) {
                throw Error("the primary mount must be a cloned mount");
            }
        }
    }
    if (primaries > 1) throw Error("only one mount may be the primary mount");

    // Would make the policy depend on rule order.
    std::vector<std::string> paths;
    for (const Mount& m : mounts) paths.push_back(m.host);
    std::sort(paths.begin(), paths.end());
    auto dup = std::adjacent_find(paths.begin(), paths.end());
    if (dup != paths.end()) {
        throw Error("path '" + *dup + "' appears in more than one mount");
    }

    if (limits.memory_bytes.has_value() && *limits.memory_bytes < (16u << 20)) {
        throw Error("a memory limit below 16M cannot be met by a process that has "
                    "loaded libSystem");
    }
    if (limits.max_processes.has_value() && *limits.max_processes < 1) {
        throw Error("the process limit must be at least 1");
    }
    if (limits.open_files.has_value() && *limits.open_files < 16) {
        throw Error("an open-file limit below 16 cannot be met by dyld");
    }
    if (limits.wall_seconds.has_value() && *limits.wall_seconds <= 0) {
        throw Error("the time limit must be positive");
    }
    if (credentials.umask > 0777) throw Error("umask must be an octal value up to 777");
}

json::Value Spec::to_json() const {
    json::Object root;
    root.insert_or_assign("format_version", json::Value(format_version));
    root.insert_or_assign("id", json::Value(id));
    root.insert_or_assign("name", json::Value(name));
    root.insert_or_assign("created_at", json::Value(created_at));
    root.insert_or_assign("ephemeral", json::Value(ephemeral));

    json::Array mount_array;
    for (const Mount& m : mounts) {
        json::Object o;
        o.insert_or_assign("host", json::Value(m.host));
        o.insert_or_assign("access", json::Value(to_string(m.access)));
        o.insert_or_assign("storage", json::Value(to_string(m.storage)));
        if (m.primary) o.insert_or_assign("primary", json::Value(true));
        if (!m.clone_name.empty()) o.insert_or_assign("clone_name", json::Value(m.clone_name));
        mount_array.emplace_back(std::move(o));
    }
    root.insert_or_assign("mounts", json::Value(std::move(mount_array)));

    root.insert_or_assign("network", json::Value(to_string(network)));
    root.insert_or_assign("services", json::Value(to_string(services)));

    json::Array extra;
    for (const std::string& s : extra_services) extra.emplace_back(s);
    root.insert_or_assign("extra_services", json::Value(std::move(extra)));

    json::Object lim;
    put_optional(lim, "cpu_seconds", limits.cpu_seconds);
    put_optional(lim, "file_bytes", limits.file_bytes);
    put_optional(lim, "open_files", limits.open_files);
    put_optional(lim, "memory_bytes", limits.memory_bytes);
    put_optional(lim, "max_processes", limits.max_processes);
    put_optional(lim, "wall_seconds", limits.wall_seconds);
    put_optional(lim, "cpu_multiple", limits.cpu_multiple);
    lim.insert_or_assign("core_dumps", json::Value(limits.core_dumps));
    lim.insert_or_assign("background_priority", json::Value(limits.background_priority));
    root.insert_or_assign("limits", json::Value(std::move(lim)));

    json::Object env_obj;
    env_obj.insert_or_assign("inherit", json::Value(env.inherit));
    json::Array pass;
    for (const std::string& s : env.passthrough) pass.emplace_back(s);
    env_obj.insert_or_assign("passthrough", json::Value(std::move(pass)));
    json::Array sets;
    for (const auto& [key, value] : env.set) {
        json::Object entry;
        entry.insert_or_assign("name", json::Value(key));
        entry.insert_or_assign("value", json::Value(value));
        sets.emplace_back(std::move(entry));
    }
    env_obj.insert_or_assign("set", json::Value(std::move(sets)));
    root.insert_or_assign("environment", json::Value(std::move(env_obj)));

    json::Object cred;
    cred.insert_or_assign("user", json::Value(credentials.user));
    cred.insert_or_assign("block_setuid_exec", json::Value(credentials.block_setuid_exec));
    cred.insert_or_assign("umask", json::Value(static_cast<int64_t>(credentials.umask)));
    root.insert_or_assign("credentials", json::Value(std::move(cred)));

    root.insert_or_assign("private_tmp", json::Value(private_tmp));
    root.insert_or_assign("private_home", json::Value(private_home));
    root.insert_or_assign("permissive_metadata", json::Value(permissive_metadata));
    root.insert_or_assign("redirect_paths", json::Value(redirect_paths));

    json::Array argv;
    for (const std::string& a : command) argv.emplace_back(a);
    root.insert_or_assign("command", json::Value(std::move(argv)));
    root.insert_or_assign("cwd", json::Value(cwd));

    return json::Value(std::move(root));
}

Spec Spec::from_json(const json::Value& v) {
    Spec s;
    s.format_version = static_cast<int>(v.get_int("format_version", 1));
    if (s.format_version != 1) {
        throw Error("capsule.json has format version " + std::to_string(s.format_version) +
                    ", which this build of isolate does not understand");
    }
    s.id = v.at("id").as_string();
    s.name = v.get_string("name", "");
    s.created_at = v.get_int("created_at", 0);
    s.ephemeral = v.get_bool("ephemeral", false);

    if (const json::Value* mounts = v.get("mounts"); mounts && !mounts->is_null()) {
        for (const json::Value& item : mounts->as_array()) {
            Mount m;
            m.host = item.at("host").as_string();
            m.access = parse_access(item.get_string("access", "ro"));
            m.storage = item.get_string("storage", "host") == "clone" ? Storage::Clone
                                                                      : Storage::Host;
            m.primary = item.get_bool("primary", false);
            m.clone_name = item.get_string("clone_name", "");
            s.mounts.push_back(std::move(m));
        }
    }

    s.network = parse_network(v.get_string("network", "none"));
    s.services = parse_service_bundle(v.get_string("services", "minimal"));
    s.extra_services = string_array(v, "extra_services");

    if (const json::Value* lim = v.get("limits"); lim && !lim->is_null()) {
        s.limits.cpu_seconds = get_optional_double(*lim, "cpu_seconds");
        s.limits.file_bytes = get_optional_u64(*lim, "file_bytes");
        s.limits.open_files = get_optional_int(*lim, "open_files");
        s.limits.memory_bytes = get_optional_u64(*lim, "memory_bytes");
        s.limits.max_processes = get_optional_int(*lim, "max_processes");
        s.limits.wall_seconds = get_optional_double(*lim, "wall_seconds");
        s.limits.cpu_multiple = get_optional_double(*lim, "cpu_multiple");
        s.limits.core_dumps = lim->get_bool("core_dumps", false);
        s.limits.background_priority = lim->get_bool("background_priority", false);
    }

    if (const json::Value* env = v.get("environment"); env && !env->is_null()) {
        s.env.inherit = env->get_bool("inherit", false);
        s.env.passthrough = string_array(*env, "passthrough");
        if (const json::Value* sets = env->get("set"); sets && !sets->is_null()) {
            for (const json::Value& item : sets->as_array()) {
                s.env.set.emplace_back(item.at("name").as_string(),
                                       item.at("value").as_string());
            }
        }
    }

    if (const json::Value* cred = v.get("credentials"); cred && !cred->is_null()) {
        s.credentials.user = cred->get_string("user", "");
        s.credentials.block_setuid_exec = cred->get_bool("block_setuid_exec", true);
        s.credentials.umask = static_cast<uint32_t>(cred->get_int("umask", 0077));
    }

    s.private_tmp = v.get_bool("private_tmp", true);
    s.private_home = v.get_bool("private_home", true);
    s.permissive_metadata = v.get_bool("permissive_metadata", false);
    s.redirect_paths = v.get_bool("redirect_paths", false);

    s.command = string_array(v, "command");
    s.cwd = v.get_string("cwd", "");
    return s;
}

} // namespace isolate
