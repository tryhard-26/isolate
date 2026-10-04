#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace isolate::fsview {

enum class ChangeKind {
    Added,
    Deleted,
    Modified,    // contents differ
    TypeChanged, // file -> dir, symlink retargeted, etc
    Metadata,    // same contents, diff mode/flags/owner
};

const char* to_string(ChangeKind k);

// A, D, M, T, m.
char marker(ChangeKind k);

struct Change {
    ChangeKind kind = ChangeKind::Modified;
    std::string path;      // relative to the compared roots
    uint64_t size = 0;     // size in the new tree, or the old tree for deletions
    bool is_dir = false;
    std::string detail;    // e.g. "mode 0644 -> 0755"

    // Entries inside a wholly added/deleted dir. Not listed
    // separately: a new build dir is one change, not 1000 lines.
    uint64_t contained_entries = 0;
};

struct DiffStats {
    uint64_t compared = 0;
    uint64_t proved_by_extents = 0; // unchanged w/o reading data
    uint64_t read_to_compare = 0;
    uint64_t bytes_added = 0;
    uint64_t bytes_removed = 0;
};

struct Diff {
    std::vector<Change> changes; // sorted by path
    DiffStats stats;

    bool empty() const { return changes.empty(); }
    size_t count(ChangeKind k) const;
};

struct DiffOptions {
    bool include_metadata = true;

    // Relative to the root.
    std::vector<std::string> exclude;

    // Set for --user capsules: isolate chowned the layer, so the
    // diff would otherwise be all ownership noise.
    bool ignore_ownership = false;
};

// Compares old_root -> new_root. Unchanged clone entries are
// proven from extent maps w/ no reads; only broken-sharing files
// get a byte compare. Stats say how much of each.
Diff diff_trees(const std::string& old_root, const std::string& new_root,
                const DiffOptions& options = {});

} // namespace isolate::fsview
