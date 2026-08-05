#include <doctest/doctest.h>
#include <struct_view/ConfigLoader.hpp>

TEST_CASE("ConfigLoader: parses recipes and tokenizes template") {
    std::string text = R"({
      "recipes": [
        { "name": "alarm", "template": "${cam}:${time}|literal" },
        { "name": "person", "template": "${name}-${age}" }
      ]
    })";
    auto result = sv::ConfigLoader::parse(text);
    REQUIRE(result.ok);
    REQUIRE(result.ast.recipes.size() == 2);
    CHECK(result.ast.recipes[0].name == "alarm");
    // segments: ${cam} : ${time} | literal
    const auto& segs = result.ast.recipes[0].segments;
    REQUIRE(segs.size() == 4);
    CHECK(segs[0].isRef);  CHECK(segs[0].text == "cam");
    CHECK(!segs[1].isRef); CHECK(segs[1].text == ":");
    CHECK(segs[2].isRef);  CHECK(segs[2].text == "time");
    CHECK(!segs[3].isRef); CHECK(segs[3].text == "|literal");
}

TEST_CASE("ConfigLoader: malformed JSON reports error, no ast") {
    auto result = sv::ConfigLoader::parse("{ not json");
    CHECK_FALSE(result.ok);
    CHECK_FALSE(result.parseError.empty());
}

TEST_CASE("ConfigLoader: lone $ not followed by { is literal") {
    auto result = sv::ConfigLoader::parse(R"({"recipes":[{"name":"r","template":"a$b"}]})");
    REQUIRE(result.ok);
    REQUIRE(result.ast.recipes[0].segments.size() == 1);
    CHECK(!result.ast.recipes[0].segments[0].isRef);
    CHECK(result.ast.recipes[0].segments[0].text == "a$b");
}
