#include <doctest/doctest.h>
#include <string>
#include <vector>
#include <struct_view/NameRegistry.hpp>
#include <struct_view/ValueProvider.hpp>
#include <struct_view/Recipe.hpp>
#include <cstdio>

namespace { struct Event { uint64_t timestamp; }; }

TEST_CASE("NameRegistry: registerProvider + lookup") {
    sv::NameRegistry reg;
    reg.registerProvider("time", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const Event* e = static_cast<const Event*>(p);
        char b[32]; std::snprintf(b, sizeof(b), "%llu", (unsigned long long)e->timestamp);
        return b;
    }));
    const sv::ValueProvider* vp = reg.lookup("time");
    REQUIRE(vp != nullptr);
    Event e{42};
    sv::DeviceCtx dummy;
    CHECK(vp->get(&e, dummy) == "42");
}

TEST_CASE("NameRegistry: registerStruct stores name in decls") {
    sv::NameRegistry reg;
    reg.registerStruct("person",
        sv::Navigator([](const void*) -> const void* { return nullptr; }),
        {"name"}, "-", "");
    auto decls = reg.structDecls();
    REQUIRE(decls.size() == 1);
    CHECK(decls[0].first == "person");
}

TEST_CASE("NameRegistry: unknown lookup returns nullptr") {
    sv::NameRegistry reg;
    CHECK(reg.lookup("nope") == nullptr);
}

TEST_CASE("NameRegistry: registerStructArray stores decl") {
    sv::NameRegistry reg;
    reg.registerStructArray("boxes",
        sv::IndexedNavigator([](const void*, std::size_t) -> const void* { return nullptr; }),
        {"w", "h"}, 4, ",", "|", "");
    auto decls = reg.structArrayDecls();
    REQUIRE(decls.size() == 1);
    CHECK(decls[0].first == "boxes");
    CHECK(decls[0].second.fieldNames == std::vector<std::string>{"w", "h"});
    CHECK(decls[0].second.count == 4);
    CHECK(decls[0].second.sep == ",");
    CHECK(decls[0].second.arraySep == "|");
}

TEST_CASE("NameRegistry: struct array not in lookup (only scalars/devices)") {
    sv::NameRegistry reg;
    reg.registerStructArray("boxes",
        sv::IndexedNavigator([](const void*, std::size_t) -> const void* { return nullptr; }),
        {"w", "h"}, 4, ",", "|", "");
    CHECK(reg.lookup("boxes") == nullptr);   // struct arrays are NOT in entries_
}

TEST_CASE("NameRegistry: registerStruct stores fields+sep+desc (no subRecipe)") {
    sv::NameRegistry reg;
    reg.registerStruct("person",
        sv::Navigator([](const void*) -> const void* { return nullptr; }),
        {"name", "age"}, "-", "人体信息");
    auto decls = reg.structDecls();
    REQUIRE(decls.size() == 1);
    CHECK(decls[0].first == "person");
    CHECK(decls[0].second.fieldNames == std::vector<std::string>{"name", "age"});
    CHECK(decls[0].second.sep == "-");
    CHECK(decls[0].second.desc == "人体信息");
}

TEST_CASE("NameRegistry: registerProvider stores desc; describe() returns it") {
    sv::NameRegistry reg;
    reg.registerProvider("x", sv::makeProvider([](const void*, const sv::DeviceCtx&){return std::string("1");}), "横坐标");
    CHECK(reg.describe("x") == "横坐标");
    CHECK(reg.describe("nope").empty());
}

TEST_CASE("NameRegistry: registerProvider desc defaults to empty (backward compat)") {
    sv::NameRegistry reg;
    reg.registerProvider("y", sv::makeProvider([](const void*, const sv::DeviceCtx&){return std::string("2");}));
    CHECK(reg.describe("y").empty());
}
