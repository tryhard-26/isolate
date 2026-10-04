#include "core/ids.h"

#include "util/error.h"
#include "util/path.h"

#include <cctype>
#include <cstdlib>
#include <string>

namespace isolate {
namespace {

// Crockford-style base32 without the characters that read ambiguously in a
// terminal (i, l, o, u), so an identifier can be retyped from a screenshot.
constexpr std::string_view kAlphabet = "0123456789abcdefghjkmnpqrstvwxyz";
constexpr size_t kIdLength = 12;

} // namespace

std::string generate_capsule_id() {
    std::string id;
    id.reserve(kIdLength);
    for (size_t i = 0; i < kIdLength; ++i) {
        // arc4random_uniform is bias-free and needs no seeding or error handling.
        id += kAlphabet[arc4random_uniform(static_cast<uint32_t>(kAlphabet.size()))];
    }
    return id;
}

bool is_valid_capsule_id(std::string_view id) {
    if (id.size() != kIdLength) return false;
    for (char c : id) {
        if (kAlphabet.find(c) == std::string_view::npos) return false;
    }
    return true;
}

bool is_valid_capsule_name(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    if (name.front() == '.' || name.front() == '-') return false;
    for (char c : name) {
        bool ok = std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '_' ||
                  c == '-';
        if (!ok) return false;
    }
    // A name that looks like an identifier would make `isolate ps <x>` ambiguous.
    return !is_valid_capsule_id(name);
}

bool is_valid_snapshot_name(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    if (name.front() == '.' || name.front() == '-') return false;
    for (char c : name) {
        bool ok = std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '_' ||
                  c == '-';
        if (!ok) return false;
    }
    return true;
}

std::string name_from_path(std::string_view p) {
    std::string base = path::basename(p);
    std::string out;
    for (char c : base) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '_' ||
            c == '-') {
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (!out.empty() && out.back() != '-') {
            out += '-';
        }
    }
    while (!out.empty() && (out.back() == '-' || out.back() == '.')) out.pop_back();
    while (!out.empty() && (out.front() == '-' || out.front() == '.')) out.erase(out.begin());
    if (out.size() > 64) out.resize(64);
    if (!is_valid_capsule_name(out)) return {};
    return out;
}

} // namespace isolate
