#include "proc/tracker.h"

#include "sb/tag.h"
#include "util/log.h"

#include <libproc.h>
#include <signal.h>
#include <sys/proc_info.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <set>
#include <utility>

namespace isolate::proc {

const char* to_string(Evidence e) {
    switch (e) {
        case Evidence::Attested: return "attested";
        case Evidence::Descendant: return "descendant";
    }
    return "?";
}

std::vector<pid_t> all_pids() {
    std::vector<pid_t> pids;
    // Can grow between sizing and reading, so over-allocate
    // and retry if the answer fills the buffer.
    int needed = ::proc_listallpids(nullptr, 0);
    if (needed <= 0) return pids;

    size_t capacity = static_cast<size_t>(needed) + 64;
    for (int attempt = 0; attempt < 4; ++attempt) {
        pids.assign(capacity, 0);
        int bytes = ::proc_listallpids(pids.data(),
                                       static_cast<int>(capacity * sizeof(pid_t)));
        if (bytes <= 0) {
            pids.clear();
            return pids;
        }
        size_t count = static_cast<size_t>(bytes) / sizeof(pid_t);
        if (count < capacity) {
            pids.resize(count);
            // Zero entries for skipped slots.
            pids.erase(std::remove(pids.begin(), pids.end(), 0), pids.end());
            return pids;
        }
        capacity *= 2;
    }
    pids.clear();
    return pids;
}

uint64_t process_footprint(pid_t pid) {
    rusage_info_v4 info {};
    if (::proc_pid_rusage(pid, RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&info)) != 0) {
        return 0;
    }
    return info.ri_phys_footprint;
}

bool read_proc_info(pid_t pid, ProcInfo& out) {
    proc_bsdinfo bsd {};
    int got = ::proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bsd, sizeof bsd);
    if (got < static_cast<int>(sizeof bsd)) return false;

    // Exited, awaiting reap. Not running, so not a member.
    constexpr uint32_t kZombie = 5;
    if (bsd.pbi_status == kZombie) return false;

    out.pid = pid;
    out.ppid = static_cast<pid_t>(bsd.pbi_ppid);
    out.name = bsd.pbi_name[0] != '\0' ? bsd.pbi_name : bsd.pbi_comm;
    out.started_at = static_cast<int64_t>(bsd.pbi_start_tvsec);

    proc_taskinfo task {};
    if (::proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &task, sizeof task) >=
        static_cast<int>(sizeof task)) {
        out.priority = task.pti_priority;
        out.thread_count = task.pti_threadnum;
        // ns, user + system.
        out.cpu_seconds =
            static_cast<double>(task.pti_total_user + task.pti_total_system) / 1e9;
    }

    out.footprint = process_footprint(pid);

    char path[PROC_PIDPATHINFO_MAXSIZE];
    if (::proc_pidpath(pid, path, sizeof path) > 0) out.path = path;

    return true;
}

Tracker::Tracker(std::string membership_name, pid_t root_pid)
    : membership_name_(std::move(membership_name)), root_pid_(root_pid) {}

std::vector<ProcInfo> Tracker::scan() {
    std::vector<ProcInfo> found;
    std::map<pid_t, ProcInfo> table;
    std::vector<pid_t> pids = all_pids();

    // Once, so the parentage below sees one consistent view.
    for (pid_t pid : pids) {
        if (pid <= 1) continue;
        ProcInfo info;
        if (read_proc_info(pid, info)) table.emplace(pid, std::move(info));
    }

    bool any_attested = false;
    bool any_query_failed = false;
    std::set<pid_t> member_pids;

    for (auto& [pid, info] : table) {
        CacheKey key{pid, info.started_at};
        auto cached = verdicts_.find(key);
        bool member = false;

        if (cached != verdicts_.end()) {
            member = cached->second;
        } else {
            sb::Membership verdict = sb::check_membership(pid, membership_name_);
            switch (verdict) {
                case sb::Membership::Member:
                    member = true;
                    break;
                case sb::Membership::NotMember:
                    member = false;
                    break;
                case sb::Membership::Gone:
                    continue; // don't cache; pid may be reused
                case sb::Membership::Unknown:
                    any_query_failed = true;
                    member = false;
                    break;
            }
            // Pre-exec negatives flip once it execs.
            if (verdict != sb::Membership::Unknown) verdicts_.emplace(key, member);
        }

        if (member) {
            info.evidence = Evidence::Attested;
            member_pids.insert(pid);
            any_attested = true;
        }
    }

    // Counts even if the query failed: we started it.
    if (root_pid_ > 0 && table.count(root_pid_) != 0) member_pids.insert(root_pid_);

    // Pre-exec children. Loop until stable, so a fork chain is
    // picked up in one scan.
    bool grew = true;
    while (grew) {
        grew = false;
        for (const auto& [pid, info] : table) {
            if (member_pids.count(pid) != 0) continue;
            if (member_pids.count(info.ppid) != 0) {
                member_pids.insert(pid);
                grew = true;
            }
        }
    }

    for (pid_t pid : member_pids) {
        auto it = table.find(pid);
        if (it == table.end()) continue;
        ProcInfo info = it->second;
        CacheKey key{pid, info.started_at};
        auto cached = verdicts_.find(key);
        bool attested = cached != verdicts_.end() && cached->second;
        info.evidence = attested ? Evidence::Attested : Evidence::Descendant;
        found.push_back(std::move(info));
    }

    std::sort(found.begin(), found.end(), [this](const ProcInfo& a, const ProcInfo& b) {
        if (a.pid == root_pid_) return true;
        if (b.pid == root_pid_) return false;
        if (a.started_at != b.started_at) return a.started_at < b.started_at;
        return a.pid < b.pid;
    });

    // Working == at least one confirmed, none failed outright.
    attestation_working_ = any_attested && !any_query_failed;

    // Drop dead entries; a long run would accumulate them.
    if (verdicts_.size() > 4096) {
        std::map<CacheKey, bool> kept;
        for (const auto& [key, value] : verdicts_) {
            if (table.count(key.pid) != 0) kept.emplace(key, value);
        }
        verdicts_ = std::move(kept);
    }

    members_ = found;
    return found;
}

uint64_t Tracker::total_footprint() const {
    uint64_t total = 0;
    for (const ProcInfo& info : members_) total += info.footprint;
    return total;
}

double Tracker::total_cpu_seconds() const {
    double total = 0;
    for (const ProcInfo& info : members_) total += info.cpu_seconds;
    return total;
}

int Tracker::signal_extra_processes(int signum, size_t keep) {
    if (members_.size() <= keep) return 0;

    // Newest first, so the proc that broke the ceiling goes.
    // Never the capsule's first proc.
    std::vector<ProcInfo> candidates;
    for (const ProcInfo& info : members_) {
        if (info.pid != root_pid_) candidates.push_back(info);
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const ProcInfo& a, const ProcInfo& b) {
                  if (a.started_at != b.started_at) return a.started_at > b.started_at;
                  return a.pid > b.pid;
              });

    size_t to_kill = members_.size() - keep;
    int delivered = 0;
    for (size_t i = 0; i < candidates.size() && static_cast<size_t>(delivered) < to_kill; ++i) {
        if (::kill(candidates[i].pid, signum) == 0) ++delivered;
    }
    return delivered;
}

int Tracker::signal_all(int signum) {
    // Deepest first: a shell signalled early respawns work.
    std::vector<ProcInfo> ordered = members_;
    std::sort(ordered.begin(), ordered.end(), [](const ProcInfo& a, const ProcInfo& b) {
        if (a.started_at != b.started_at) return a.started_at > b.started_at;
        return a.pid > b.pid;
    });

    int delivered = 0;
    for (const ProcInfo& info : ordered) {
        if (::kill(info.pid, signum) == 0) ++delivered;
    }
    return delivered;
}

} // namespace isolate::proc
