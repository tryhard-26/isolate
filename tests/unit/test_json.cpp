#include "harness.h"

#include "util/json.h"

using namespace isolate;
using namespace isolate::json;

TEST(json, parses_scalars) {
    CHECK(parse("null").is_null());
    CHECK_EQ(parse("true").as_bool(), true);
    CHECK_EQ(parse("false").as_bool(), false);
    CHECK_EQ(parse("42").as_int(), int64_t(42));
    CHECK_EQ(parse("-7").as_int(), int64_t(-7));
    CHECK_EQ(parse("1.5").as_double(), 1.5);
    CHECK_EQ(parse("\"hello\"").as_string(), std::string("hello"));
}

TEST(json, keeps_large_integers_exact) {
    // Byte counts and timestamps must survive a round trip without being
    // rounded through a double.
    int64_t big = 9007199254740993LL; // 2^53 + 1
    Value v(big);
    CHECK_EQ(parse(dump(v)).as_int(), big);
}

TEST(json, parses_nested_containers) {
    Value v = parse(R"({"a": [1, 2, {"b": "c"}], "d": {}})");
    CHECK(v.is_object());
    CHECK_EQ(v.at("a").as_array().size(), size_t(3));
    CHECK_EQ(v.at("a").as_array()[2].at("b").as_string(), std::string("c"));
    CHECK(v.at("d").as_object().empty());
}

TEST(json, decodes_escapes) {
    CHECK_EQ(parse(R"("a\nb")").as_string(), std::string("a\nb"));
    CHECK_EQ(parse(R"("a\"b")").as_string(), std::string("a\"b"));
    CHECK_EQ(parse(R"("a\\b")").as_string(), std::string("a\\b"));
    CHECK_EQ(parse(R"("A")").as_string(), std::string("A"));
    CHECK_EQ(parse(R"("é")").as_string(), std::string("\xc3\xa9"));
    // Surrogate pair for U+1F600.
    CHECK_EQ(parse(R"("😀")").as_string(), std::string("\xf0\x9f\x98\x80"));
}

TEST(json, rejects_malformed_input) {
    CHECK_THROWS(parse(""));
    CHECK_THROWS(parse("{"));
    CHECK_THROWS(parse("{\"a\"}"));
    CHECK_THROWS(parse("[1,]"));
    CHECK_THROWS(parse("tru"));
    CHECK_THROWS(parse("{} {}"));
    CHECK_THROWS(parse("\"unterminated"));
}

TEST(json, rejects_deep_nesting) {
    std::string deep(200, '[');
    CHECK_THROWS(parse(deep));
}

TEST(json, typed_accessors_report_wrong_type) {
    Value v = parse(R"({"n": 1, "s": "x"})");
    CHECK_THROWS(v.at("n").as_string());
    CHECK_THROWS(v.at("s").as_int());
    CHECK_THROWS(v.at("missing"));
}

TEST(json, fallback_accessors_tolerate_absent_fields) {
    Value v = parse(R"({"a": 1, "b": null})");
    CHECK_EQ(v.get_int("a", 9), int64_t(1));
    CHECK_EQ(v.get_int("b", 9), int64_t(9));
    CHECK_EQ(v.get_int("c", 9), int64_t(9));
    CHECK_EQ(v.get_string("c", "fallback"), std::string("fallback"));
    CHECK_EQ(v.get_bool("c", true), true);
    CHECK_EQ(v.get("c"), nullptr);
}

TEST(json, dump_round_trips) {
    const char* text = R"({"arr":[1,2,3],"nested":{"k":"v"},"flag":true,"nul":null})";
    Value once = parse(text);
    Value twice = parse(dump(once));
    CHECK_EQ(dump(once), dump(twice));
    CHECK_EQ(twice.at("arr").as_array().size(), size_t(3));
}

TEST(json, dump_escapes_control_characters) {
    Value v(std::string("line\nnext\ttab"));
    std::string text = dump(v, 0);
    CHECK_EQ(text, std::string("\"line\\nnext\\ttab\""));
    CHECK_EQ(parse(text).as_string(), std::string("line\nnext\ttab"));
}

TEST(json, object_keys_are_sorted_for_stable_output) {
    // Capsule metadata is rewritten on every state change; sorted keys keep the
    // file diffable instead of reshuffling on each write.
    std::string text = dump(parse(R"({"z":1,"a":2,"m":3})"), 0);
    CHECK_EQ(text, std::string("{\"a\":2,\"m\":3,\"z\":1}"));
}

TEST(json, empty_containers_dump_compactly) {
    CHECK_EQ(dump(parse("[]"), 2), std::string("[]\n"));
    CHECK_EQ(dump(parse("{}"), 2), std::string("{}\n"));
}
