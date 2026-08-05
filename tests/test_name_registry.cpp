#include <doctest/doctest.h>
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

TEST_CASE("NameRegistry: registerStruct stores name + subRecipe name") {
    sv::NameRegistry reg;
    reg.registerStruct("person", sv::Navigator([](const void*) -> const void* { return nullptr; }), "person");
    auto blocks = reg.structBlocks();
    REQUIRE(blocks.size() == 1);
    CHECK(blocks[0].first == "person");
    CHECK(blocks[0].second->subRecipeName() == "person");
}

TEST_CASE("NameRegistry: unknown lookup returns nullptr") {
    sv::NameRegistry reg;
    CHECK(reg.lookup("nope") == nullptr);
}
