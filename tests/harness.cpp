#include "harness.h"

#include "util/fs.h"
#include "util/path.h"

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace isolate::test {
namespace {

bool color() {
    static const bool enabled = ::isatty(STDOUT_FILENO) == 1 && !std::getenv("NO_COLOR");
    return enabled;
}

const char* green() { return color() ? "\033[32m" : ""; }
const char* red() { return color() ? "\033[31m" : ""; }
const char* dim() { return color() ? "\033[90m" : ""; }
const char* reset() { return color() ? "\033[0m" : ""; }

bool matches(const Case& c, const std::string& filter) {
    if (filter.empty()) return true;
    if (c.suite == filter) return true;
    return c.suite + "." + c.name == filter;
}

std::string harness_temp_root() {
    if (const char* dir = std::getenv("ISOLATE_TEST_TMPDIR"); dir && *dir) return dir;
    if (const char* dir = std::getenv("TMPDIR"); dir && *dir) {
        return path::join(dir, "isolate-tests");
    }
    return "/private/tmp/isolate-tests";
}

} // namespace

std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

void fail_at(const char* file, int line, const std::string& message) {
    throw Failure{std::string(file) + ":" + std::to_string(line) + ": " + message};
}

TempDir::TempDir() {
    std::string root = harness_temp_root();
    fs::mkdir_p(root, 0700);
    std::string pattern = root + "/case.XXXXXX";
    std::vector<char> buf(pattern.begin(), pattern.end());
    buf.push_back('\0');
    if (!::mkdtemp(buf.data())) {
        std::perror("mkdtemp");
        std::abort();
    }
    // Resolved because macOS puts the temporary root behind the /var firmlink,
    // and tests compare paths against values that isolate itself resolved.
    path_ = path::resolve(buf.data());
}

TempDir::~TempDir() {
    try {
        fs::remove_tree(path_);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "warning: could not clean up %s: %s\n", path_.c_str(), e.what());
    }
}

std::string TempDir::child(const std::string& relative) const {
    return path::join(path_, relative);
}

void TempDir::write(const std::string& relative, const std::string& contents) const {
    std::string full = child(relative);
    fs::mkdir_p(path::dirname(full), 0700);
    fs::write_file_atomic(full, contents, 0600);
}

std::string TempDir::read(const std::string& relative) const {
    return fs::read_file(child(relative));
}

void TempDir::make_dir(const std::string& relative) const {
    fs::mkdir_p(child(relative), 0700);
}

int run_all(int argc, char** argv) {
    std::string filter;
    bool list_only = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--list") == 0) {
            list_only = true;
        } else {
            filter = argv[i];
        }
    }

    std::vector<Case>& cases = registry();
    std::stable_sort(cases.begin(), cases.end(), [](const Case& a, const Case& b) {
        return a.suite == b.suite ? a.name < b.name : a.suite < b.suite;
    });

    if (list_only) {
        for (const Case& c : cases) std::printf("%s.%s\n", c.suite.c_str(), c.name.c_str());
        return 0;
    }

    size_t passed = 0;
    std::vector<std::string> failures;
    std::string current_suite;

    for (const Case& c : cases) {
        if (!matches(c, filter)) continue;
        if (c.suite != current_suite) {
            current_suite = c.suite;
            std::printf("%s%s%s\n", dim(), current_suite.c_str(), reset());
        }
        std::printf("  %-52s", c.name.c_str());
        std::fflush(stdout);
        try {
            c.fn();
            ++passed;
            std::printf("%sok%s\n", green(), reset());
        } catch (const Failure& f) {
            failures.push_back(c.suite + "." + c.name + "\n    " + f.message);
            std::printf("%sFAIL%s\n", red(), reset());
        } catch (const std::exception& e) {
            failures.push_back(c.suite + "." + c.name +
                               "\n    unexpected exception: " + e.what());
            std::printf("%sFAIL%s\n", red(), reset());
        }
    }

    std::printf("\n%zu passed", passed);
    if (!failures.empty()) std::printf(", %s%zu failed%s", red(), failures.size(), reset());
    std::printf("\n");

    for (const std::string& f : failures) std::printf("\n%s%s%s\n", red(), f.c_str(), reset());

    if (passed == 0 && failures.empty()) {
        std::printf("no tests matched '%s'\n", filter.c_str());
        return 1;
    }
    return failures.empty() ? 0 : 1;
}

} // namespace isolate::test

int main(int argc, char** argv) {
    return isolate::test::run_all(argc, argv);
}
