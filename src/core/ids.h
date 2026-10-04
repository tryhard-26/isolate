#pragma once

#include <string>
#include <string_view>

namespace isolate {

// 12 base32 chars from arc4random, e.g. "k7mq2z4ptb9x".
// Goes in a Mach name, so no chars needing escaping. Must be
// unguessable: guessing one lets a capsule probe for another.
std::string generate_capsule_id();

// Check before touching the fs, so a hostile arg can't traverse.
bool is_valid_capsule_id(std::string_view id);

// 1-64 of [A-Za-z0-9._-], not leading '.' or '-'.
bool is_valid_capsule_name(std::string_view name);

// "/Users/x/my project" -> "my-project". Empty if unusable.
std::string name_from_path(std::string_view p);

bool is_valid_snapshot_name(std::string_view name);

} // namespace isolate
