#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace isolate::fsview {

// One contiguous run of file data on the device.
struct Extent {
    uint64_t logical = 0;  // offset within the file
    uint64_t physical = 0; // offset on the device
    uint64_t length = 0;

    bool operator==(const Extent& other) const {
        return logical == other.logical && physical == other.physical &&
               length == other.length;
    }
};

// fcntl(F_LOG2PHYS_EXT). Nothing for compressed files (decmpfs
// keeps data in an xattr), fs's w/o the call, or inline data.
// Absent map means unknown, never unchanged.
std::optional<std::vector<Extent>> extent_map(const std::string& path);

enum class ExtentVerdict {
    SharedBlocks, // identical maps: same bytes on disk
    Different,    // maps differ, contents may or may not
    Unknown,      // couldn't read one of the maps
};

// Compares by extent map alone, reading no data. An untouched
// clone shares every block, so it's proven unchanged w/ zero I/O.
// SharedBlocks proves equality. Different does NOT prove
// inequality: a rewrite w/ the same bytes breaks sharing, so the
// caller still has to compare contents.
ExtentVerdict compare_extents(const std::string& a, const std::string& b);

bool contents_equal(const std::string& a, const std::string& b);

} // namespace isolate::fsview
