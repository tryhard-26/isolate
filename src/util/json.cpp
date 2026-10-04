#include "util/json.h"

#include "util/error.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace isolate::json {
namespace {

const char* type_name(const Value& v) {
    if (v.is_null()) return "null";
    if (v.is_bool()) return "boolean";
    if (v.is_int() || v.is_double()) return "number";
    if (v.is_string()) return "string";
    if (v.is_array()) return "array";
    return "object";
}

[[noreturn]] void wrong_type(const Value& v, const char* want) {
    throw Error(std::string("json: expected ") + want + ", found " + type_name(v));
}

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    Value parse_document() {
        skip_ws();
        Value v = parse_value(0);
        skip_ws();
        if (pos_ != s_.size()) fail("trailing data after value");
        return v;
    }

private:
    static constexpr int kMaxDepth = 64;

    std::string_view s_;
    size_t pos_ = 0;

    [[noreturn]] void fail(const std::string& why) const {
        throw Error("json: " + why + " at offset " + std::to_string(pos_));
    }

    void skip_ws() {
        while (pos_ < s_.size() &&
               (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) {
            ++pos_;
        }
    }

    char peek() const {
        if (pos_ >= s_.size()) fail("unexpected end of input");
        return s_[pos_];
    }

    void expect(char c) {
        if (pos_ >= s_.size() || s_[pos_] != c) {
            fail(std::string("expected '") + c + "'");
        }
        ++pos_;
    }

    bool literal(std::string_view word) {
        if (s_.compare(pos_, word.size(), word) == 0) {
            pos_ += word.size();
            return true;
        }
        return false;
    }

    Value parse_value(int depth) {
        if (depth > kMaxDepth) fail("nesting too deep");
        switch (peek()) {
            case '{': return parse_object(depth);
            case '[': return parse_array(depth);
            case '"': return Value(parse_string());
            case 't':
                if (literal("true")) return Value(true);
                fail("bad literal");
            case 'f':
                if (literal("false")) return Value(false);
                fail("bad literal");
            case 'n':
                if (literal("null")) return Value(nullptr);
                fail("bad literal");
            default: return parse_number();
        }
    }

    Value parse_object(int depth) {
        expect('{');
        Object obj;
        skip_ws();
        if (peek() == '}') {
            ++pos_;
            return Value(std::move(obj));
        }
        while (true) {
            skip_ws();
            std::string key = parse_string();
            skip_ws();
            expect(':');
            skip_ws();
            obj.insert_or_assign(std::move(key), parse_value(depth + 1));
            skip_ws();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            expect('}');
            return Value(std::move(obj));
        }
    }

    Value parse_array(int depth) {
        expect('[');
        Array arr;
        skip_ws();
        if (peek() == ']') {
            ++pos_;
            return Value(std::move(arr));
        }
        while (true) {
            skip_ws();
            arr.push_back(parse_value(depth + 1));
            skip_ws();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            expect(']');
            return Value(std::move(arr));
        }
    }

    // \uXXXX incl. surrogate pairs -> UTF-8.
    void append_utf8(uint32_t cp, std::string& out) {
        if (cp <= 0x7F) {
            out += static_cast<char>(cp);
        } else if (cp <= 0x7FF) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp <= 0xFFFF) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    uint32_t parse_hex4() {
        if (pos_ + 4 > s_.size()) fail("truncated \\u escape");
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else fail("bad hex digit in \\u escape");
        }
        return v;
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        while (true) {
            if (pos_ >= s_.size()) fail("unterminated string");
            char c = s_[pos_++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) fail("control character in string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= s_.size()) fail("unterminated escape");
            char e = s_[pos_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp = parse_hex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 1 < s_.size() &&
                        s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                        size_t save = pos_;
                        pos_ += 2;
                        uint32_t low = parse_hex4();
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        } else {
                            pos_ = save;
                        }
                    }
                    append_utf8(cp, out);
                    break;
                }
                default: fail("unknown escape");
            }
        }
    }

    Value parse_number() {
        size_t start = pos_;
        if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) ++pos_;
        bool is_float = false;
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (std::isdigit(static_cast<unsigned char>(c))) {
                ++pos_;
            } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                is_float = true;
                ++pos_;
            } else {
                break;
            }
        }
        if (pos_ == start) fail("expected a value");
        std::string text(s_.substr(start, pos_ - start));
        errno = 0;
        if (!is_float) {
            char* end = nullptr;
            long long v = std::strtoll(text.c_str(), &end, 10);
            if (end == text.c_str() + text.size() && errno != ERANGE) {
                return Value(static_cast<int64_t>(v));
            }
        }
        char* end = nullptr;
        double d = std::strtod(text.c_str(), &end);
        if (end != text.c_str() + text.size()) fail("bad number '" + text + "'");
        return Value(d);
    }
};

void dump_string(const std::string& s, std::string& out) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

void dump_value(const Value& v, int indent, int level, std::string& out) {
    auto newline = [&](int lv) {
        if (indent <= 0) return;
        out += '\n';
        out.append(static_cast<size_t>(indent * lv), ' ');
    };

    if (v.is_null()) {
        out += "null";
    } else if (v.is_bool()) {
        out += v.as_bool() ? "true" : "false";
    } else if (v.is_int()) {
        out += std::to_string(v.as_int());
    } else if (v.is_double()) {
        double d = v.as_double();
        char buf[40];
        if (!std::isfinite(d)) {
            // No inf/NaN in JSON; 0 beats invalid output.
            out += '0';
        } else {
            std::snprintf(buf, sizeof buf, "%.17g", d);
            out += buf;
        }
    } else if (v.is_string()) {
        dump_string(v.as_string(), out);
    } else if (v.is_array()) {
        const Array& a = v.as_array();
        if (a.empty()) {
            out += "[]";
            return;
        }
        out += '[';
        for (size_t i = 0; i < a.size(); ++i) {
            if (i) out += ',';
            newline(level + 1);
            dump_value(a[i], indent, level + 1, out);
        }
        newline(level);
        out += ']';
    } else {
        const Object& o = v.as_object();
        if (o.empty()) {
            out += "{}";
            return;
        }
        out += '{';
        bool first = true;
        for (const auto& [key, val] : o) {
            if (!first) out += ',';
            first = false;
            newline(level + 1);
            dump_string(key, out);
            out += ':';
            if (indent > 0) out += ' ';
            dump_value(val, indent, level + 1, out);
        }
        newline(level);
        out += '}';
    }
}

} // namespace

bool Value::as_bool() const {
    if (!is_bool()) wrong_type(*this, "boolean");
    return std::get<bool>(v_);
}

int64_t Value::as_int() const {
    if (is_int()) return std::get<int64_t>(v_);
    if (is_double()) {
        double d = std::get<double>(v_);
        if (std::floor(d) == d && std::abs(d) < 9.0e15) return static_cast<int64_t>(d);
    }
    wrong_type(*this, "integer");
}

double Value::as_double() const {
    if (is_double()) return std::get<double>(v_);
    if (is_int()) return static_cast<double>(std::get<int64_t>(v_));
    wrong_type(*this, "number");
}

const std::string& Value::as_string() const {
    if (!is_string()) wrong_type(*this, "string");
    return std::get<std::string>(v_);
}

const Array& Value::as_array() const {
    if (!is_array()) wrong_type(*this, "array");
    return std::get<Array>(v_);
}

const Object& Value::as_object() const {
    if (!is_object()) wrong_type(*this, "object");
    return std::get<Object>(v_);
}

const Value* Value::get(std::string_view key) const {
    if (!is_object()) return nullptr;
    const Object& o = std::get<Object>(v_);
    auto it = o.find(std::string(key));
    return it == o.end() ? nullptr : &it->second;
}

const Value& Value::at(std::string_view key) const {
    const Value* v = get(key);
    if (!v) throw Error("json: missing field '" + std::string(key) + "'");
    return *v;
}

bool Value::get_bool(std::string_view key, bool fallback) const {
    const Value* v = get(key);
    return v && !v->is_null() ? v->as_bool() : fallback;
}

int64_t Value::get_int(std::string_view key, int64_t fallback) const {
    const Value* v = get(key);
    return v && !v->is_null() ? v->as_int() : fallback;
}

double Value::get_double(std::string_view key, double fallback) const {
    const Value* v = get(key);
    return v && !v->is_null() ? v->as_double() : fallback;
}

std::string Value::get_string(std::string_view key, std::string fallback) const {
    const Value* v = get(key);
    return v && !v->is_null() ? v->as_string() : fallback;
}

Array& Value::array_ref() {
    if (!is_array()) v_ = Array{};
    return std::get<Array>(v_);
}

Object& Value::object_ref() {
    if (!is_object()) v_ = Object{};
    return std::get<Object>(v_);
}

Value parse(std::string_view text) {
    return Parser(text).parse_document();
}

std::string dump(const Value& v, int indent) {
    std::string out;
    dump_value(v, indent, 0, out);
    if (indent > 0) out += '\n';
    return out;
}

} // namespace isolate::json
