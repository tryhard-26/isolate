#include "sb/services.h"

#include <algorithm>

namespace isolate::sb {
namespace {

// Needed by anything linked against libSystem. Without these, a plain C program
// that calls getpwuid, localtime, or os_log fails or hangs.
const char* const kMinimal[] = {
    "com.apple.bsd.dirhelper",                       // confstr(_CS_DARWIN_USER_TEMP_DIR)
    "com.apple.cfprefsd.agent",                      // CFPreferences, per-user
    "com.apple.cfprefsd.daemon",                     // CFPreferences, system
    "com.apple.diagnosticd",                         // os_log
    "com.apple.logd",                                // os_log
    "com.apple.logd.events",                         // os_log
    "com.apple.system.DirectoryService.libinfo_v1",  // getpwnam and friends
    "com.apple.system.libinfo.muser",                // user lookup on recent systems
    "com.apple.system.logger",                       // syslog
    "com.apple.system.notification_center",          // libnotify
    "com.apple.system.opendirectoryd.libinfo",       // getpwuid, getgrgid
    "com.apple.system.opendirectoryd.membership",    // group membership
};

// Adds what command-line development tools expect: LaunchServices (used by
// anything that resolves a file's type or an application), the Core Services
// daemon, file-system event notification, and the Xcode toolchain's helpers.
const char* const kDevelopment[] = {
    "com.apple.CoreServices.coreservicesd",
    "com.apple.FSEvents",
    "com.apple.PowerManagement.control",
    "com.apple.SecurityServer",             // keychain access for code signing
    "com.apple.SystemConfiguration.configd",
    "com.apple.coreservices.launchservicesd",
    "com.apple.coreservices.quarantine-resolver",
    "com.apple.dt.automationmode.reader",
    "com.apple.lsd.mapdb",
    "com.apple.lsd.modifydb",
    "com.apple.secinitd",                   // sandbox container setup for child processes
    "com.apple.trustd",                     // certificate evaluation
    "com.apple.trustd.agent",
    "com.apple.xpc.activity.unmanaged",
};

// Enough for a windowed application. This bundle gives the capsule access to
// the window server and the pasteboard, which means the capsule can draw on
// screen, read the clipboard, and send events. Those are not isolated from the
// rest of the session, so this bundle trades away a large part of the boundary.
const char* const kGraphical[] = {
    "com.apple.CARenderServer",
    "com.apple.CoreDisplay.master",
    "com.apple.audio.AudioComponentRegistrar",
    "com.apple.audio.coreaudiod",
    "com.apple.cvmsServ",
    "com.apple.fonts",
    "com.apple.pasteboard.1",
    "com.apple.tsm.portname",
    "com.apple.tsm.uikit.portname",
    "com.apple.windowserver.active",
};

const char* const kNetwork[] = {
    "com.apple.AppSSO.service-xpc",
    "com.apple.SystemConfiguration.SCNetworkReachability",
    "com.apple.SystemConfiguration.configd",
    "com.apple.cfnetwork.cfnetworkagent",
    "com.apple.dnssd.service",
    "com.apple.nehelper",
    "com.apple.nesessionmanager",
    "com.apple.networkd",
    "com.apple.symptomsd",
    "com.apple.trustd",
    "com.apple.trustd.agent",
    "com.apple.usymptomsd",
};

template <size_t N>
void append(std::vector<std::string>& out, const char* const (&names)[N]) {
    out.insert(out.end(), names, names + N);
}

} // namespace

std::vector<std::string> bundle_services(ServiceBundle bundle) {
    std::vector<std::string> out;
    switch (bundle) {
        case ServiceBundle::None:
            break;
        case ServiceBundle::Graphical:
            append(out, kGraphical);
            [[fallthrough]];
        case ServiceBundle::Development:
            append(out, kDevelopment);
            [[fallthrough]];
        case ServiceBundle::Minimal:
            append(out, kMinimal);
            break;
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<std::string> network_services() {
    std::vector<std::string> out;
    append(out, kNetwork);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<std::string> services_for(const Spec& spec) {
    std::vector<std::string> out = bundle_services(spec.services);
    if (spec.network != Network::None) {
        std::vector<std::string> net = network_services();
        out.insert(out.end(), net.begin(), net.end());
    }
    out.insert(out.end(), spec.extra_services.begin(), spec.extra_services.end());
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

const char* bundle_description(ServiceBundle bundle) {
    switch (bundle) {
        case ServiceBundle::None:
            return "no Mach services; only a static binary that makes no library calls "
                   "beyond raw system calls will run";
        case ServiceBundle::Minimal:
            return "the services libSystem needs: user lookup, preferences, and logging";
        case ServiceBundle::Development:
            return "minimal, plus LaunchServices, Core Services, file-system events, and "
                   "the keychain access that code signing needs";
        case ServiceBundle::Graphical:
            return "development, plus the window server, pasteboard, and audio; a capsule "
                   "with this bundle can draw on screen and read the clipboard";
    }
    return "";
}

} // namespace isolate::sb
