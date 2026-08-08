#include <doctest/doctest.h>
#include <memory>
#include <string>
#include <struct_view/DeviceCtx.hpp>
#include <struct_view/Errors.hpp>
#include <struct_view/ValueProvider.hpp>
#include <struct_view/Recipe.hpp>   // RecipeB used by StructBlockProvider

namespace { struct MyDeviceCtx : sv::DeviceCtx {
    std::string cameraId() const { return "CAM001"; }
}; }

TEST_CASE("DeviceCtx: subclass usable as base ref") {
    MyDeviceCtx ctx;
    const sv::DeviceCtx& base = ctx;
    CHECK(static_cast<const MyDeviceCtx&>(base).cameraId() == "CAM001");
}

TEST_CASE("Errors: LoadResult defaults to not-ok with no errors") {
    sv::LoadResult r;
    CHECK_FALSE(r.ok);
    CHECK(r.errors.empty());
}

#include <cstdio>

namespace {
struct Event { uint64_t timestamp; };
}

TEST_CASE("ValueProvider: makeProvider lambda reads field") {
    auto p = sv::makeProvider([](const void* ptr, const sv::DeviceCtx&) -> std::string {
        const Event* e = static_cast<const Event*>(ptr);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%llu", (unsigned long long)e->timestamp);
        return buf;
    });
    Event e{1717171717};
    sv::DeviceCtx dummy;   // use a real default-constructed base, not nullptr deref
    CHECK(p->get(&e, dummy) == "1717171717");
}

TEST_CASE("ValueProvider: ConnectorProvider emits literal") {
    sv::ConnectorProvider dash("-");
    sv::DeviceCtx dummy;
    CHECK(dash.get(nullptr, dummy) == "-");
}

TEST_CASE("Navigator: stateless lambda converts to function ptr") {
    sv::Navigator nav([](const void* p) -> const void* {
        return p;  // toy: pretend embedded sub-struct at offset 0
    });
    int x = 5;
    CHECK(nav.navigate(&x) == &x);
}

namespace {
struct ArrEvent { int ids[8]; };
struct ArrBox { int x, y; };
struct ArrEventBox { ArrBox boxes[4]; };
}

TEST_CASE("ArrayStructBlockProvider: navigates each element, joins fields with sep") {
    // Field providers for x, y (on ArrBox). No sub-recipe — field list + sep.
    auto xq = sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const ArrBox* b = static_cast<const ArrBox*>(p);
        char buf[16]; std::snprintf(buf, sizeof(buf), "%d", b->x); return buf;
    });
    auto yq = sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const ArrBox* b = static_cast<const ArrBox*>(p);
        char buf[16]; std::snprintf(buf, sizeof(buf), "%d", b->y); return buf;
    });
    sv::IndexedNavigator nav([](const void* p, std::size_t i) -> const void* {
        const ArrEventBox* e = static_cast<const ArrEventBox*>(p);
        return &e->boxes[i];
    });
    // fields [x, y], element-internal sep="", element-between sep="|" → "12|34|56|78"
    sv::ArrayStructBlockProvider asbp(nav, {xq.get(), yq.get()}, 4, "", "|");
    sv::DeviceCtx dummy;
    ArrEventBox e{ {{1,2},{3,4},{5,6},{7,8}} };
    CHECK(asbp.get(&e, dummy) == "12|34|56|78");
}

TEST_CASE("ArrayStructBlockProviderA: Route A variant mirrors B (field-list traversal)") {
    // Same field providers + nav as the B test. ArrayStructBlockProviderA now has
    // identical logic (field-list traversal, no runRecipeA).
    auto xq = sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const ArrBox* b = static_cast<const ArrBox*>(p);
        char buf[16]; std::snprintf(buf, sizeof(buf), "%d", b->x); return buf;
    });
    auto yq = sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const ArrBox* b = static_cast<const ArrBox*>(p);
        char buf[16]; std::snprintf(buf, sizeof(buf), "%d", b->y); return buf;
    });
    sv::IndexedNavigator nav([](const void* p, std::size_t i) -> const void* {
        const ArrEventBox* e = static_cast<const ArrEventBox*>(p);
        return &e->boxes[i];
    });
    sv::ArrayStructBlockProviderA asbp(nav, {xq.get(), yq.get()}, 4, "", "|");
    sv::DeviceCtx dummy;
    ArrEventBox e{ {{1,2},{3,4},{5,6},{7,8}} };
    CHECK(asbp.get(&e, dummy) == "12|34|56|78");
}
