#pragma once

// A small test harness. isolate has no third-party dependencies, including for
// tests, so that a clone builds and tests with nothing but CMake and clang.
//
//   TEST(json, parses_nested_objects) {
//       CHECK_EQ(json::parse("{\"a\":1}").at("a").as_int(), 1);
//   }
//
// Executables built from this header accept:
//   <exe>                 run every test
//   <exe> suite           run one suite
//   <exe> suite.name      run one test
//   <exe> --list          print test names

#include <exception>
#include <functional>
#include <sstream>
#include <utility>
#include <string>
#include <vector>

namespace isolate::test {

struct Failure {
    std::string message;
};

struct Case {
    std::string suite;
    std::string name;
    std::function<void()> fn;
    const char* file;
    int line;
};

// Returns the mutable global registry. Tests self-register before main runs.
std::vector<Case>& registry();

int run_all(int argc, char** argv);

[[noreturn]] void fail_at(const char* file, int line, const std::string& message);

// Scratch directory for a single test, removed when the test ends. Lives under
// the harness's own temporary root, never under the project or /tmp.
class TempDir {
public:
    TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir();

    const std::string& path() const { return path_; }
    std::string child(const std::string& relative) const;

    // Convenience builders used by the filesystem tests.
    void write(const std::string& relative, const std::string& contents) const;
    std::string read(const std::string& relative) const;
    void make_dir(const std::string& relative) const;

private:
    std::string path_;
};

// describe() renders a value for a failure message. The overloads cover what
// the tests actually compare: scalars, strings, and containers of them.
std::string describe(const std::string& value);
std::string describe(bool value);

template <typename T>
std::string describe(const T& value);

template <typename T>
std::string describe(const std::vector<T>& values);

template <typename A, typename B>
std::string describe(const std::pair<A, B>& value);

inline std::string describe(const std::string& value) {
    return "\"" + value + "\"";
}

inline std::string describe(bool value) {
    return value ? "true" : "false";
}

template <typename T>
std::string describe(const T& value) {
    std::ostringstream out;
    out << value;
    return out.str();
}

template <typename T>
std::string describe(const std::vector<T>& values) {
    std::string out = "[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out += ", ";
        out += describe(values[i]);
    }
    out += "]";
    return out;
}

template <typename A, typename B>
std::string describe(const std::pair<A, B>& value) {
    return "(" + describe(value.first) + ", " + describe(value.second) + ")";
}

struct Registrar {
    Registrar(const char* suite, const char* name, std::function<void()> fn, const char* file,
              int line) {
        registry().push_back(Case{suite, name, std::move(fn), file, line});
    }
};

} // namespace isolate::test

#define ISOLATE_CONCAT_(a, b) a##b
#define ISOLATE_CONCAT(a, b) ISOLATE_CONCAT_(a, b)

#define TEST(suite_name, test_name)                                                     \
    static void ISOLATE_CONCAT(isolate_test_fn_, __LINE__)();                           \
    static const ::isolate::test::Registrar ISOLATE_CONCAT(isolate_test_reg_, __LINE__)( \
        #suite_name, #test_name, &ISOLATE_CONCAT(isolate_test_fn_, __LINE__), __FILE__,  \
        __LINE__);                                                                      \
    static void ISOLATE_CONCAT(isolate_test_fn_, __LINE__)()

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            ::isolate::test::fail_at(__FILE__, __LINE__, "CHECK failed: " #cond);     \
        }                                                                             \
    } while (false)

// The operands are copied rather than bound by reference. An expression like
// parse(text).as_string() returns a reference into a temporary that would be
// destroyed at the end of the declaration, leaving the comparison reading freed
// stack. Copying costs nothing worth saving in a test and removes the trap.
#define CHECK_EQ(actual, expected)                                                      \
    do {                                                                                \
        auto isolate_actual_ = (actual);                                                \
        auto isolate_expected_ = (expected);                                            \
        if (!(isolate_actual_ == isolate_expected_)) {                                  \
            ::isolate::test::fail_at(                                                   \
                __FILE__, __LINE__,                                                     \
                std::string("CHECK_EQ failed: " #actual " == " #expected "\n    actual: ") + \
                    ::isolate::test::describe(isolate_actual_) + "\n  expected: " +     \
                    ::isolate::test::describe(isolate_expected_));                      \
        }                                                                               \
    } while (false)

#define CHECK_NE(actual, unexpected)                                                   \
    do {                                                                               \
        if ((actual) == (unexpected)) {                                                \
            ::isolate::test::fail_at(__FILE__, __LINE__,                               \
                                     "CHECK_NE failed: " #actual " != " #unexpected);  \
        }                                                                              \
    } while (false)

#define CHECK_THROWS(expr)                                                            \
    do {                                                                              \
        bool isolate_threw_ = false;                                                  \
        try {                                                                         \
            (void)(expr);                                                             \
        } catch (const std::exception&) {                                             \
            isolate_threw_ = true;                                                    \
        }                                                                             \
        if (!isolate_threw_) {                                                        \
            ::isolate::test::fail_at(__FILE__, __LINE__,                              \
                                     "expected an exception from: " #expr);           \
        }                                                                             \
    } while (false)

#define CHECK_NO_THROW(expr)                                                              \
    do {                                                                                  \
        try {                                                                             \
            (void)(expr);                                                                 \
        } catch (const std::exception& isolate_err_) {                                    \
            ::isolate::test::fail_at(__FILE__, __LINE__,                                  \
                                     std::string("unexpected exception from " #expr ": ") + \
                                         isolate_err_.what());                            \
        }                                                                                 \
    } while (false)
