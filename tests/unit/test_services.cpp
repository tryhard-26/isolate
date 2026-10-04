#include "harness.h"

#include "sb/services.h"

#include <algorithm>
#include <set>

using namespace isolate;
using namespace isolate::sb;

namespace {

bool has(const std::vector<std::string>& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

bool is_sorted_and_unique(const std::vector<std::string>& names) {
    if (!std::is_sorted(names.begin(), names.end())) return false;
    return std::set<std::string>(names.begin(), names.end()).size() == names.size();
}

} // namespace

TEST(services, none_is_empty) {
    CHECK_EQ(bundle_services(ServiceBundle::None).size(), size_t(0));
}

TEST(services, minimal_covers_what_libsystem_needs) {
    std::vector<std::string> names = bundle_services(ServiceBundle::Minimal);
    CHECK(!names.empty());
    // These three are what a plain C program needs to log, read preferences,
    // and resolve its own user.
    CHECK(has(names, "com.apple.logd"));
    CHECK(has(names, "com.apple.cfprefsd.agent"));
    CHECK(has(names, "com.apple.system.opendirectoryd.libinfo"));
}

TEST(services, bundles_are_cumulative) {
    std::vector<std::string> minimal = bundle_services(ServiceBundle::Minimal);
    std::vector<std::string> development = bundle_services(ServiceBundle::Development);
    std::vector<std::string> graphical = bundle_services(ServiceBundle::Graphical);

    for (const std::string& name : minimal) CHECK(has(development, name));
    for (const std::string& name : development) CHECK(has(graphical, name));
    CHECK(development.size() > minimal.size());
    CHECK(graphical.size() > development.size());
}

TEST(services, each_bundle_is_sorted_and_free_of_duplicates) {
    for (ServiceBundle bundle : {ServiceBundle::None, ServiceBundle::Minimal,
                                 ServiceBundle::Development, ServiceBundle::Graphical}) {
        CHECK(is_sorted_and_unique(bundle_services(bundle)));
    }
    CHECK(is_sorted_and_unique(network_services()));
}

TEST(services, minimal_does_not_include_the_window_server) {
    // A capsule that can reach the window server can draw over other windows
    // and read the pasteboard, so it must not be in the default bundle.
    std::vector<std::string> names = bundle_services(ServiceBundle::Minimal);
    CHECK(!has(names, "com.apple.windowserver.active"));
    CHECK(!has(names, "com.apple.pasteboard.1"));
}

TEST(services, minimal_does_not_include_networking_daemons) {
    std::vector<std::string> names = bundle_services(ServiceBundle::Minimal);
    CHECK(!has(names, "com.apple.networkd"));
    CHECK(!has(names, "com.apple.dnssd.service"));
}

TEST(services, networking_services_are_added_only_with_networking) {
    Spec spec;
    spec.services = ServiceBundle::Minimal;
    spec.network = Network::None;
    CHECK(!has(services_for(spec), "com.apple.dnssd.service"));

    spec.network = Network::Outbound;
    CHECK(has(services_for(spec), "com.apple.dnssd.service"));

    spec.network = Network::All;
    CHECK(has(services_for(spec), "com.apple.dnssd.service"));
}

TEST(services, extra_services_are_merged_and_deduplicated) {
    Spec spec;
    spec.services = ServiceBundle::Minimal;
    spec.extra_services = {"com.example.one", "com.example.one", "com.apple.logd"};

    std::vector<std::string> names = services_for(spec);
    CHECK(has(names, "com.example.one"));
    CHECK(is_sorted_and_unique(names));
    CHECK_EQ(std::count(names.begin(), names.end(), "com.apple.logd"), 1);
}

TEST(services, service_lists_for_bundle_none_contain_only_extras) {
    Spec spec;
    spec.services = ServiceBundle::None;
    spec.network = Network::None;
    spec.extra_services = {"com.example.only"};
    std::vector<std::string> names = services_for(spec);
    CHECK_EQ(names.size(), size_t(1));
    CHECK_EQ(names[0], std::string("com.example.only"));
}

TEST(services, every_bundle_has_a_description) {
    for (ServiceBundle bundle : {ServiceBundle::None, ServiceBundle::Minimal,
                                 ServiceBundle::Development, ServiceBundle::Graphical}) {
        CHECK(std::string(bundle_description(bundle)).size() > 10);
    }
}

TEST(services, names_look_like_mach_service_names) {
    // A name with a space or a quote would have to be escaped into the profile,
    // and nothing in these lists should need escaping.
    for (ServiceBundle bundle : {ServiceBundle::Minimal, ServiceBundle::Development,
                                 ServiceBundle::Graphical}) {
        for (const std::string& name : bundle_services(bundle)) {
            CHECK(name.find(' ') == std::string::npos);
            CHECK(name.find('"') == std::string::npos);
            CHECK(name.find('\\') == std::string::npos);
            CHECK(!name.empty());
        }
    }
}
