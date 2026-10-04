#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <sys/types.h>
#include <vector>

namespace isolate::proc {

enum class Evidence {
    Attested,   // kernel confirmed profile
    Descendant, // fork child pre-exec
};

const char* to_string(Evidence e);

struct ProcInfo {
    pid_t pid = 0;
    pid_t ppid = 0;
    std::string name;
    std::string path;
    uint64_t footprint = 0; // bytes
    double cpu_seconds = 0;
    int64_t started_at = 0;
    int priority = 0;
    int thread_count = 0;
    Evidence evidence = Evidence::Attested;
};

// Track capsule procs via tree walk & kernel sb attestation
class Tracker {
public:
    Tracker(std::string membership_name, pid_t root_pid);

    // Rescan procs (root first, then by start time)
    std::vector<ProcInfo> scan();

    const std::vector<ProcInfo>& members() const { return members_; }

    uint64_t total_footprint() const;
    double total_cpu_seconds() const;
    size_t count() const { return members_.size(); }

    // Signal excess procs (newest first)
    int signal_extra_processes(int signum, size_t keep);

    // Signal all (children first)
    int signal_all(int signum);

    // True if kernel attestation verified
    bool attestation_working() const { return attestation_working_; }

    const std::string& membership_name() const { return membership_name_; }

private:
    // Keyed on pid + start time so a reused pid isn't confused
    // with the proc that held it.
    struct CacheKey {
        pid_t pid;
        int64_t started_at;
        bool operator<(const CacheKey& other) const {
            return pid != other.pid ? pid < other.pid : started_at < other.started_at;
        }
    };

    std::string membership_name_;
    pid_t root_pid_;
    std::vector<ProcInfo> members_;
    std::map<CacheKey, bool> verdicts_;
    bool attestation_working_ = true;
};

// False if gone, unreadable, or a zombie. Zombies are excluded b/c
// they're still listed and still take kill(2), so one would read as
// a leftover proc when it had in fact finished.
bool read_proc_info(pid_t pid, ProcInfo& out);

// phys_footprint, what Activity Monitor shows. RSS would count
// shared pages that aren't the proc's cost.
uint64_t process_footprint(pid_t pid);

// proc_listallpids, handling the table growing mid-call.
std::vector<pid_t> all_pids();

} // namespace isolate::proc
