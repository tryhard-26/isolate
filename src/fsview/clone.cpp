#include "fsview/clone.h"

#include "util/error.h"
#include "util/fs.h"
#include "util/log.h"
#include "util/path.h"

#include <copyfile.h>
#include <sys/attr.h>
#include <sys/clonefile.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace isolate::fsview {
namespace {

// Longest existing prefix, so a dst that doesn't exist yet works.
bool statfs_for(const std::string& p, struct statfs& out) {
    std::string probe = path::absolute(p);
    while (true) {
        if (::statfs(probe.c_str(), &out) == 0) return true;
        if (errno != ENOENT || probe == "/") return false;
        probe = path::dirname(probe);
    }
}

void tally(const std::string& dst, CloneResult& result) {
    struct stat st {};
    if (::lstat(dst.c_str(), &st) != 0) return;
    if (!S_ISDIR(st.st_mode)) {
        result.entries = 1;
        result.bytes_logical = static_cast<uint64_t>(st.st_size);
        result.bytes_on_disk = static_cast<uint64_t>(st.st_blocks) * 512;
        return;
    }
    fs::walk(dst, [&](const fs::Entry& e) {
        ++result.entries;
        if (e.kind == fs::Kind::File) {
            result.bytes_logical += static_cast<uint64_t>(e.st.st_size);
        }
        result.bytes_on_disk += static_cast<uint64_t>(e.st.st_blocks) * 512;
        return true;
    });
}

} // namespace

std::string filesystem_type(const std::string& p) {
    struct statfs sfs {};
    if (!statfs_for(p, sfs)) return "unknown";
    return sfs.f_fstypename;
}

bool supports_cloning(const std::string& p) {
    struct statfs sfs {};
    if (!statfs_for(p, sfs)) return false;
    // Authoritative; not every APFS volume can clone.
    struct {
        uint32_t length;
        vol_capabilities_attr_t capabilities;
    } __attribute__((aligned(4), packed)) buf{};

    attrlist list{};
    list.bitmapcount = ATTR_BIT_MAP_COUNT;
    list.volattr = ATTR_VOL_INFO | ATTR_VOL_CAPABILITIES;

    if (::getattrlist(sfs.f_mntonname, &list, &buf, sizeof buf, 0) != 0) {
        return std::strcmp(sfs.f_fstypename, "apfs") == 0;
    }
    bool valid = (buf.capabilities.valid[VOL_CAPABILITIES_INTERFACES] & VOL_CAP_INT_CLONE) != 0;
    bool present =
        (buf.capabilities.capabilities[VOL_CAPABILITIES_INTERFACES] & VOL_CAP_INT_CLONE) != 0;
    return valid && present;
}

CloneResult clone_tree(const std::string& src, const std::string& dst) {
    if (!fs::exists(src)) throw Error("cannot clone '" + src + "': no such path");
    if (fs::exists(dst)) throw Error("cannot clone onto '" + dst + "': it already exists");
    fs::mkdir_p(path::dirname(dst), 0700);

    CloneResult result;

    // NOFOLLOW: a symlinked src stays a symlink instead of
    // cloning its target, which would widen reach. Symlinks
    // inside a cloned dir are preserved either way.
    if (::clonefile(src.c_str(), dst.c_str(), CLONE_NOFOLLOW) == 0) {
        result.method = CloneMethod::Clone;
        tally(dst, result);
        return result;
    }

    const int clone_errno = errno;
    bool fall_back = clone_errno == ENOTSUP || clone_errno == EXDEV || clone_errno == EINVAL ||
                     clone_errno == EOPNOTSUPP;
    if (!fall_back) {
        throw Error("cannot clone '" + src + "' to '" + dst + "'", clone_errno);
    }

    log::verbose("clonefile unavailable for '" + src + "' (" + std::strerror(clone_errno) +
                 "); copying instead. Source filesystem is " + filesystem_type(src) +
                 ", destination is " + filesystem_type(dst));

    // Not COPYFILE_CLONE_FORCE: fails for the same reason.
    copyfile_flags_t flags = COPYFILE_ALL | COPYFILE_NOFOLLOW | COPYFILE_RECURSIVE;
    if (::copyfile(src.c_str(), dst.c_str(), nullptr, flags) != 0) {
        int saved = errno;
        try {
            fs::remove_tree(dst);
        } catch (const Error&) {
            // Report the copy failure, not the cleanup's.
        }
        throw Error("cannot copy '" + src + "' to '" + dst + "'", saved);
    }

    result.method = CloneMethod::Copy;
    tally(dst, result);
    return result;
}

void clone_file_over(const std::string& src, const std::string& dst) {
    // clonefile refuses an existing dst; sibling + rename also
    // keeps it atomic for readers.
    std::string tmp = dst + ".isolate-tmp." + std::to_string(::getpid());
    if (fs::exists(tmp)) fs::remove_tree(tmp);

    if (::clonefile(src.c_str(), tmp.c_str(), CLONE_NOFOLLOW) != 0) {
        int clone_errno = errno;
        if (clone_errno != ENOTSUP && clone_errno != EXDEV && clone_errno != EINVAL &&
            clone_errno != EOPNOTSUPP) {
            throw Error("cannot clone '" + src + "'", clone_errno);
        }
        if (::copyfile(src.c_str(), tmp.c_str(), nullptr,
                       COPYFILE_ALL | COPYFILE_NOFOLLOW) != 0) {
            int saved = errno;
            ::unlink(tmp.c_str());
            throw Error("cannot copy '" + src + "' to '" + dst + "'", saved);
        }
    }

    if (::rename(tmp.c_str(), dst.c_str()) != 0) {
        int saved = errno;
        ::unlink(tmp.c_str());
        throw Error("cannot replace '" + dst + "'", saved);
    }
}

} // namespace isolate::fsview
