#include "core/store.h"

#include "core/ids.h"
#include "util/error.h"
#include "util/fs.h"
#include "util/log.h"
#include "util/path.h"

#include <pwd.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <utility>

namespace isolate {

const char* to_string(Status s) {
    switch (s) {
        case Status::Created: return "created";
        case Status::Running: return "running";
        case Status::Stopped: return "stopped";
    }
    return "?";
}

namespace {

Status parse_status(std::string_view s) {
    if (s == "created") return Status::Created;
    if (s == "running") return Status::Running;
    if (s == "stopped") return Status::Stopped;
    throw Error("state.json: unknown status '" + std::string(s) + "'");
}

} // namespace

json::Value State::to_json() const {
    json::Object o;
    o.insert_or_assign("status", json::Value(to_string(status)));
    o.insert_or_assign("supervisor_pid", json::Value(supervisor_pid));
    o.insert_or_assign("run_count", json::Value(run_count));
    o.insert_or_assign("started_at", json::Value(started_at));
    o.insert_or_assign("finished_at", json::Value(finished_at));
    if (last_exit_code.has_value()) {
        o.insert_or_assign("last_exit_code", json::Value(*last_exit_code));
    }
    o.insert_or_assign("last_termination", json::Value(last_termination));
    return json::Value(std::move(o));
}

State State::from_json(const json::Value& v) {
    State s;
    s.status = parse_status(v.get_string("status", "created"));
    s.supervisor_pid = static_cast<int>(v.get_int("supervisor_pid", 0));
    s.run_count = static_cast<int>(v.get_int("run_count", 0));
    s.started_at = v.get_int("started_at", 0);
    s.finished_at = v.get_int("finished_at", 0);
    if (const json::Value* code = v.get("last_exit_code"); code && !code->is_null()) {
        s.last_exit_code = static_cast<int>(code->as_int());
    }
    s.last_termination = v.get_string("last_termination", "");
    return s;
}

std::string Capsule::spec_path() const { return path::join(dir_, "capsule.json"); }
std::string Capsule::state_path() const { return path::join(dir_, "state.json"); }
std::string Capsule::lock_path() const { return path::join(dir_, "lock"); }
std::string Capsule::layers_dir() const { return path::join(dir_, "layers"); }
std::string Capsule::base_dir() const { return path::join(dir_, "base"); }
std::string Capsule::snapshots_dir() const { return path::join(dir_, "snapshots"); }
std::string Capsule::home_dir() const { return path::join(dir_, "home"); }
std::string Capsule::tmp_dir() const { return path::join(dir_, "tmp"); }
std::string Capsule::log_path() const { return path::join(dir_, "log"); }

std::string Capsule::layer_dir(const std::string& clone_name) const {
    if (clone_name.empty() || clone_name.find('/') != std::string::npos ||
        clone_name == "." || clone_name == "..") {
        throw Error("malformed layer name '" + clone_name + "' in capsule metadata");
    }
    return path::join(layers_dir(), clone_name);
}

std::string Capsule::base_layer_dir(const std::string& clone_name) const {
    if (clone_name.empty() || clone_name.find('/') != std::string::npos ||
        clone_name == "." || clone_name == "..") {
        throw Error("malformed layer name '" + clone_name + "' in capsule metadata");
    }
    return path::join(base_dir(), clone_name);
}

std::string Capsule::snapshot_dir(const std::string& snapshot) const {
    if (!is_valid_snapshot_name(snapshot)) {
        throw Error("malformed snapshot name '" + snapshot + "'");
    }
    return path::join(snapshots_dir(), snapshot);
}

void Capsule::save_spec() const {
    fs::write_file_atomic(spec_path(), json::dump(spec_.to_json()), 0600);
}

void Capsule::save_state() const {
    fs::write_file_atomic(state_path(), json::dump(state_.to_json()), 0600);
}

void Capsule::reload_state() {
    if (!fs::exists(state_path())) return;
    state_ = State::from_json(json::parse(fs::read_file(state_path())));
}

std::vector<std::string> Capsule::snapshots() const {
    std::vector<std::string> out;
    for (const std::string& name : fs::list_dir(snapshots_dir())) {
        if (fs::is_dir(path::join(snapshots_dir(), name))) out.push_back(name);
    }
    return out;
}

std::string Capsule::label() const {
    return spec_.name.empty() ? spec_.id : spec_.name;
}

void Capsule::grant_to_user(const std::string& user) {
    if (::geteuid() != 0) throw Error("--user needs root");

    const passwd* pw = ::getpwnam(user.c_str());
    if (!pw) throw Error("no such user '" + user + "'");
    uid_t uid = pw->pw_uid;
    gid_t gid = pw->pw_gid;

    // 0711: traverse, not list. Siblings stay 0700/0600.
    for (const std::string& d : {dir_, layers_dir()}) {
        if (::chmod(d.c_str(), 0711) != 0) {
            throw Error("cannot open traversal on '" + d + "'", errno);
        }
    }

    // A parent we don't own can still block it; name it.
    for (const std::string& a : path::ancestors(dir_)) {
        auto st = fs::lstat_opt(a);
        if (!st.has_value()) continue;
        bool traversable = (st->st_mode & S_IXOTH) != 0 || st->st_uid == uid ||
                           (st->st_gid == gid && (st->st_mode & S_IXGRP) != 0);
        if (!traversable) {
            throw Error("user '" + user + "' cannot traverse '" + a +
                        "' (mode " + std::to_string(st->st_mode & 07777) +
                        "); put the capsule store somewhere reachable, e.g. "
                        "ISOLATE_HOME=/private/tmp/isolate");
        }
    }

    if (spec_.private_home) fs::chown_tree(home_dir(), uid, gid);
    if (spec_.private_tmp) fs::chown_tree(tmp_dir(), uid, gid);

    // Clones keep the src owner, so recurse.
    for (const Mount& m : spec_.mounts) {
        if (m.storage == Storage::Clone) fs::chown_tree(layer_dir(m.clone_name), uid, gid);
    }
}

Store::Store(std::string root) : root_(root.empty() ? path::state_dir() : std::move(root)) {
    root_ = path::absolute(root_);
}

std::string Store::capsules_dir() const {
    return path::join(root_, "capsules");
}

std::vector<std::string> Store::capsule_ids() const {
    std::vector<std::string> out;
    for (const std::string& name : fs::list_dir(capsules_dir())) {
        if (is_valid_capsule_id(name) && fs::is_dir(path::join(capsules_dir(), name))) {
            out.push_back(name);
        }
    }
    return out;
}

std::unique_ptr<Capsule> Store::create(const Spec& spec) {
    spec.validate();

    std::string dir = path::join(capsules_dir(), spec.id);
    if (fs::exists(dir)) throw Error("capsule " + spec.id + " already exists");

    // 0700: layers can hold copies of the user's files.
    fs::mkdir_p(dir, 0700);

    auto capsule = std::unique_ptr<Capsule>(new Capsule());
    capsule->dir_ = dir;
    capsule->spec_ = spec;

    fs::mkdir_p(capsule->layers_dir(), 0700);
    fs::mkdir_p(capsule->base_dir(), 0700);
    fs::mkdir_p(capsule->snapshots_dir(), 0700);
    if (spec.private_home) fs::mkdir_p(capsule->home_dir(), 0700);
    if (spec.private_tmp) fs::mkdir_p(capsule->tmp_dir(), 0700);

    capsule->lock_ = DirLock::acquire(capsule->lock_path(), spec.id);
    capsule->save_spec();
    capsule->save_state();
    return capsule;
}

std::unique_ptr<Capsule> Store::load(const std::string& id, bool exclusive) const {
    std::string dir = path::join(capsules_dir(), id);
    if (!fs::is_dir(dir)) throw Error("no capsule " + id);

    auto capsule = std::unique_ptr<Capsule>(new Capsule());
    capsule->dir_ = dir;
    capsule->spec_ = Spec::from_json(json::parse(fs::read_file(capsule->spec_path())));
    if (capsule->spec_.id != id) {
        throw Error("capsule " + id + " records a different identifier (" +
                    capsule->spec_.id + "); its metadata is inconsistent");
    }
    if (fs::exists(capsule->state_path())) {
        capsule->state_ = State::from_json(json::parse(fs::read_file(capsule->state_path())));
    }

    if (exclusive) {
        DirLock attempt = DirLock::try_acquire(capsule->lock_path(), id);
        if (!attempt.held()) {
            int holder = DirLock::holder_pid(capsule->lock_path());
            std::string who = holder ? " (held by pid " + std::to_string(holder) + ")" : "";
            throw Error("capsule " + capsule->label() + " is in use" + who);
        }
        capsule->lock_ = std::move(attempt);
    } else {
        // Supervisor gone == killed before it could update state.
        if (capsule->state_.status == Status::Running && capsule->state_.supervisor_pid > 0 &&
            ::kill(capsule->state_.supervisor_pid, 0) != 0 && errno == ESRCH) {
            capsule->state_.status = Status::Stopped;
            capsule->state_.last_termination = "supervisor exited without cleanup";
        }
    }
    return capsule;
}

std::unique_ptr<Capsule> Store::open(const std::string& reference, bool exclusive) {
    if (reference.empty()) throw Error("expected a capsule name or identifier");

    if (is_valid_capsule_id(reference)) return load(reference, exclusive);

    if (!is_valid_capsule_name(reference)) {
        throw Error("'" + reference + "' is not a valid capsule name or identifier");
    }

    std::vector<std::string> matches;
    for (const std::string& id : capsule_ids()) {
        try {
            std::unique_ptr<Capsule> candidate = load(id, false);
            if (candidate->spec().name == reference) matches.push_back(id);
        } catch (const Error&) {
            continue; // unreadable; ls reports it
        }
    }

    if (matches.empty()) throw Error("no capsule named '" + reference + "'");
    if (matches.size() > 1) {
        std::string ids = matches[0];
        for (size_t i = 1; i < matches.size(); ++i) ids += ", " + matches[i];
        throw Error("'" + reference + "' matches more than one capsule (" + ids +
                    "); use an identifier");
    }
    return load(matches[0], exclusive);
}

bool Store::exists(const std::string& reference) const {
    if (is_valid_capsule_id(reference)) {
        return fs::is_dir(path::join(capsules_dir(), reference));
    }
    for (const std::string& id : capsule_ids()) {
        try {
            if (load(id, false)->spec().name == reference) return true;
        } catch (const Error&) {
            continue;
        }
    }
    return false;
}

std::vector<std::unique_ptr<Capsule>> Store::list() const {
    std::vector<std::unique_ptr<Capsule>> out;
    for (const std::string& id : capsule_ids()) {
        try {
            out.push_back(load(id, false));
        } catch (const Error& e) {
            log::warn("skipping capsule " + id + ": " + e.what());
        }
    }
    std::sort(out.begin(), out.end(),
              [](const std::unique_ptr<Capsule>& a, const std::unique_ptr<Capsule>& b) {
                  if (a->spec().created_at != b->spec().created_at) {
                      return a->spec().created_at > b->spec().created_at;
                  }
                  return a->spec().id < b->spec().id;
              });
    return out;
}

void Store::open_traversal() {
    for (const std::string& d : {root_, capsules_dir()}) {
        if (::chmod(d.c_str(), 0711) != 0) {
            throw Error("cannot open traversal on '" + d + "'", errno);
        }
    }
}

void Store::destroy(Capsule& capsule) {
    if (!capsule.locked()) {
        throw Error("refusing to destroy capsule " + capsule.label() +
                    " without holding its lock");
    }
    // Before an rm -rf: must be in the store, named like an id.
    std::string dir = capsule.dir();
    if (!path::is_within(capsules_dir(), dir) ||
        !is_valid_capsule_id(path::basename(dir))) {
        throw Error("capsule directory '" + dir + "' is not inside the capsule store");
    }
    fs::remove_tree(dir);
}

} // namespace isolate
