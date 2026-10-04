#pragma once

#include <string>
#include <vector>

namespace isolate::test {

// Drives the built isolate binary as a user would, and reads back what the
// kernel actually allowed. These tests are the ones that would catch an
// isolation claim that is true in the generator but false on the machine.

struct Run {
    int status = -1;
    std::string out;
    std::string err;

    bool ok() const { return status == 0; }
    bool out_contains(const std::string& needle) const;
    bool err_contains(const std::string& needle) const;
    bool contains(const std::string& needle) const;
};

// A capsule store in a temporary directory, with helpers for invoking isolate
// against it. The store is removed when the environment goes out of scope,
// after killing anything still running in it.
class Environment {
public:
    Environment();
    Environment(const Environment&) = delete;
    Environment& operator=(const Environment&) = delete;
    ~Environment();

    // Runs isolate with the given arguments.
    Run isolate(const std::vector<std::string>& args) const;

    // Runs `isolate run --ephemeral <args>` with a shell command, which is how
    // most of these tests observe what a capsule can and cannot do.
    Run run_shell(const std::vector<std::string>& options,
                  const std::string& script) const;

    const std::string& store() const { return store_; }
    const std::string& work() const { return work_; }

    // Creates a file under the scratch work directory and returns its path.
    std::string write(const std::string& relative, const std::string& contents) const;
    std::string read(const std::string& relative) const;
    bool exists(const std::string& relative) const;
    std::string path(const std::string& relative) const;

    // The identifier of the single capsule in the store, for tests that create
    // exactly one. Fails the test if there is not exactly one.
    std::string only_capsule() const;

private:
    std::string root_;
    std::string store_;
    std::string work_;
};

// Runs an arbitrary program and captures its output. Used for the isolate
// binary itself and for probes.
Run execute(const std::string& program, const std::vector<std::string>& args,
            const std::vector<std::string>& extra_env = {});

// The path of the isolate binary under test.
const char* isolate_binary();

} // namespace isolate::test
