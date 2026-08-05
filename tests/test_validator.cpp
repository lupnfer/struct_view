#include <doctest/doctest.h>
#include <struct_view/Validator.hpp>
#include <struct_view/ConfigLoader.hpp>
#include <struct_view/NameRegistry.hpp>
#include <struct_view/ConnectorLib.hpp>
#include <struct_view/ValueProvider.hpp>

static sv::ConfigAst parse(std::string_view t) {
    return sv::ConfigLoader::parse(t).ast;  // assume well-formed in tests
}

TEST_CASE("Validator: collects ALL missing names at once") {
    auto ast = parse(R"({"recipes":[{"name":"r","template":"${a}${b}:${c}"}]})");
    sv::NameRegistry reg;
    sv::ConnectorLib lib;
    auto errs = sv::Validator::validate(ast, reg, lib);
    // a, b, c all missing -> 3 errors (collect, don't stop at first)
    CHECK(errs.size() == 3);
}

TEST_CASE("Validator: known field + connector + literal pass clean") {
    auto ast = parse(R"({"recipes":[
        {"name":"r","template":"${time}|${dash}lit"}]})");
    sv::NameRegistry reg;
    reg.registerProvider("time", sv::makeProvider([](const void*, const sv::DeviceCtx&){return std::string();}));
    sv::ConnectorLib lib;
    lib.add("dash", "-");
    auto errs = sv::Validator::validate(ast, reg, lib);
    CHECK(errs.empty());
}

TEST_CASE("Validator: struct block subRecipe must exist as a recipe") {
    auto ast = parse(R"({"recipes":[{"name":"main","template":"${person}"}]})");
    sv::NameRegistry reg;
    reg.registerStruct("person", sv::Navigator([](const void*)->const void*{return nullptr;}), "person");
    sv::ConnectorLib lib;
    auto errs = sv::Validator::validate(ast, reg, lib);
    REQUIRE_FALSE(errs.empty());
    bool mentionsPerson = false;
    for (auto& e : errs) if (e.message.find("person") != std::string::npos) mentionsPerson = true;
    CHECK(mentionsPerson);
}

TEST_CASE("Validator: detects cyclic struct-block dependency") {
    // main -> person(structblock, subRecipe "person")
    // recipe "person" -> addr(structblock, subRecipe "main")  => cycle main->person->main
    auto ast = parse(R"({"recipes":[
        {"name":"main","template":"${person}"},
        {"name":"person","template":"${addr}"}]})");
    sv::NameRegistry reg;
    reg.registerStruct("person", sv::Navigator([](const void*)->const void*{return nullptr;}), "person");
    reg.registerStruct("addr", sv::Navigator([](const void*)->const void*{return nullptr;}), "main");
    sv::ConnectorLib lib;
    auto errs = sv::Validator::validate(ast, reg, lib);
    bool foundCycle = false;
    for (auto& e : errs) if (e.message.find("cycle") != std::string::npos) foundCycle = true;
    CHECK(foundCycle);
}

TEST_CASE("Validator: struct array subRecipe must exist as a recipe") {
    auto ast = parse(R"({"recipes":[{"name":"main","template":"${boxes}"}]})");
    sv::NameRegistry reg;
    reg.registerStructArray("boxes",
        sv::IndexedNavigator([](const void*, std::size_t) -> const void* { return nullptr; }),
        "box", 4, "|");   // "box" subRecipe does not exist
    sv::ConnectorLib lib;
    auto errs = sv::Validator::validate(ast, reg, lib);
    REQUIRE_FALSE(errs.empty());
    bool mentions = false;
    for (auto& e : errs) if (e.message.find("box") != std::string::npos) mentions = true;
    CHECK(mentions);
}

TEST_CASE("Validator: struct array valid subRecipe passes clean") {
    auto ast = parse(R"({"recipes":[
        {"name":"main","template":"[${boxes}]"},
        {"name":"box","template":"${x}"}]})");
    sv::NameRegistry reg;
    reg.registerStructArray("boxes",
        sv::IndexedNavigator([](const void*, std::size_t) -> const void* { return nullptr; }),
        "box", 4, "|");
    reg.registerProvider("x", sv::makeProvider([](const void*, const sv::DeviceCtx&){return std::string("1");}));
    sv::ConnectorLib lib;
    auto errs = sv::Validator::validate(ast, reg, lib);
    CHECK(errs.empty());
}

TEST_CASE("Validator: detects cyclic struct-array dependency") {
    // main -> boxes(array, subRecipe "box") -> box recipe refs arr2(array, subRecipe "main") => cycle
    auto ast = parse(R"({"recipes":[
        {"name":"main","template":"${boxes}"},
        {"name":"box","template":"${arr2}"}]})");
    sv::NameRegistry reg;
    reg.registerStructArray("boxes",
        sv::IndexedNavigator([](const void*, std::size_t) -> const void* { return nullptr; }),
        "box", 4, "|");
    reg.registerStructArray("arr2",
        sv::IndexedNavigator([](const void*, std::size_t) -> const void* { return nullptr; }),
        "main", 2, "|");
    sv::ConnectorLib lib;
    auto errs = sv::Validator::validate(ast, reg, lib);
    bool foundCycle = false;
    for (auto& e : errs) if (e.message.find("cycle") != std::string::npos) foundCycle = true;
    CHECK(foundCycle);
}
