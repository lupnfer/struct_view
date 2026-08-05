#include <doctest/doctest.h>
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
