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
        {"name":"r_test","template":"${time}-${lit}"}]})").ast;
    sv::NameRegistry reg;
    reg.registerProvider("time", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const Event* e = static_cast<const Event*>(p);
        char b[32]; std::snprintf(b, sizeof(b), "%llu", (unsigned long long)e->timestamp); return b;
    }));
    sv::ConnectorLib lib;
    lib.add("lit", "lit");
    auto result = sv::Builder::compile(ast, reg, lib);
    REQUIRE(result.ok);
    auto snap = result.recipes.at("r_test");
    REQUIRE(snap);
    Event e{1717171717, {}};
    sv::DeviceCtx dummy;
    REQUIRE(snap->steps.size() == 3);  // time, "-", lit
    std::string out;
    for (auto& s : snap->steps) out += s.provider->get(&e, dummy);
    CHECK(out == "1717171717-lit");
}

TEST_CASE("Builder: struct block compiles to field-list provider") {
    auto ast = sv::ConfigLoader::parse(R"({"recipes":[
        {"name":"r_main","template":"[${person}]"}]})").ast;
    sv::NameRegistry reg;
    reg.registerStruct("person", sv::Navigator([](const void* p) -> const void* {
        const Event* e = static_cast<const Event*>(p); return &e->person;
    }), {"name"}, "", "");
    reg.registerProvider("name", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const PersonInfo* pi = static_cast<const PersonInfo*>(p);
        return std::string(pi->name);
    }));
    sv::ConnectorLib lib;
    auto result = sv::Builder::compile(ast, reg, lib);
    REQUIRE(result.ok);
    auto snap = result.recipes.at("r_main");
    REQUIRE(snap);
    Event e{0, {}};
    std::strncpy(e.person.name, "Alice", 32);
    sv::DeviceCtx dummy;
    std::string out;
    for (auto& s : snap->steps) out += s.provider->get(&e, dummy);
    CHECK(out == "[Alice]");
}

namespace {
struct ArrScalarEv { int ids[8]; };
struct ArrBox { int x, y; };
struct ArrStructEv { ArrBox boxes[4]; };
struct ArrNestedBox { int x, y; int tags[2]; };
struct ArrNestedEv { ArrNestedBox boxes[4]; };
}

TEST_CASE("Builder: scalar array compiles to looping provider") {
    auto ast = sv::ConfigLoader::parse(R"({"recipes":[
        {"name":"r_test","template":"${ids}"}]})").ast;
    sv::NameRegistry reg;
    reg.registerProvider("ids", sv::makeProvider(
        [](const void* p, const sv::DeviceCtx&) -> std::string {
            const ArrScalarEv* s = static_cast<const ArrScalarEv*>(p);
            std::string out; char b[16];
            for (std::size_t i = 0; i < 8; ++i) { if (i) out += "-"; std::snprintf(b,sizeof(b),"%d",s->ids[i]); out += b; }
            return out;
        }));
    sv::ConnectorLib lib;
    auto result = sv::Builder::compile(ast, reg, lib);
    REQUIRE(result.ok);
    auto snap = result.recipes.at("r_test");
    ArrScalarEv e{{11,22,33,0,0,0,0,0}};
    sv::DeviceCtx dummy;
    std::string out;
    for (auto& s : snap->steps) out += s.provider->get(&e, dummy);
    CHECK(out == "11-22-33-0-0-0-0-0");
}

TEST_CASE("Builder: struct array compiles to field-list provider") {
    auto ast = sv::ConfigLoader::parse(R"({"recipes":[
        {"name":"r_main","template":"[${boxes}]"}]})").ast;
    sv::NameRegistry reg;
    reg.registerStructArray("boxes",
        sv::IndexedNavigator([](const void* p, std::size_t i) -> const void* {
            const ArrStructEv* s = static_cast<const ArrStructEv*>(p); return &s->boxes[i];
        }), {"x", "y"}, 4, ",", "|");
    reg.registerProvider("x", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const ArrBox* b = static_cast<const ArrBox*>(p); char buf[16]; std::snprintf(buf,sizeof(buf),"%d",b->x); return buf;
    }));
    reg.registerProvider("y", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const ArrBox* b = static_cast<const ArrBox*>(p); char buf[16]; std::snprintf(buf,sizeof(buf),"%d",b->y); return buf;
    }));
    sv::ConnectorLib lib;
    auto result = sv::Builder::compile(ast, reg, lib);
    REQUIRE(result.ok);
    auto snap = result.recipes.at("r_main");
    ArrStructEv e{ {{1,2},{3,4},{5,6},{7,8}} };
    sv::DeviceCtx dummy;
    std::string out;
    for (auto& s : snap->steps) out += s.provider->get(&e, dummy);
    // boxes: 4 elements, each x,y joined by ",", elements joined by "|", wrapped in []
    CHECK(out == "[1,2|3,4|5,6|7,8]");
}

TEST_CASE("Builder: struct array with array field in element (nested combo, §9a)") {
    auto ast = sv::ConfigLoader::parse(R"js({"recipes":[
        {"name":"r_main","template":"${boxes}"}]})js").ast;
    sv::NameRegistry reg;
    reg.registerStructArray("boxes",
        sv::IndexedNavigator([](const void* p, std::size_t i) -> const void* {
            const ArrNestedEv* s = static_cast<const ArrNestedEv*>(p); return &s->boxes[i];
        }), {"x", "tags"}, 4, "", "|");
    reg.registerProvider("x", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const ArrNestedBox* b = static_cast<const ArrNestedBox*>(p); char buf[16]; std::snprintf(buf,sizeof(buf),"%d",b->x); return buf;
    }));
    reg.registerProvider("tags", sv::makeProvider(
        [](const void* p, const sv::DeviceCtx&) -> std::string {
            const ArrNestedBox* b = static_cast<const ArrNestedBox*>(p);
            std::string out; char buf[16];
            for (std::size_t i = 0; i < 2; ++i) { if (i) out += "-"; std::snprintf(buf,sizeof(buf),"%d",b->tags[i]); out += buf; }
            return out;
        }));
    sv::ConnectorLib lib;
    auto result = sv::Builder::compile(ast, reg, lib);
    REQUIRE(result.ok);
    auto snap = result.recipes.at("r_main");
    ArrNestedEv e{ {{1,2,{7,8}},{3,4,{9,10}},{5,6,{11,12}},{7,8,{13,14}}} };
    sv::DeviceCtx dummy;
    std::string out;
    for (auto& s : snap->steps) out += s.provider->get(&e, dummy);
    // boxes: 4 elements, each x+tags joined by "" (sep=""), elements joined by "|"
    // x=1, tags=7-8 → "17-8"; x=3, tags=9-10 → "39-10"; etc.
    CHECK(out == "17-8|39-10|511-12|713-14");
}
