#include "harness.h"

#include "fsview/clone.h"
#include "fsview/extents.h"
#include "util/fs.h"

#include <fcntl.h>
#include <unistd.h>

#include <string>

using namespace isolate;
using namespace isolate::fsview;

namespace {

// Large enough that APFS allocates real extents rather than keeping the data
// inline, which is what makes the comparison meaningful.
std::string payload(size_t bytes, char fill = 'x') {
    return std::string(bytes, fill);
}

} // namespace

TEST(extents, reads_a_map_for_a_regular_file) {
    test::TempDir dir;
    dir.write("file", payload(256 * 1024));

    auto map = extent_map(dir.child("file"));
    CHECK(map.has_value());
    CHECK(!map->empty());

    // The extents must cover the file exactly once, in order.
    uint64_t covered = 0;
    uint64_t expected_offset = 0;
    for (const Extent& extent : *map) {
        CHECK_EQ(extent.logical, expected_offset);
        CHECK(extent.length > 0);
        covered += extent.length;
        expected_offset += extent.length;
    }
    CHECK(covered >= 256 * 1024);
}

TEST(extents, an_empty_file_has_no_extents) {
    test::TempDir dir;
    dir.write("empty", "");
    auto map = extent_map(dir.child("empty"));
    CHECK(map.has_value());
    CHECK(map->empty());
}

TEST(extents, returns_nothing_for_a_directory) {
    test::TempDir dir;
    dir.make_dir("sub");
    CHECK(!extent_map(dir.child("sub")).has_value());
}

TEST(extents, returns_nothing_for_a_missing_file) {
    test::TempDir dir;
    CHECK(!extent_map(dir.child("absent")).has_value());
}

TEST(extents, returns_nothing_for_a_symlink) {
    test::TempDir dir;
    dir.write("target", payload(4096));
    fs::write_link("target", dir.child("link"));
    // O_NOFOLLOW means the symlink itself is examined, and it has no extents.
    CHECK(!extent_map(dir.child("link")).has_value());
}

TEST(extents, a_clone_shares_every_block_with_its_source) {
    // This is the property that makes change detection over a cloned tree free:
    // an untouched file is proven unchanged without reading a byte of it.
    test::TempDir dir;
    dir.write("original", payload(512 * 1024));
    clone_file_over(dir.child("original"), dir.child("clone"));

    CHECK(compare_extents(dir.child("original"), dir.child("clone")) ==
          ExtentVerdict::SharedBlocks);
}

TEST(extents, writing_to_a_clone_breaks_the_sharing) {
    test::TempDir dir;
    dir.write("original", payload(512 * 1024));
    clone_file_over(dir.child("original"), dir.child("clone"));

    // One byte in the middle is enough: copy-on-write allocates a new block
    // for the page that changed.
    fs::Fd fd(::open(dir.child("clone").c_str(), O_WRONLY));
    CHECK(fd.valid());
    CHECK_EQ(::pwrite(fd.get(), "y", 1, 100000), ssize_t(1));
    fd.reset();

    CHECK(compare_extents(dir.child("original"), dir.child("clone")) ==
          ExtentVerdict::Different);
}

TEST(extents, two_independently_written_identical_files_do_not_share_blocks) {
    // The verdict is about block sharing, not content. Different is not a
    // claim that the contents differ, which is why the caller still has to
    // compare the bytes.
    test::TempDir dir;
    dir.write("a", payload(256 * 1024));
    dir.write("b", payload(256 * 1024));

    CHECK(compare_extents(dir.child("a"), dir.child("b")) == ExtentVerdict::Different);
    CHECK(contents_equal(dir.child("a"), dir.child("b")));
}

TEST(extents, two_empty_files_compare_as_equal) {
    test::TempDir dir;
    dir.write("a", "");
    dir.write("b", "");
    CHECK(compare_extents(dir.child("a"), dir.child("b")) == ExtentVerdict::SharedBlocks);
}

TEST(extents, comparison_is_unknown_when_a_map_cannot_be_read) {
    test::TempDir dir;
    dir.write("file", payload(4096));
    dir.make_dir("directory");
    CHECK(compare_extents(dir.child("file"), dir.child("directory")) ==
          ExtentVerdict::Unknown);
    CHECK(compare_extents(dir.child("file"), dir.child("absent")) ==
          ExtentVerdict::Unknown);
}

TEST(extents, contents_equal_compares_bytes) {
    test::TempDir dir;
    dir.write("a", "same");
    dir.write("b", "same");
    dir.write("c", "different");
    dir.write("d", "sames");

    CHECK(contents_equal(dir.child("a"), dir.child("b")));
    CHECK(!contents_equal(dir.child("a"), dir.child("c")));
    // Same prefix, different length.
    CHECK(!contents_equal(dir.child("a"), dir.child("d")));
}

TEST(extents, contents_equal_handles_files_larger_than_one_chunk) {
    test::TempDir dir;
    std::string big = payload(1024 * 1024, 'a');
    dir.write("a", big);
    dir.write("b", big);
    CHECK(contents_equal(dir.child("a"), dir.child("b")));

    // A difference in the last chunk must still be found.
    big[big.size() - 1] = 'b';
    dir.write("c", big);
    CHECK(!contents_equal(dir.child("a"), dir.child("c")));
}

TEST(extents, contents_equal_handles_empty_files) {
    test::TempDir dir;
    dir.write("a", "");
    dir.write("b", "");
    CHECK(contents_equal(dir.child("a"), dir.child("b")));
}

TEST(extents, contents_equal_is_false_for_unreadable_paths) {
    test::TempDir dir;
    dir.write("a", "x");
    CHECK(!contents_equal(dir.child("a"), dir.child("absent")));
}

TEST(extents, a_sparse_file_reports_a_usable_map) {
    // A file with a hole has fewer extents than its size suggests. The map must
    // still be readable, and comparing a sparse file with its clone must work.
    test::TempDir dir;
    fs::Fd fd(::open(dir.child("sparse").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600));
    CHECK(fd.valid());
    CHECK_EQ(::ftruncate(fd.get(), 4 * 1024 * 1024), 0);
    CHECK_EQ(::pwrite(fd.get(), "end", 3, 4 * 1024 * 1024 - 3), ssize_t(3));
    fd.reset();

    clone_file_over(dir.child("sparse"), dir.child("sparse-clone"));
    ExtentVerdict verdict = compare_extents(dir.child("sparse"), dir.child("sparse-clone"));
    // Either the maps match or they cannot be read; what must not happen is a
    // false Different, which would report an untouched file as modified.
    CHECK(verdict != ExtentVerdict::Different);
}
