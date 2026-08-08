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
    auto ast = parse(R"({"recipes":[{"name":"r_test","template":"${a}${b}:${c}"}]})");
    sv::NameRegistry reg;
    sv::ConnectorLib lib;
    auto errs = sv::Validator::validate(ast, reg, lib);
    // a, b, c all missing -> 3 errors (collect, don't stop at first)
    CHECK(errs.size() == 3);
}

TEST_CASE("Validator: known field + connector + literal pass clean") {
    auto ast = parse(R"({"recipes":[
        {"name":"r_test","template":"${time}|${dash}lit"}]})");
    sv::NameRegistry reg;
    reg.registerProvider("time", sv::makeProvider([](const void*, const sv::DeviceCtx&){return std::string();}));
    sv::ConnectorLib lib;
    lib.add("dash", "-");
    auto errs = sv::Validator::validate(ast, reg, lib);
    CHECK(errs.empty());
}

TEST_CASE("Validator: recipe name must start with r_") {
    auto ast = parse(R"({"recipes":[{"name":"bad","template":"${x}"}]})");
    sv::NameRegistry reg;
    reg.registerProvider("x", sv::makeProvider([](const void*, const sv::DeviceCtx&){return std::string("1");}));
    sv::ConnectorLib lib;
    auto errs = sv::Validator::validate(ast, reg, lib);
    REQUIRE_FALSE(errs.empty());
    bool found = false;
    for (auto& e : errs) if (e.message.find("r_") != std::string::npos) found = true;
    CHECK(found);
}

TEST_CASE("Validator: block name must not start with r_") {
    auto ast = parse(R"({"recipes":[{"name":"r_ok","template":"${r_bad}"}]})");
    sv::NameRegistry reg;
    reg.registerStruct("r_bad", sv::Navigator([](const void*)->const void*{return nullptr;}),
        {"x"}, ",", "");
    sv::ConnectorLib lib;
    auto errs = sv::Validator::validate(ast, reg, lib);
    REQUIRE_FALSE(errs.empty());
    bool found = false;
    for (auto& e : errs) if (e.message.find("r_") != std::string::npos) found = true;
    CHECK(found);
}

TEST_CASE("Validator: block fields must exist in registry") {
    auto ast = parse(R"({"recipes":[{"name":"r_main","template":"${person}"}]})");
    sv::NameRegistry reg;
    reg.registerStruct("person",
        sv::Navigator([](const void*) -> const void* { return nullptr; }),
        {"name", "age"}, "-", "");
    reg.registerProvider("name", sv::makeProvider([](const void*, const sv::DeviceCtx&){return std::string("n");}));
    // age not registered → error
    sv::ConnectorLib lib;
    auto errs = sv::Validator::validate(ast, reg, lib);
    REQUIRE_FALSE(errs.empty());
    bool found = false;
    for (auto& e : errs) if (e.message.find("age") != std::string::npos) found = true;
    CHECK(found);
}

TEST_CASE("Validator: valid block with all fields passes clean") {
    auto ast = parse(R"({"recipes":[{"name":"r_main","template":"[${person}]"}]})");
    sv::NameRegistry reg;
    reg.registerStruct("person",
        sv::Navigator([](const void*) -> const void* { return nullptr; }),
        {"name", "age"}, "-", "");
    reg.registerProvider("name", sv::makeProvider([](const void*, const sv::DeviceCtx&){return std::string("n");}));
    reg.registerProvider("age", sv::makeProvider([](const void*, const sv::DeviceCtx&){return std::string("30");}));
    sv::ConnectorLib lib;
    auto errs = sv::Validator::validate(ast, reg, lib);
    CHECK(errs.empty());
}

TEST_CASE("Validator: struct array fields must exist in registry") {
    auto ast = parse(R"({"recipes":[{"name":"r_main","template":"${boxes}"}]})");
    sv::NameRegistry reg;
    reg.registerStructArray("boxes",
        sv::IndexedNavigator([](const void*, std::size_t) -> const void* { return nullptr; }),
        {"x", "y"}, 4, ",", "|");
    reg.registerProvider("x", sv::makeProvider([](const void*, const sv::DeviceCtx&){return std::string("1");}));
    // y not registered → error
    sv::ConnectorLib lib;
    auto errs = sv::Validator::validate(ast, reg, lib);
    REQUIRE_FALSE(errs.empty());
    bool found = false;
    for (auto& e : errs) if (e.message.find("y") != std::string::npos) found = true;
    CHECK(found);
}

TEST_CASE("Validator: struct array valid fields passes clean") {
    auto ast = parse(R"({"recipes":[
        {"name":"r_main","template":"[${boxes}]"}]})");
    sv::NameRegistry reg;
    reg.registerStructArray("boxes",
        sv::IndexedNavigator([](const void*, std::size_t) -> const void* { return nullptr; }),
        {"x"}, 4, "", "|");
    reg.registerProvider("x", sv::makeProvider([](const void*, const sv::DeviceCtx&){return std::string("1");}));
    sv::ConnectorLib lib;
    auto errs = sv::Validator::validate(ast, reg, lib);
    CHECK(errs.empty());
}

TEST_CASE("Validator: detects cyclic user sub-recipe dependency") {
    // r_a refs r_b, r_b refs r_a → cycle
    auto ast = parse(R"js({"recipes":[
        {"name":"r_a","template":"${r_b}"},
        {"name":"r_b","template":"${r_a}"}]})js");
    sv::NameRegistry reg;
    sv::ConnectorLib lib;
    auto errs = sv::Validator::validate(ast, reg, lib);
    bool foundCycle = false;
    for (auto& e : errs) if (e.message.find("cycle") != std::string::npos) foundCycle = true;
    CHECK(foundCycle);
}

TEST_CASE("Validator: user sub-recipe reference resolves (no false unknown-name)") {
    auto ast = parse(R"({"recipes":[
        {"name":"r_main","template":"[${r_sub}]"},
        {"name":"r_sub","template":"${x}"}]})");
    sv::NameRegistry reg;
    reg.registerProvider("x", sv::makeProvider([](const void*, const sv::DeviceCtx&){return std::string("1");}));
    sv::ConnectorLib lib;
    auto errs = sv::Validator::validate(ast, reg, lib);
    CHECK(errs.empty());
}
