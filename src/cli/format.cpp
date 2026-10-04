#include "cli/format.h"

#include "fsview/layer.h"
#include "proc/limits.h"
#include "sb/services.h"
#include "util/log.h"
#include "util/str.h"

#include <cstdio>
#include <ctime>
#include <utility>

namespace isolate::cli {
namespace {

bool use_color() {
    return log::stdout_is_tty() && !std::getenv("NO_COLOR");
}

const char* dim() { return use_color() ? "\033[90m" : ""; }
const char* bold() { return use_color() ? "\033[1m" : ""; }
const char* reset() { return use_color() ? "\033[0m" : ""; }

} // namespace

std::string format_timestamp(int64_t unix_seconds) {
    if (unix_seconds <= 0) return "-";
    std::time_t t = static_cast<std::time_t>(unix_seconds);
    std::tm tm {};
    localtime_r(&t, &tm);
    char buf[64];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

std::string format_relative(int64_t unix_seconds) {
    if (unix_seconds <= 0) return "-";
    int64_t now = static_cast<int64_t>(std::time(nullptr));
    int64_t delta = now - unix_seconds;
    if (delta < 0) return "in the future";
    if (delta < 10) return "just now";
    if (delta < 60) return std::to_string(delta) + " seconds ago";
    if (delta < 3600) {
        int64_t minutes = delta / 60;
        return std::to_string(minutes) + (minutes == 1 ? " minute ago" : " minutes ago");
    }
    if (delta < 86400) {
        int64_t hours = delta / 3600;
        return std::to_string(hours) + (hours == 1 ? " hour ago" : " hours ago");
    }
    int64_t days = delta / 86400;
    return std::to_string(days) + (days == 1 ? " day ago" : " days ago");
}

void print_table(const std::vector<std::string>& headers,
                 const std::vector<std::vector<std::string>>& rows) {
    std::vector<size_t> widths(headers.size(), 0);
    for (size_t i = 0; i < headers.size(); ++i) widths[i] = headers[i].size();
    for (const std::vector<std::string>& row : rows) {
        for (size_t i = 0; i < row.size() && i < widths.size(); ++i) {
            widths[i] = std::max(widths[i], row[i].size());
        }
    }

    std::string line;
    for (size_t i = 0; i < headers.size(); ++i) {
        line += headers[i];
        if (i + 1 < headers.size()) {
            line.append(widths[i] - headers[i].size() + 2, ' ');
        }
    }
    std::printf("%s%s%s\n", dim(), line.c_str(), reset());

    for (const std::vector<std::string>& row : rows) {
        std::string out;
        for (size_t i = 0; i < row.size(); ++i) {
            out += row[i];
            if (i + 1 < row.size() && i < widths.size()) {
                out.append(widths[i] - row[i].size() + 2, ' ');
            }
        }
        std::printf("%s\n", out.c_str());
    }
}

void print_fields(const std::vector<std::pair<std::string, std::string>>& fields) {
    size_t width = 0;
    for (const auto& [label, value] : fields) {
        (void)value;
        width = std::max(width, label.size());
    }
    for (const auto& [label, value] : fields) {
        std::string pad(width - label.size(), ' ');
        std::printf("  %s%s%s  %s\n", label.c_str(), pad.c_str(), ":", value.c_str());
    }
}

void print_heading(const std::string& text) {
    std::printf("%s%s%s\n", bold(), text.c_str(), reset());
}

void print_json(const json::Value& value) {
    std::string text = json::dump(value);
    std::fwrite(text.data(), 1, text.size(), stdout);
}

void print_diff(const std::string& label, const fsview::Diff& diff, bool show_stats) {
    if (!label.empty()) print_heading(label);

    if (diff.empty()) {
        std::printf("  no changes\n");
    } else {
        for (const fsview::Change& change : diff.changes) {
            std::string suffix;
            if (change.is_dir) suffix = "/";
            std::string detail;
            if (!change.detail.empty()) {
                detail = std::string("  ") + dim() + "(" + change.detail + ")" + reset();
            } else if (change.contained_entries > 0) {
                detail = std::string("  ") + dim() + std::to_string(change.contained_entries) +
                         (change.contained_entries == 1 ? " entry, " : " entries, ") +
                         str::human_bytes(change.size) + reset();
            } else if (change.kind == fsview::ChangeKind::Added && !change.is_dir) {
                detail = std::string("  ") + dim() + str::human_bytes(change.size) + reset();
            }
            std::printf("  %c %s%s%s\n", fsview::marker(change.kind), change.path.c_str(),
                        suffix.c_str(), detail.c_str());
        }
    }

    if (!show_stats) return;

    std::string summary;
    auto add = [&summary](size_t count, const char* word) {
        if (count == 0) return;
        if (!summary.empty()) summary += ", ";
        summary += std::to_string(count);
        summary += " ";
        summary += word;
    };
    add(diff.count(fsview::ChangeKind::Added), "added");
    add(diff.count(fsview::ChangeKind::Modified), "modified");
    add(diff.count(fsview::ChangeKind::Deleted), "deleted");
    add(diff.count(fsview::ChangeKind::TypeChanged), "retyped");
    add(diff.count(fsview::ChangeKind::Metadata), "metadata only");
    if (summary.empty()) summary = "nothing changed";

    std::printf("\n  %s%s%s\n", dim(), summary.c_str(), reset());
    std::printf("  %s%llu entries examined; %llu proven unchanged from their extent maps, "
                "%llu compared byte for byte%s\n",
                dim(), static_cast<unsigned long long>(diff.stats.compared),
                static_cast<unsigned long long>(diff.stats.proved_by_extents),
                static_cast<unsigned long long>(diff.stats.read_to_compare), reset());
}

json::Value diff_to_json(const fsview::Diff& diff) {
    json::Array changes;
    for (const fsview::Change& change : diff.changes) {
        json::Object o;
        o.insert_or_assign("kind", json::Value(fsview::to_string(change.kind)));
        o.insert_or_assign("path", json::Value(change.path));
        o.insert_or_assign("size", json::Value(change.size));
        o.insert_or_assign("directory", json::Value(change.is_dir));
        if (!change.detail.empty()) o.insert_or_assign("detail", json::Value(change.detail));
        if (change.contained_entries > 0) {
            o.insert_or_assign("contained_entries", json::Value(change.contained_entries));
        }
        changes.emplace_back(std::move(o));
    }

    json::Object stats;
    stats.insert_or_assign("compared", json::Value(diff.stats.compared));
    stats.insert_or_assign("proved_by_extents", json::Value(diff.stats.proved_by_extents));
    stats.insert_or_assign("read_to_compare", json::Value(diff.stats.read_to_compare));
    stats.insert_or_assign("bytes_added", json::Value(diff.stats.bytes_added));
    stats.insert_or_assign("bytes_removed", json::Value(diff.stats.bytes_removed));

    json::Object root;
    root.insert_or_assign("changes", json::Value(std::move(changes)));
    root.insert_or_assign("stats", json::Value(std::move(stats)));
    return json::Value(std::move(root));
}

json::Value capsule_to_json(const Capsule& capsule, bool include_policy) {
    const Spec& spec = capsule.spec();
    json::Object root;
    root.insert_or_assign("id", json::Value(spec.id));
    root.insert_or_assign("name", json::Value(spec.name));
    root.insert_or_assign("created_at", json::Value(spec.created_at));
    root.insert_or_assign("ephemeral", json::Value(spec.ephemeral));
    root.insert_or_assign("status", json::Value(to_string(capsule.state().status)));
    root.insert_or_assign("directory", json::Value(capsule.dir()));
    root.insert_or_assign("apparent_size_bytes",
                          json::Value(fsview::capsule_apparent_size(capsule)));
    root.insert_or_assign("run_count", json::Value(capsule.state().run_count));

    if (capsule.state().last_exit_code.has_value()) {
        root.insert_or_assign("last_exit_code",
                              json::Value(*capsule.state().last_exit_code));
    }
    if (!capsule.state().last_termination.empty()) {
        root.insert_or_assign("last_termination",
                              json::Value(capsule.state().last_termination));
    }

    if (!include_policy) return json::Value(std::move(root));

    root.insert_or_assign("policy", spec.to_json());

    json::Array limit_notes;
    for (const proc::LimitNote& note : proc::describe(spec.limits)) {
        json::Object o;
        o.insert_or_assign("name", json::Value(note.name));
        o.insert_or_assign("value", json::Value(note.value));
        o.insert_or_assign("kernel_enforced", json::Value(note.kernel_enforced));
        o.insert_or_assign("mechanism", json::Value(note.mechanism));
        limit_notes.emplace_back(std::move(o));
    }
    root.insert_or_assign("limit_enforcement", json::Value(std::move(limit_notes)));

    json::Array services;
    for (const std::string& name : sb::services_for(spec)) services.emplace_back(name);
    root.insert_or_assign("allowed_services", json::Value(std::move(services)));

    json::Array snapshots;
    for (const fsview::SnapshotInfo& info : fsview::snapshot_list(capsule)) {
        json::Object o;
        o.insert_or_assign("name", json::Value(info.name));
        o.insert_or_assign("created_at", json::Value(info.created_at));
        o.insert_or_assign("apparent_size_bytes", json::Value(info.bytes_on_disk));
        if (!info.note.empty()) o.insert_or_assign("note", json::Value(info.note));
        snapshots.emplace_back(std::move(o));
    }
    root.insert_or_assign("snapshots", json::Value(std::move(snapshots)));

    return json::Value(std::move(root));
}

json::Value process_to_json(const proc::ProcInfo& info) {
    json::Object o;
    o.insert_or_assign("pid", json::Value(info.pid));
    o.insert_or_assign("ppid", json::Value(info.ppid));
    o.insert_or_assign("name", json::Value(info.name));
    o.insert_or_assign("path", json::Value(info.path));
    o.insert_or_assign("footprint_bytes", json::Value(info.footprint));
    o.insert_or_assign("cpu_seconds", json::Value(info.cpu_seconds));
    o.insert_or_assign("started_at", json::Value(info.started_at));
    o.insert_or_assign("threads", json::Value(info.thread_count));
    o.insert_or_assign("priority", json::Value(info.priority));
    o.insert_or_assign("evidence", json::Value(proc::to_string(info.evidence)));
    return json::Value(std::move(o));
}

} // namespace isolate::cli
