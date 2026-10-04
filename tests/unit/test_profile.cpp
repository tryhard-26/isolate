#include "harness.h"

#include "core/ids.h"
#include "sb/apply.h"
#include "sb/profile.h"
#include "sb/tag.h"
#include "util/error.h"
#include "util/str.h"

#include <algorithm>

using namespace isolate;

namespace {

Spec base_spec() {
    Spec spec;
    spec.id = "0123456789ab";
    spec.command = {"/usr/bin/true"};
    return spec;
}

sb::ProfileInputs inputs_for(const Spec& spec) {
    sb::ProfileInputs inputs;
    inputs.spec = &spec;
    inputs.capsule_dir = "/capsules/0123456789ab";
    inputs.capsule_tmp = spec.private_tmp ? "/capsules/0123456789ab/tmp" : "";
    inputs.capsule_home = spec.private_home ? "/capsules/0123456789ab/home" : "";
    inputs.membership_name = sb::membership_name(spec.id);
    inputs.program_path = "/usr/bin/true";
    return inputs;
}

// The index of the first line matching `needle`, or npos.
size_t line_of(const std::string& profile, const std::string& needle) {
    std::vector<std::string> lines = str::split(profile, '\n');
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].find(needle) != std::string::npos) return i;
    }
    return std::string::npos;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

TEST(profile, starts_from_deny_default) {
    Spec spec = base_spec();
    std::string profile = sb::build_profile(inputs_for(spec));
    CHECK(contains(profile, "(version 1)"));
    CHECK(contains(profile, "(deny default"));
    // The deny must come before anything that allows, or it would override it.
    CHECK(line_of(profile, "(deny default") < line_of(profile, "(allow"));
}

TEST(profile, is_deterministic_for_the_same_input) {
    Spec spec = base_spec();
    spec.mounts.push_back({"/b", Access::ReadOnly, Storage::Host, false, "", "/b"});
    spec.mounts.push_back({"/a", Access::ReadWrite, Storage::Host, false, "", "/a"});
    CHECK_EQ(sb::build_profile(inputs_for(spec)), sb::build_profile(inputs_for(spec)));
}

TEST(profile, the_kernel_accepts_a_generated_profile) {
    // The definitive check: the sandbox compiler in the running kernel has to
    // accept what the generator produces.
    Spec spec = base_spec();
    sb::ValidationResult result = sb::validate_profile(sb::build_profile(inputs_for(spec)));
    CHECK(result.ok);
    if (!result.ok) CHECK_EQ(result.error, std::string(""));
}

TEST(profile, the_kernel_accepts_every_policy_combination) {
    for (Network network : {Network::None, Network::Outbound, Network::All}) {
        for (ServiceBundle bundle : {ServiceBundle::None, ServiceBundle::Minimal,
                                     ServiceBundle::Development, ServiceBundle::Graphical}) {
            Spec spec = base_spec();
            spec.network = network;
            spec.services = bundle;
            spec.permissive_metadata = true;
            spec.limits.core_dumps = true;
            spec.mounts.push_back({"/private/tmp/a", Access::ReadWrite, Storage::Host, false,
                                   "", "/private/tmp/a"});
            spec.mounts.push_back({"/private/tmp/a/deep", Access::Deny, Storage::Host, false,
                                   "", "/private/tmp/a/deep"});
            spec.mounts.push_back({"/private/tmp/b", Access::ReadWrite, Storage::Clone, true,
                                   "b", "/capsules/0123456789ab/layers/b"});
            sb::ProfileInputs inputs = inputs_for(spec);
            inputs.tty_path = "/dev/ttys000";
            sb::ValidationResult result = sb::validate_profile(sb::build_profile(inputs));
            CHECK(result.ok);
            if (!result.ok) CHECK_EQ(result.error, std::string(""));
        }
    }
}

TEST(profile, mount_rules_are_ordered_by_depth) {
    // Ordering is what makes a nested rule win, because the sandbox breaks ties
    // between rules naming the same operations by taking the last one.
    Spec spec = base_spec();
    spec.mounts.push_back({"/a/b/c", Access::Deny, Storage::Host, false, "", "/a/b/c"});
    spec.mounts.push_back({"/a", Access::ReadOnly, Storage::Host, false, "", "/a"});
    spec.mounts.push_back({"/a/b", Access::ReadWrite, Storage::Host, false, "", "/a/b"});

    std::string profile = sb::build_profile(inputs_for(spec));
    size_t shallow = line_of(profile, "\"/a\")");
    size_t middle = line_of(profile, "\"/a/b\")");
    size_t deep = line_of(profile, "\"/a/b/c\")");
    CHECK(shallow < middle);
    CHECK(middle < deep);
}

TEST(profile, every_path_rule_names_the_same_operations) {
    // This is the property the whole ordering scheme depends on. If a rule used
    // file-read* while another used file*, the narrower token would win over
    // whichever rule came last and a --deny could be silently ignored.
    Spec spec = base_spec();
    spec.mounts.push_back({"/a", Access::ReadOnly, Storage::Host, false, "", "/a"});
    spec.mounts.push_back({"/a/w", Access::ReadWrite, Storage::Host, false, "", "/a/w"});
    spec.mounts.push_back({"/a/d", Access::Deny, Storage::Host, false, "", "/a/d"});

    std::string profile = sb::build_profile(inputs_for(spec));
    CHECK(!contains(profile, "file*"));
    CHECK(!contains(profile, "file-read*"));
    CHECK(!contains(profile, "file-write*"));
    CHECK(contains(profile, "file-read-data"));
    CHECK(contains(profile, "file-write-data"));
}

TEST(profile, read_only_mounts_grant_no_write_operations) {
    Spec spec = base_spec();
    spec.mounts.push_back({"/ro", Access::ReadOnly, Storage::Host, false, "", "/ro"});
    std::string profile = sb::build_profile(inputs_for(spec));

    // Find the rule that mentions "/ro" and check it is not a write grant.
    std::vector<std::string> lines = str::split(profile, '\n');
    size_t index = line_of(profile, "(subpath \"/ro\")");
    CHECK(index != std::string::npos);
    // Walk back to the opening of the rule.
    std::string rule_head;
    for (size_t i = index; i-- > 0;) {
        if (str::starts_with(lines[i], "(allow") || str::starts_with(lines[i], "(deny")) {
            rule_head = lines[i];
            break;
        }
    }
    CHECK(contains(rule_head, "allow"));
    CHECK(contains(rule_head, "file-read-data"));
    CHECK(!contains(rule_head, "file-write-data"));
}

TEST(profile, denied_mounts_use_the_host_path) {
    Spec spec = base_spec();
    spec.mounts.push_back({"/private/secret", Access::Deny, Storage::Host, false, "",
                           "/private/secret"});
    std::string profile = sb::build_profile(inputs_for(spec));
    CHECK(contains(profile, "(deny"));
    CHECK(contains(profile, "\"/private/secret\""));
}

TEST(profile, cloned_mounts_grant_the_layer_and_deny_the_original) {
    Spec spec = base_spec();
    spec.mounts.push_back({"/work", Access::ReadWrite, Storage::Clone, true, "work",
                           "/capsules/0123456789ab/layers/work"});
    std::string profile = sb::build_profile(inputs_for(spec));

    CHECK(contains(profile, "\"/capsules/0123456789ab/layers/work\""));
    // The original must be withheld, so that a read-only mount of an enclosing
    // directory cannot be used to reach the files the clone exists to protect.
    size_t deny_line = line_of(profile, "(subpath \"/work\")");
    CHECK(deny_line != std::string::npos);
    std::vector<std::string> lines = str::split(profile, '\n');
    std::string head;
    for (size_t i = deny_line; i-- > 0;) {
        if (str::starts_with(lines[i], "(allow") || str::starts_with(lines[i], "(deny")) {
            head = lines[i];
            break;
        }
    }
    CHECK(contains(head, "deny"));
}

TEST(profile, the_capsule_cannot_reach_its_own_metadata) {
    Spec spec = base_spec();
    std::string profile = sb::build_profile(inputs_for(spec));

    // Only tmp and home are granted inside the capsule directory. Nothing may
    // grant the capsule directory itself, or a program could rewrite the policy
    // and the baseline that isolate reports against.
    CHECK(contains(profile, "\"/capsules/0123456789ab/tmp\""));
    CHECK(contains(profile, "\"/capsules/0123456789ab/home\""));
    CHECK(!contains(profile, "(subpath \"/capsules/0123456789ab\")"));
    CHECK(!contains(profile, "capsule.json"));
    CHECK(!contains(profile, "\"/capsules/0123456789ab/base\""));
    CHECK(!contains(profile, "\"/capsules/0123456789ab/snapshots\""));
}

TEST(profile, network_none_grants_nothing_outbound) {
    Spec spec = base_spec();
    spec.network = Network::None;
    std::string profile = sb::build_profile(inputs_for(spec));

    CHECK(!contains(profile, "(allow network-outbound)"));
    CHECK(!contains(profile, "(allow network*)"));
    // The syslog socket and the capsule's own scratch directory are the only
    // network-outbound grants, and both are paths rather than addresses.
    CHECK(contains(profile, "/private/var/run/syslog"));
}

TEST(profile, network_outbound_denies_listening) {
    Spec spec = base_spec();
    spec.network = Network::Outbound;
    std::string profile = sb::build_profile(inputs_for(spec));
    CHECK(contains(profile, "(allow network-outbound)"));
    CHECK(!contains(profile, "(allow network*)"));
    CHECK(!contains(profile, "(allow network-inbound)"));
}

TEST(profile, network_all_grants_everything) {
    Spec spec = base_spec();
    spec.network = Network::All;
    std::string profile = sb::build_profile(inputs_for(spec));
    CHECK(contains(profile, "(allow network*)"));
}

TEST(profile, networking_services_appear_only_when_networking_does) {
    Spec offline = base_spec();
    offline.services = ServiceBundle::Minimal;
    offline.network = Network::None;
    CHECK(!contains(sb::build_profile(inputs_for(offline)), "com.apple.dnssd.service"));

    Spec online = offline;
    online.network = Network::Outbound;
    CHECK(contains(sb::build_profile(inputs_for(online)), "com.apple.dnssd.service"));
}

TEST(profile, the_membership_rule_is_present_and_specific_to_the_capsule) {
    Spec spec = base_spec();
    std::string profile = sb::build_profile(inputs_for(spec));
    CHECK(contains(profile, "dev.isolate.capsule.0123456789ab"));

    Spec other = base_spec();
    other.id = "ba9876543210";
    CHECK(!contains(sb::build_profile(inputs_for(other)),
                    "dev.isolate.capsule.0123456789ab"));
}

TEST(profile, service_bundle_none_allows_no_lookups) {
    Spec spec = base_spec();
    spec.services = ServiceBundle::None;
    spec.network = Network::None;
    std::string profile = sb::build_profile(inputs_for(spec));
    CHECK(!contains(profile, "com.apple.logd"));
    // The membership marker is still there: it grants nothing.
    CHECK(contains(profile, "dev.isolate.capsule."));
}

TEST(profile, graphical_bundle_includes_the_window_server) {
    Spec spec = base_spec();
    spec.services = ServiceBundle::Graphical;
    std::string profile = sb::build_profile(inputs_for(spec));
    CHECK(contains(profile, "com.apple.windowserver.active"));
    // Bundles are cumulative, so the minimal services must still be there.
    CHECK(contains(profile, "com.apple.logd"));
}

TEST(profile, privilege_helpers_are_denied_after_the_tool_directories) {
    Spec spec = base_spec();
    std::string profile = sb::build_profile(inputs_for(spec));
    size_t tools = line_of(profile, "(subpath \"/usr/bin\")");
    size_t sudo = line_of(profile, "/usr/bin/sudo");
    CHECK(tools != std::string::npos);
    CHECK(sudo != std::string::npos);
    CHECK(tools < sudo);
}

TEST(profile, allow_setuid_removes_the_helper_denials) {
    Spec spec = base_spec();
    spec.credentials.block_setuid_exec = false;
    CHECK(!contains(sb::build_profile(inputs_for(spec)), "/usr/bin/sudo"));
}

TEST(profile, traversal_rules_name_ancestors_individually) {
    Spec spec = base_spec();
    spec.mounts.push_back({"/a/b/c", Access::ReadOnly, Storage::Host, false, "", "/a/b/c"});
    std::string profile = sb::build_profile(inputs_for(spec));

    // Metadata on the ancestors, as literals so that the grant does not extend
    // to their contents.
    CHECK(contains(profile, "(literal \"/a\")"));
    CHECK(contains(profile, "(literal \"/a/b\")"));
    CHECK(!contains(profile, "(subpath \"/a\")"));
}

TEST(profile, permissive_metadata_is_opt_in) {
    Spec spec = base_spec();
    CHECK(!contains(sb::build_profile(inputs_for(spec)), "(allow file-read-metadata)"));
    spec.permissive_metadata = true;
    CHECK(contains(sb::build_profile(inputs_for(spec)), "(allow file-read-metadata)"));
}

TEST(profile, the_terminal_is_granted_only_when_there_is_one) {
    Spec spec = base_spec();
    sb::ProfileInputs without = inputs_for(spec);
    CHECK(!contains(sb::build_profile(without), "/dev/tty"));

    sb::ProfileInputs with = inputs_for(spec);
    with.tty_path = "/dev/ttys003";
    std::string profile = sb::build_profile(with);
    CHECK(contains(profile, "/dev/ttys003"));
    // Not every terminal: only this one.
    CHECK(!contains(profile, "/dev/ttys004"));
}

TEST(profile, a_program_outside_the_mounts_is_granted_explicitly) {
    Spec spec = base_spec();
    sb::ProfileInputs inputs = inputs_for(spec);
    inputs.program_path = "/opt/custom/bin/tool";
    std::string profile = sb::build_profile(inputs);
    CHECK(contains(profile, "(literal \"/opt/custom/bin/tool\")"));
}

TEST(profile, a_program_inside_a_mount_needs_no_extra_rule) {
    Spec spec = base_spec();
    spec.mounts.push_back({"/opt/custom", Access::ReadOnly, Storage::Host, false, "",
                           "/opt/custom"});
    sb::ProfileInputs inputs = inputs_for(spec);
    inputs.program_path = "/opt/custom/bin/tool";
    std::string profile = sb::build_profile(inputs);
    CHECK(!contains(profile, "(literal \"/opt/custom/bin/tool\")"));
}

TEST(profile, denial_logging_follows_the_verbose_flag) {
    Spec spec = base_spec();
    sb::ProfileInputs quiet = inputs_for(spec);
    CHECK(contains(sb::build_profile(quiet), "(with no-log)"));

    sb::ProfileInputs loud = inputs_for(spec);
    loud.log_denials = true;
    std::string profile = sb::build_profile(loud);
    CHECK(contains(profile, "(debug deny)"));
    CHECK(!contains(profile, "(with no-log)"));
}

TEST(profile, paths_with_quotes_are_escaped) {
    Spec spec = base_spec();
    spec.mounts.push_back({"/private/tmp/odd\"name", Access::ReadOnly, Storage::Host, false,
                           "", "/private/tmp/odd\"name"});
    std::string profile = sb::build_profile(inputs_for(spec));
    CHECK(contains(profile, "odd\\\"name"));
    CHECK(sb::validate_profile(profile).ok);
}

TEST(profile, paths_with_newlines_are_refused) {
    // A newline would end the SBPL string and let the rest of the path be read
    // as profile source.
    Spec spec = base_spec();
    spec.mounts.push_back({"/private/tmp/a\nb", Access::ReadOnly, Storage::Host, false, "",
                           "/private/tmp/a\nb"});
    CHECK_THROWS(sb::build_profile(inputs_for(spec)));
}

TEST(profile, sections_split_on_headings) {
    Spec spec = base_spec();
    std::vector<std::string> sections = sb::profile_sections(sb::build_profile(inputs_for(spec)));
    CHECK(sections.size() > 5);
    bool found_network = false;
    for (const std::string& section : sections) {
        if (str::starts_with(section, ";; Network")) found_network = true;
    }
    CHECK(found_network);
}

TEST(profile, build_requires_a_spec) {
    sb::ProfileInputs inputs;
    CHECK_THROWS(sb::build_profile(inputs));
}
