#pragma once

#include "core/spec.h"
#include "util/lock.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace isolate {

enum class Status {
    Created, // dirs + layers exist, nothing ran yet
    Running, // supervisor alive, holds the lock
    Stopped,
};

const char* to_string(Status s);

// Capsule state, written to <capsule>/state.json.
struct State {
    Status status = Status::Created;
    int supervisor_pid = 0;
    int run_count = 0;
    int64_t started_at = 0;
    int64_t finished_at = 0;
    std::optional<int> last_exit_code;
    std::string last_termination; // Reason string, e.g. "sig: SIGKILL"

    json::Value to_json() const;
    static State from_json(const json::Value& v);
};

// Manages a single capsule directory and lock.
class Capsule {
public:
    const Spec& spec() const { return spec_; }
    Spec& mutable_spec() { return spec_; }
    const State& state() const { return state_; }
    State& mutable_state() { return state_; }

    const std::string& dir() const { return dir_; }
    std::string spec_path() const;
    std::string state_path() const;
    std::string lock_path() const;
    std::string layers_dir() const;
    std::string layer_dir(const std::string& clone_name) const;

    // Pristine clone base dir for 3-way diff/commit.
    std::string base_dir() const;
    std::string base_layer_dir(const std::string& clone_name) const;
    std::string snapshots_dir() const;
    std::string snapshot_dir(const std::string& snapshot) const;
    std::string home_dir() const;
    std::string tmp_dir() const;
    std::string log_path() const;

    void save_spec() const;
    void save_state() const;

    // Reload state.json from disk.
    void reload_state();

    bool locked() const { return lock_.held(); }

    std::vector<std::string> snapshots() const;

    // Capsule label (name or id).
    std::string label() const;

    // Set ownership of writable areas for --user. Root only.
    void grant_to_user(const std::string& user);

private:
    friend class Store;

    std::string dir_;
    Spec spec_;
    State state_;
    DirLock lock_;
};

// Capsule repo store (~/.local/share/isolate).
class Store {
public:
    explicit Store(std::string root = {});

    const std::string& root() const { return root_; }
    std::string capsules_dir() const;

    // Create capsule dir and acquire lock.
    std::unique_ptr<Capsule> create(const Spec& spec);

    // Open by id or name; exclusive=true locks.
    std::unique_ptr<Capsule> open(const std::string& reference, bool exclusive);

    // List all capsules, newest first.
    std::vector<std::unique_ptr<Capsule>> list() const;

    // Remove capsule dir; caller must hold lock.
    void destroy(Capsule& capsule);

    // Open dir permissions for --user.
    void open_traversal();

    bool exists(const std::string& reference) const;

private:
    std::unique_ptr<Capsule> load(const std::string& id, bool exclusive) const;
    std::vector<std::string> capsule_ids() const;

    std::string root_;
};

} // namespace isolate
