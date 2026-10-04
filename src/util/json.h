#pragma once

#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace isolate::json {

// Capsule metadata is JSON so the on-disk state stays readable
// w/ standard tools. Docs are a few KB; speed is irrelevant.
class Value;

using Object = std::map<std::string, Value>;
using Array = std::vector<Value>;

class Value {
public:
    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : v_(b) {}
    Value(int64_t i) : v_(i) {}
    Value(int i) : v_(static_cast<int64_t>(i)) {}
    Value(uint64_t u) : v_(static_cast<int64_t>(u)) {}
    Value(double d) : v_(d) {}
    Value(const char* s) : v_(std::string(s)) {}
    Value(std::string s) : v_(std::move(s)) {}
    Value(Array a) : v_(std::move(a)) {}
    Value(Object o) : v_(std::move(o)) {}

    bool is_null() const { return std::holds_alternative<std::monostate>(v_); }
    bool is_bool() const { return std::holds_alternative<bool>(v_); }
    bool is_int() const { return std::holds_alternative<int64_t>(v_); }
    bool is_double() const { return std::holds_alternative<double>(v_); }
    bool is_number() const { return is_int() || is_double(); }
    bool is_string() const { return std::holds_alternative<std::string>(v_); }
    bool is_array() const { return std::holds_alternative<Array>(v_); }
    bool is_object() const { return std::holds_alternative<Object>(v_); }

    // Throw on type mismatch, so a corrupt capsule.json is
    // reported rather than silently defaulted.
    bool as_bool() const;
    int64_t as_int() const;
    double as_double() const;
    const std::string& as_string() const;
    const Array& as_array() const;
    const Object& as_object() const;

    const Value* get(std::string_view key) const; // nullptr if absent
    const Value& at(std::string_view key) const;  // throws

    // With a fallback, for fields added in later versions.
    bool get_bool(std::string_view key, bool fallback) const;
    int64_t get_int(std::string_view key, int64_t fallback) const;
    double get_double(std::string_view key, double fallback) const;
    std::string get_string(std::string_view key, std::string fallback) const;

    Array& array_ref();
    Object& object_ref();

private:
    std::variant<std::monostate, bool, int64_t, double, std::string, Array, Object> v_;
};

Value parse(std::string_view text);
std::string dump(const Value& v, int indent = 2);

} // namespace isolate::json
