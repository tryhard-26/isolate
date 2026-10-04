#include "fsview/extents.h"

#include "util/error.h"
#include "util/fs.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace isolate::fsview {
namespace {

// Past this, reading the map costs more than the data.
constexpr size_t kMaxExtents = 4096;

} // namespace

std::optional<std::vector<Extent>> extent_map(const std::string& path) {
    fs::Fd fd(::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    if (!fd.valid()) return std::nullopt;

    struct stat st {};
    if (::fstat(fd.get(), &st) != 0 || !S_ISREG(st.st_mode)) return std::nullopt;

    // decmpfs keeps data in an xattr, not extents.
    if ((st.st_flags & UF_COMPRESSED) != 0) return std::nullopt;

    std::vector<Extent> extents;
    uint64_t size = static_cast<uint64_t>(st.st_size);
    uint64_t offset = 0;

    while (offset < size) {
        struct log2phys info {};
        info.l2p_flags = 0;
        info.l2p_devoffset = static_cast<off_t>(offset);
        info.l2p_contigbytes = static_cast<off_t>(size - offset);

        if (::fcntl(fd.get(), F_LOG2PHYS_EXT, &info) != 0) {
            // ERANGE: past allocated data, i.e. a sparse tail.
            if (errno == ERANGE) break;
            return std::nullopt;
        }
        if (info.l2p_contigbytes <= 0) break;

        Extent e;
        e.logical = offset;
        e.physical = static_cast<uint64_t>(info.l2p_devoffset);
        e.length = static_cast<uint64_t>(info.l2p_contigbytes);
        extents.push_back(e);

        offset += e.length;
        if (extents.size() > kMaxExtents) return std::nullopt;
    }

    return extents;
}

ExtentVerdict compare_extents(const std::string& a, const std::string& b) {
    std::optional<std::vector<Extent>> ma = extent_map(a);
    if (!ma.has_value()) return ExtentVerdict::Unknown;
    std::optional<std::vector<Extent>> mb = extent_map(b);
    if (!mb.has_value()) return ExtentVerdict::Unknown;

    // No extents either side, but trivially equal.
    if (ma->empty() && mb->empty()) {
        struct stat sa {};
        struct stat sb {};
        if (::stat(a.c_str(), &sa) == 0 && ::stat(b.c_str(), &sb) == 0 &&
            sa.st_size == 0 && sb.st_size == 0) {
            return ExtentVerdict::SharedBlocks;
        }
        return ExtentVerdict::Unknown;
    }

    return *ma == *mb ? ExtentVerdict::SharedBlocks : ExtentVerdict::Different;
}

bool contents_equal(const std::string& a, const std::string& b) {
    fs::Fd fa(::open(a.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    fs::Fd fb(::open(b.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    if (!fa.valid() || !fb.valid()) return false;

    struct stat sa {};
    struct stat sb {};
    if (::fstat(fa.get(), &sa) != 0 || ::fstat(fb.get(), &sb) != 0) return false;
    if (sa.st_size != sb.st_size) return false;

    constexpr size_t kChunk = 256 * 1024;
    std::vector<char> buf_a(kChunk);
    std::vector<char> buf_b(kChunk);

    while (true) {
        ssize_t na = ::read(fa.get(), buf_a.data(), kChunk);
        if (na < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        ssize_t nb = 0;
        while (nb < na) {
            ssize_t got = ::read(fb.get(), buf_b.data() + nb, static_cast<size_t>(na - nb));
            if (got < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (got == 0) break;
            nb += got;
        }
        if (na != nb) return false;
        if (na == 0) return true;
        if (std::memcmp(buf_a.data(), buf_b.data(), static_cast<size_t>(na)) != 0) return false;
    }
}

} // namespace isolate::fsview
