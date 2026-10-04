#include "harness.h"

#include "core/ids.h"
#include "core/spec.h"
#include "util/error.h"
#include "util/json.h"

using namespace isolate;

namespace {

Spec minimal_spec() {
    Spec spec;
    spec.id = generate_capsule_id();
    spec.created_at = 1700000000;
    spec.command = {"/usr/bin/true"};
    return spec;
}

Mount make_mount(const std::string& host, Access access, Storage storage = Storage::Host) {
    Mount mount;
    mount.host = host;
    mount.access = access;
    mount.storage = storage;
    if (storage == Storage::Clone) mount.clone_name = "layer";
    return mount;
}

} // namespace

TEST(spec, generated_ids_are_valid_and_distinct) {
    std::string a = generate_capsule_id();
    std::string b = generate_capsule_id();
    CHECK(is_valid_capsule_id(a));
    CHECK(is_valid_capsule_id(b));
    CHECK_NE(a, b);
    CHECK_EQ(a.size(), size_t(12));
}

TEST(spec, id_validation_rejects_path_traversal) {
    // An identifier becomes a directory name and part of a Mach service name,
    // so anything that is not the exact alphabet has to be refused.
    CHECK(!is_valid_capsule_id(""));
    CHECK(!is_valid_capsule_id(".."));
    CHECK(!is_valid_capsule_id("../../etc/passwd"));
    CHECK(!is_valid_capsule_id("abc"));                 // too short
    CHECK(!is_valid_capsule_id("abcdefghijklmnop"));    // too long
    CHECK(!is_valid_capsule_id("ABCDEFGHJKMN"));        // upper case
    CHECK(!is_valid_capsule_id("abcdefghijkl"));        // 'i' and 'l' excluded
    CHECK(is_valid_capsule_id("0123456789ab"));
}

TEST(spec, name_validation) {
    CHECK(is_valid_capsule_name("my-project"));
    CHECK(is_valid_capsule_name("build_2"));
    CHECK(is_valid_capsule_name("a"));
    CHECK(!is_valid_capsule_name(""));
    CHECK(!is_valid_capsule_name(".hidden"));
    CHECK(!is_valid_capsule_name("-dash"));
    CHECK(!is_valid_capsule_name("has space"));
    CHECK(!is_valid_capsule_name("has/slash"));
    CHECK(!is_valid_capsule_name(std::string(65, 'a')));
    // A name that looks like an identifier would make a reference ambiguous.
    CHECK(!is_valid_capsule_name("0123456789ab"));
}

TEST(spec, name_from_path_sanitises) {
    CHECK_EQ(name_from_path("/Users/x/my project"), std::string("my-project"));
    CHECK_EQ(name_from_path("/Users/x/Repo.git"), std::string("repo.git"));
    CHECK_EQ(name_from_path("/a/b/CAPS"), std::string("caps"));
    CHECK_EQ(name_from_path("/a/b/..."), std::string(""));
    CHECK_EQ(name_from_path("/"), std::string(""));
}

TEST(spec, parse_access_accepts_aliases) {
    CHECK(parse_access("ro") == Access::ReadOnly);
    CHECK(parse_access("read-only") == Access::ReadOnly);
    CHECK(parse_access("RW") == Access::ReadWrite);
    CHECK(parse_access("deny") == Access::Deny);
    CHECK_THROWS(parse_access("maybe"));
}

TEST(spec, parse_network_accepts_aliases) {
    CHECK(parse_network("none") == Network::None);
    CHECK(parse_network("off") == Network::None);
    CHECK(parse_network("outbound") == Network::Outbound);
    CHECK(parse_network("all") == Network::All);
    CHECK_THROWS(parse_network("partial"));
}

TEST(spec, parse_service_bundle_accepts_aliases) {
    CHECK(parse_service_bundle("none") == ServiceBundle::None);
    CHECK(parse_service_bundle("min") == ServiceBundle::Minimal);
    CHECK(parse_service_bundle("dev") == ServiceBundle::Development);
    CHECK(parse_service_bundle("gui") == ServiceBundle::Graphical);
    CHECK_THROWS(parse_service_bundle("everything"));
}

TEST(spec, validate_accepts_a_minimal_spec) {
    CHECK_NO_THROW(minimal_spec().validate());
}

TEST(spec, validate_requires_a_command) {
    Spec spec = minimal_spec();
    spec.command.clear();
    CHECK_THROWS(spec.validate());
}

TEST(spec, validate_rejects_relative_and_unnormalised_mounts) {
    Spec spec = minimal_spec();
    spec.mounts.push_back(make_mount("relative/path", Access::ReadOnly));
    CHECK_THROWS(spec.validate());

    spec.mounts.clear();
    spec.mounts.push_back(make_mount("/a/b/../c", Access::ReadOnly));
    CHECK_THROWS(spec.validate());
}

TEST(spec, validate_rejects_contradictory_mounts) {
    Spec spec = minimal_spec();
    Mount mount = make_mount("/a", Access::Deny, Storage::Clone);
    spec.mounts.push_back(mount);
    CHECK_THROWS(spec.validate());

    // A read-only clone would cost space and protect nothing the host path
    // does not already protect.
    spec.mounts.clear();
    Mount readonly_clone = make_mount("/a", Access::ReadOnly, Storage::Clone);
    spec.mounts.push_back(readonly_clone);
    CHECK_THROWS(spec.validate());
}

TEST(spec, validate_rejects_duplicate_mount_paths) {
    Spec spec = minimal_spec();
    spec.mounts.push_back(make_mount("/a", Access::ReadOnly));
    spec.mounts.push_back(make_mount("/a", Access::ReadWrite));
    CHECK_THROWS(spec.validate());
}

TEST(spec, validate_allows_nested_mount_paths) {
    // Nesting is the whole point of --ro /a --deny /a/secret.
    Spec spec = minimal_spec();
    spec.mounts.push_back(make_mount("/a", Access::ReadOnly));
    spec.mounts.push_back(make_mount("/a/secret", Access::Deny));
    CHECK_NO_THROW(spec.validate());
}

TEST(spec, validate_rejects_more_than_one_primary_mount) {
    Spec spec = minimal_spec();
    Mount first = make_mount("/a", Access::ReadWrite, Storage::Clone);
    first.primary = true;
    first.clone_name = "a";
    Mount second = make_mount("/b", Access::ReadWrite, Storage::Clone);
    second.primary = true;
    second.clone_name = "b";
    spec.mounts.push_back(first);
    spec.mounts.push_back(second);
    CHECK_THROWS(spec.validate());
}

TEST(spec, validate_requires_the_primary_to_be_a_clone) {
    Spec spec = minimal_spec();
    Mount mount = make_mount("/a", Access::ReadWrite, Storage::Host);
    mount.primary = true;
    spec.mounts.push_back(mount);
    CHECK_THROWS(spec.validate());
}

TEST(spec, validate_rejects_unmeetable_limits) {
    Spec spec = minimal_spec();
    spec.limits.memory_bytes = 1024;
    CHECK_THROWS(spec.validate());

    spec = minimal_spec();
    spec.limits.max_processes = 0;
    CHECK_THROWS(spec.validate());

    spec = minimal_spec();
    spec.limits.open_files = 4;
    CHECK_THROWS(spec.validate());

    spec = minimal_spec();
    spec.limits.wall_seconds = 0;
    CHECK_THROWS(spec.validate());

    spec = minimal_spec();
    spec.credentials.umask = 01000;
    CHECK_THROWS(spec.validate());
}

TEST(spec, json_round_trip_preserves_everything) {
    Spec original = minimal_spec();
    original.name = "project";
    original.ephemeral = true;
    original.network = Network::Outbound;
    original.services = ServiceBundle::Development;
    original.extra_services = {"com.apple.example", "com.apple.other"};
    original.permissive_metadata = true;
    original.private_tmp = false;
    original.private_home = false;
    original.cwd = "/work";
    original.command = {"/bin/sh", "-c", "echo hi"};

    Mount clone = make_mount("/src", Access::ReadWrite, Storage::Clone);
    clone.primary = true;
    clone.clone_name = "src";
    original.mounts.push_back(clone);
    original.mounts.push_back(make_mount("/usr/local", Access::ReadOnly));
    original.mounts.push_back(make_mount("/private/secret", Access::Deny));

    original.limits.memory_bytes = 4ull << 30;
    original.limits.cpu_multiple = 2.0;
    original.limits.cpu_seconds = 600;
    original.limits.max_processes = 64;
    original.limits.wall_seconds = 1800;
    original.limits.file_bytes = 1ull << 30;
    original.limits.open_files = 512;
    original.limits.core_dumps = true;
    original.limits.background_priority = true;

    original.env.inherit = false;
    original.env.passthrough = {"TERM", "LANG"};
    original.env.set = {{"CC", "clang"}, {"DEBUG", "1"}};
    original.credentials.user = "nobody";
    original.credentials.block_setuid_exec = false;
    original.credentials.umask = 0022;

    Spec parsed = Spec::from_json(json::parse(json::dump(original.to_json())));

    CHECK_EQ(parsed.id, original.id);
    CHECK_EQ(parsed.name, original.name);
    CHECK_EQ(parsed.created_at, original.created_at);
    CHECK_EQ(parsed.ephemeral, original.ephemeral);
    CHECK(parsed.network == original.network);
    CHECK(parsed.services == original.services);
    CHECK_EQ(parsed.extra_services, original.extra_services);
    CHECK_EQ(parsed.permissive_metadata, original.permissive_metadata);
    CHECK_EQ(parsed.private_tmp, original.private_tmp);
    CHECK_EQ(parsed.private_home, original.private_home);
    CHECK_EQ(parsed.cwd, original.cwd);
    CHECK_EQ(parsed.command, original.command);

    CHECK_EQ(parsed.mounts.size(), original.mounts.size());
    CHECK_EQ(parsed.mounts[0].host, std::string("/src"));
    CHECK(parsed.mounts[0].storage == Storage::Clone);
    CHECK_EQ(parsed.mounts[0].primary, true);
    CHECK_EQ(parsed.mounts[0].clone_name, std::string("src"));
    CHECK(parsed.mounts[2].access == Access::Deny);

    CHECK_EQ(*parsed.limits.memory_bytes, *original.limits.memory_bytes);
    CHECK_EQ(*parsed.limits.cpu_multiple, *original.limits.cpu_multiple);
    CHECK_EQ(*parsed.limits.max_processes, *original.limits.max_processes);
    CHECK_EQ(*parsed.limits.file_bytes, *original.limits.file_bytes);
    CHECK_EQ(parsed.limits.core_dumps, true);
    CHECK_EQ(parsed.limits.background_priority, true);

    CHECK_EQ(parsed.env.passthrough, original.env.passthrough);
    CHECK_EQ(parsed.env.set.size(), size_t(2));
    CHECK_EQ(parsed.env.set[0].first, std::string("CC"));
    CHECK_EQ(parsed.credentials.user, std::string("nobody"));
    CHECK_EQ(parsed.credentials.block_setuid_exec, false);
    CHECK_EQ(parsed.credentials.umask, uint32_t(0022));
}

TEST(spec, json_defaults_are_safe_when_fields_are_missing) {
    // A capsule.json written by an older build must not silently become
    // more permissive than it was.
    json::Value minimal = json::parse(R"({"id":"0123456789ab","command":["/usr/bin/true"]})");
    Spec parsed = Spec::from_json(minimal);
    CHECK(parsed.network == Network::None);
    CHECK(parsed.services == ServiceBundle::Minimal);
    CHECK_EQ(parsed.private_tmp, true);
    CHECK_EQ(parsed.private_home, true);
    CHECK_EQ(parsed.permissive_metadata, false);
    CHECK_EQ(parsed.credentials.block_setuid_exec, true);
    CHECK_EQ(parsed.ephemeral, false);
    CHECK(!parsed.limits.any());
}

TEST(spec, json_rejects_an_unknown_format_version) {
    json::Value future = json::parse(
        R"({"format_version":99,"id":"0123456789ab","command":["/usr/bin/true"]})");
    CHECK_THROWS(Spec::from_json(future));
}

TEST(spec, json_rejects_negative_limit_values) {
    json::Value bad = json::parse(
        R"({"id":"0123456789ab","command":["/x"],"limits":{"memory_bytes":-1}})");
    CHECK_THROWS(Spec::from_json(bad));
}

TEST(spec, primary_mount_lookup) {
    Spec spec = minimal_spec();
    CHECK_EQ(spec.primary_mount(), nullptr);

    Mount clone = make_mount("/src", Access::ReadWrite, Storage::Clone);
    clone.primary = true;
    spec.mounts.push_back(clone);
    CHECK(spec.primary_mount() != nullptr);
    CHECK_EQ(spec.primary_mount()->host, std::string("/src"));
}

TEST(spec, limits_report_how_they_are_enforced) {
    Limits limits;
    CHECK(!limits.any());

    limits.cpu_seconds = 60;
    CHECK(limits.any_kernel_enforced());
    CHECK(!limits.any_supervised());

    limits.memory_bytes = 1ull << 30;
    CHECK(limits.any_supervised());
}
