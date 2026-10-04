#include "harness.h"

#include "util/error.h"
#include "util/str.h"

using namespace isolate;
using namespace isolate::str;

TEST(str, split_handles_empty_and_edges) {
    CHECK_EQ(split("", ',').size(), size_t(0));
    CHECK_EQ(split("a", ',').size(), size_t(1));

    auto three = split("a,b,c", ',');
    CHECK_EQ(three.size(), size_t(3));
    CHECK_EQ(three[2], std::string("c"));

    // A trailing separator yields a trailing empty field, which is what the
    // option parser relies on to reject "--ro=/a,".
    auto trailing = split("a,", ',');
    CHECK_EQ(trailing.size(), size_t(2));
    CHECK_EQ(trailing[1], std::string(""));
}

TEST(str, join_round_trips_split) {
    CHECK_EQ(join(split("a:b:c", ':'), ":"), std::string("a:b:c"));
    CHECK_EQ(join({}, ":"), std::string(""));
}

TEST(str, trim_removes_surrounding_whitespace) {
    CHECK_EQ(std::string(trim("  hi \t\n")), std::string("hi"));
    CHECK_EQ(std::string(trim("")), std::string(""));
    CHECK_EQ(std::string(trim("   ")), std::string(""));
}

TEST(str, prefix_and_suffix_tests) {
    CHECK(starts_with("abcdef", "abc"));
    CHECK(!starts_with("ab", "abc"));
    CHECK(ends_with("abcdef", "def"));
    CHECK(!ends_with("ab", "abc"));
    CHECK(starts_with("abc", ""));
}

TEST(str, human_bytes_matches_ls_style) {
    CHECK_EQ(human_bytes(0), std::string("0B"));
    CHECK_EQ(human_bytes(1023), std::string("1023B"));
    CHECK_EQ(human_bytes(1024), std::string("1.0K"));
    CHECK_EQ(human_bytes(1536), std::string("1.5K"));
    CHECK_EQ(human_bytes(10 * 1024), std::string("10K"));
    CHECK_EQ(human_bytes(uint64_t(4) << 30), std::string("4.0G"));
}

TEST(str, human_duration_formats) {
    CHECK_EQ(human_duration(0), std::string("0s"));
    CHECK_EQ(human_duration(45), std::string("45s"));
    CHECK_EQ(human_duration(252), std::string("4m12s"));
    CHECK_EQ(human_duration(3600 + 4 * 60), std::string("1h04m"));
}

TEST(str, parse_bytes_accepts_suffixes) {
    CHECK_EQ(parse_bytes("1024"), uint64_t(1024));
    CHECK_EQ(parse_bytes("1K"), uint64_t(1024));
    CHECK_EQ(parse_bytes("512m"), uint64_t(512) << 20);
    CHECK_EQ(parse_bytes("4G"), uint64_t(4) << 30);
    CHECK_EQ(parse_bytes("1.5g"), uint64_t(1610612736));
    CHECK_EQ(parse_bytes(" 2MiB "), uint64_t(2) << 20);
}

TEST(str, parse_bytes_rejects_garbage) {
    CHECK_THROWS(parse_bytes(""));
    CHECK_THROWS(parse_bytes("abc"));
    CHECK_THROWS(parse_bytes("12x"));
    CHECK_THROWS(parse_bytes("-5M"));
}

TEST(str, parse_duration_accepts_compound_values) {
    CHECK_EQ(parse_duration("90"), 90.0);
    CHECK_EQ(parse_duration("30s"), 30.0);
    CHECK_EQ(parse_duration("5m"), 300.0);
    CHECK_EQ(parse_duration("1h30m"), 5400.0);
    CHECK_EQ(parse_duration("2d"), 172800.0);
}

TEST(str, parse_duration_rejects_garbage) {
    CHECK_THROWS(parse_duration(""));
    CHECK_THROWS(parse_duration("soon"));
    CHECK_THROWS(parse_duration("10y"));
}

TEST(str, parse_cpu_accepts_percent_and_multiple) {
    CHECK_EQ(parse_cpu("100%"), 1.0);
    CHECK_EQ(parse_cpu("200%"), 2.0);
    CHECK_EQ(parse_cpu("1.5"), 1.5);
    CHECK_THROWS(parse_cpu("0"));
    CHECK_THROWS(parse_cpu("-1"));
    CHECK_THROWS(parse_cpu("lots"));
}

TEST(str, sbpl_quote_escapes_quotes_and_backslashes) {
    CHECK_EQ(sbpl_quote("/tmp/plain"), std::string("\"/tmp/plain\""));
    CHECK_EQ(sbpl_quote("/tmp/a\"b"), std::string("\"/tmp/a\\\"b\""));
    CHECK_EQ(sbpl_quote("/tmp/a\\b"), std::string("\"/tmp/a\\\\b\""));
}

TEST(str, sbpl_quote_refuses_control_characters) {
    // A newline inside a path would terminate the SBPL string and let the rest
    // of the path be read as profile source, so it has to be refused outright.
    CHECK_THROWS(sbpl_quote(std::string("/tmp/a\nb")));
    CHECK_THROWS(sbpl_quote(std::string("/tmp/a\tb")));
}

TEST(str, shell_quote_only_quotes_when_needed) {
    CHECK_EQ(shell_quote("/usr/bin/true"), std::string("/usr/bin/true"));
    CHECK_EQ(shell_quote("a b"), std::string("'a b'"));
    CHECK_EQ(shell_quote("it's"), std::string("'it'\\''s'"));
    CHECK_EQ(shell_quote(""), std::string("''"));
}
