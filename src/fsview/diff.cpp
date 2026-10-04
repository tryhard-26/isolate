#include "fsview/diff.h"

#include "fsview/extents.h"
#include "util/error.h"
#include "util/fs.h"
#include "util/path.h"
#include "util/str.h"

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <utility>

namespace isolate::fsview {
namespace {

struct EntryInfo {
    fs::Kind kind = fs::Kind::Other;
    struct stat st {};
};

using EntryMap = std::map<std::string, EntryInfo>;

EntryMap index_tree(const std::string& root, const std::vector<std::string>& exclude) {
    EntryMap out;
    if (!fs::exists(root)) return out;

    fs::walk(root, [&](const fs::Entry& e) {
        for (const std::string& ex : exclude) {
            if (e.relative == ex || path::is_within(ex, e.relative) ||
                str::match_glob(ex, e.relative, e.kind == fs::Kind::Dir)) {
                return false;
            }
        }
        EntryInfo info;
        info.kind = e.kind;
        info.st = e.st;
        out.emplace(e.relative, info);
        return true;
    });
    return out;
}

std::string octal(mode_t mode) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04o", mode & 07777);
    return buf;
}

const char* kind_name(fs::Kind k) {
    switch (k) {
        case fs::Kind::File: return "file";
        case fs::Kind::Dir: return "directory";
        case fs::Kind::Symlink: return "symlink";
        case fs::Kind::Other: return "special file";
    }
    return "entry";
}

// Same kind guaranteed by the caller.
bool same_contents(const std::string& old_path, const std::string& new_path,
                   const EntryInfo& old_info, const EntryInfo& new_info, DiffStats& stats) {
    switch (new_info.kind) {
        case fs::Kind::Dir:
            return true; // children handled separately
        case fs::Kind::Symlink:
            return fs::read_link(old_path) == fs::read_link(new_path);
        case fs::Kind::Other:
            // No contents; same rdev == unchanged.
            return old_info.st.st_rdev == new_info.st.st_rdev;
        case fs::Kind::File:
            break;
    }

    if (old_info.st.st_size != new_info.st.st_size) return false;

    switch (compare_extents(old_path, new_path)) {
        case ExtentVerdict::SharedBlocks:
            ++stats.proved_by_extents;
            return true;
        case ExtentVerdict::Different:
        case ExtentVerdict::Unknown:
            break;
    }

    ++stats.read_to_compare;
    return contents_equal(old_path, new_path);
}

bool metadata_differs(const EntryInfo& a, const EntryInfo& b, std::string& detail,
                      bool ignore_ownership) {
    if ((a.st.st_mode & 07777) != (b.st.st_mode & 07777)) {
        detail = "mode " + octal(a.st.st_mode) + " -> " + octal(b.st.st_mode);
        return true;
    }
    if (a.st.st_flags != b.st.st_flags) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "flags 0x%x -> 0x%x", a.st.st_flags, b.st.st_flags);
        detail = buf;
        return true;
    }
    if (!ignore_ownership && (a.st.st_uid != b.st.st_uid || a.st.st_gid != b.st.st_gid)) {
        detail = "owner " + std::to_string(a.st.st_uid) + ":" + std::to_string(a.st.st_gid) +
                 " -> " + std::to_string(b.st.st_uid) + ":" + std::to_string(b.st.st_gid);
        return true;
    }
    return false;
}

uint64_t entry_size(const EntryInfo& info) {
    return info.kind == fs::Kind::File ? static_cast<uint64_t>(info.st.st_size) : 0;
}

// Sorting puts a dir right before its contents, so one pass.
void collapse_subtrees(Diff& diff) {
    std::vector<Change> kept;
    kept.reserve(diff.changes.size());

    for (const Change& change : diff.changes) {
        if (!kept.empty()) {
            Change& parent = kept.back();
            bool collapsible = parent.is_dir && (parent.kind == ChangeKind::Added ||
                                                 parent.kind == ChangeKind::Deleted);
            if (collapsible && change.kind == parent.kind &&
                path::is_within(parent.path, change.path)) {
                ++parent.contained_entries;
                parent.size += change.size;
                continue;
            }
        }
        kept.push_back(change);
    }
    diff.changes = std::move(kept);
}

} // namespace

const char* to_string(ChangeKind k) {
    switch (k) {
        case ChangeKind::Added: return "added";
        case ChangeKind::Deleted: return "deleted";
        case ChangeKind::Modified: return "modified";
        case ChangeKind::TypeChanged: return "type changed";
        case ChangeKind::Metadata: return "metadata";
    }
    return "?";
}

char marker(ChangeKind k) {
    switch (k) {
        case ChangeKind::Added: return 'A';
        case ChangeKind::Deleted: return 'D';
        case ChangeKind::Modified: return 'M';
        case ChangeKind::TypeChanged: return 'T';
        case ChangeKind::Metadata: return 'm';
    }
    return '?';
}

size_t Diff::count(ChangeKind k) const {
    return static_cast<size_t>(
        std::count_if(changes.begin(), changes.end(),
                      [k](const Change& c) { return c.kind == k; }));
}

Diff diff_trees(const std::string& old_root, const std::string& new_root,
                const DiffOptions& options) {
    Diff diff;
    EntryMap old_entries = index_tree(old_root, options.exclude);
    EntryMap new_entries = index_tree(new_root, options.exclude);

    for (const auto& [rel, new_info] : new_entries) {
        ++diff.stats.compared;
        auto old_it = old_entries.find(rel);

        if (old_it == old_entries.end()) {
            Change c;
            c.kind = ChangeKind::Added;
            c.path = rel;
            c.size = entry_size(new_info);
            c.is_dir = new_info.kind == fs::Kind::Dir;
            diff.stats.bytes_added += c.size;
            diff.changes.push_back(std::move(c));
            continue;
        }

        const EntryInfo& old_info = old_it->second;
        std::string old_path = path::join(old_root, rel);
        std::string new_path = path::join(new_root, rel);

        if (old_info.kind != new_info.kind) {
            Change c;
            c.kind = ChangeKind::TypeChanged;
            c.path = rel;
            c.size = entry_size(new_info);
            c.is_dir = new_info.kind == fs::Kind::Dir;
            c.detail = std::string(kind_name(old_info.kind)) + " -> " +
                       kind_name(new_info.kind);
            diff.changes.push_back(std::move(c));
            continue;
        }

        if (!same_contents(old_path, new_path, old_info, new_info, diff.stats)) {
            Change c;
            c.kind = ChangeKind::Modified;
            c.path = rel;
            c.size = entry_size(new_info);
            diff.changes.push_back(std::move(c));
            continue;
        }

        std::string detail;
        if (options.include_metadata &&
            metadata_differs(old_info, new_info, detail, options.ignore_ownership)) {
            Change c;
            c.kind = ChangeKind::Metadata;
            c.path = rel;
            c.size = entry_size(new_info);
            c.is_dir = new_info.kind == fs::Kind::Dir;
            c.detail = std::move(detail);
            diff.changes.push_back(std::move(c));
        }
    }

    for (const auto& [rel, old_info] : old_entries) {
        if (new_entries.count(rel) != 0) continue;
        Change c;
        c.kind = ChangeKind::Deleted;
        c.path = rel;
        c.size = entry_size(old_info);
        c.is_dir = old_info.kind == fs::Kind::Dir;
        diff.stats.bytes_removed += c.size;
        diff.changes.push_back(std::move(c));
    }

    std::sort(diff.changes.begin(), diff.changes.end(),
              [](const Change& a, const Change& b) { return a.path < b.path; });

    collapse_subtrees(diff);
    return diff;
}

} // namespace isolate::fsview
