#pragma once

#include "core/spec.h"

#include <string>
#include <vector>

namespace isolate::sb {

// Mach names a capsule may look up, by bundle.
//
// On macOS most privileged ops are a msg to a service (WindowServer,
// keychain, TCC, Spotlight), so fs limits w/o a closed bootstrap ns
// aren't isolation. Capsules start closed and get back only their
// bundle's names. A missing one fails w/ a lookup error naming it;
// add it w/ --service NAME.
std::vector<std::string> bundle_services(ServiceBundle bundle);

// DNS/TLS daemons. Separate so net:none can't reach them either.
std::vector<std::string> network_services();

// Bundle + net + extras, sorted, deduped.
std::vector<std::string> services_for(const Spec& spec);

const char* bundle_description(ServiceBundle bundle);

} // namespace isolate::sb
