#include <doctest/doctest.h>
#include <struct_view/Engine.hpp>
#include <struct_view/NameRegistry.hpp>
#include <struct_view/ConnectorLib.hpp>
#include <struct_view/DeviceCtx.hpp>
#include <struct_view/ValueProvider.hpp>
#include <cstdio>

namespace {
struct Event { uint64_t timestamp; };
struct MyDeviceCtx : sv::DeviceCtx {
    std::string cameraId() const { return "CAM001"; }
};
}

TEST_CASE("Engine: loadConfig + render end-to-end (Route B)") {
    sv::NameRegistry reg;
    reg.registerProvider("time", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const Event* e = static_cast<const Event*>(p);
        char b[32]; std::snprintf(b, sizeof(b), "%llu", (unsigned long long)e->timestamp); return b;
    }));
    reg.registerProvider("camera", sv::makeProvider([](const void*, const sv::DeviceCtx& base) -> std::string {
        return static_cast<const MyDeviceCtx&>(base).cameraId();
    }));
    sv::ConnectorLib lib;
    sv::Engine engine(reg, lib);

    auto lr = engine.loadConfig(R"({"recipes":[
        {"name":"alarm","template":"${camera}:${time}"}]})");
    REQUIRE(lr.ok);
    REQUIRE(lr.errors.empty());

    Event e{1717171717};
    MyDeviceCtx ctx;
    CHECK(engine.render("alarm", &e, ctx) == "CAM001:1717171717");
}

TEST_CASE("Engine: invalid config returns errors and does not publish") {
    sv::NameRegistry reg;
    sv::ConnectorLib lib;
    sv::Engine engine(reg, lib);
    auto lr = engine.loadConfig(R"({"recipes":[{"name":"r","template":"${missing}"}]})");
    CHECK_FALSE(lr.ok);
    CHECK_FALSE(lr.errors.empty());
    MyDeviceCtx ctx;
    CHECK(engine.render("r", nullptr, ctx) == "");  // not published
}

TEST_CASE("Engine: unknown recipe renders empty (no crash)") {
    sv::NameRegistry reg; sv::ConnectorLib lib;
    sv::Engine engine(reg, lib);
    MyDeviceCtx ctx;
    CHECK(engine.render("nope", nullptr, ctx) == "");
}
