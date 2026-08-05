#include <doctest/doctest.h>
#include <struct_view/Builder.hpp>
#include <struct_view/ConfigLoader.hpp>
#include <struct_view/NameRegistry.hpp>
#include <struct_view/ConnectorLib.hpp>
#include <struct_view/ValueProvider.hpp>
#include <struct_view/DeviceCtx.hpp>
#include <struct_view/Recipe.hpp>
#include <cstring>
#include <cstdio>

namespace {
struct PersonInfo { char name[32]; int age; };
struct Event { uint64_t timestamp; PersonInfo person; };
}

TEST_CASE("Builder: compiles literals + field refs into RecipeB") {
    auto ast = sv::ConfigLoader::parse(R"({"recipes":[
        {"name":"r","template":"${time}-${lit}"}]})").ast;
    sv::NameRegistry reg;
    reg.registerProvider("time", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const Event* e = static_cast<const Event*>(p);
        char b[32]; std::snprintf(b, sizeof(b), "%llu", (unsigned long long)e->timestamp); return b;
    }));
    sv::ConnectorLib lib;
    lib.add("lit", "lit");  // ${lit} is a connector ref (cf. Validator test's ${dash})
    auto result = sv::Builder::compile(ast, reg, lib);
    REQUIRE(result.ok);
    auto snap = result.recipes.at("r");
    REQUIRE(snap);
    Event e{1717171717, {}};
    sv::DeviceCtx dummy;
    REQUIRE(snap->steps.size() == 3);  // time, "-", lit
    std::string out;
    for (auto& s : snap->steps) out += s.provider->get(&e, dummy);
    CHECK(out == "1717171717-lit");
}

TEST_CASE("Builder: binds struct block sub-recipe pointers") {
    auto ast = sv::ConfigLoader::parse(R"({"recipes":[
        {"name":"main","template":"[${person}]"},
        {"name":"person","template":"${name}"}]})").ast;
    sv::NameRegistry reg;
    reg.registerStruct("person", sv::Navigator([](const void* p) -> const void* {
        const Event* e = static_cast<const Event*>(p); return &e->person;
    }), "person");
    reg.registerProvider("name", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const PersonInfo* pi = static_cast<const PersonInfo*>(p);
        return std::string(pi->name);
    }));
    sv::ConnectorLib lib;
    auto result = sv::Builder::compile(ast, reg, lib);
    REQUIRE(result.ok);
    auto snap = result.recipes.at("main");
    REQUIRE(snap);
    Event e{0, {}};
    std::strncpy(e.person.name, "Alice", 32);
    sv::DeviceCtx dummy;
    std::string out;
    for (auto& s : snap->steps) out += s.provider->get(&e, dummy);
    CHECK(out == "[Alice]");
}
