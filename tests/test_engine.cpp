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

namespace {
struct EngArrBox { int x, y; };
struct EngArrEv { uint64_t ts; EngArrBox boxes[4]; };
}

TEST_CASE("Engine: render with struct array end-to-end") {
    sv::NameRegistry reg;
    reg.registerProvider("time", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const EngArrEv* e = static_cast<const EngArrEv*>(p);
        char b[32]; std::snprintf(b,sizeof(b),"%llu",(unsigned long long)e->ts); return b;
    }));
    reg.registerStructArray("boxes",
        sv::IndexedNavigator([](const void* p, std::size_t i) -> const void* {
            const EngArrEv* e = static_cast<const EngArrEv*>(p); return &e->boxes[i];
        }), "box", 4, "|");
    reg.registerProvider("x", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const EngArrBox* b = static_cast<const EngArrBox*>(p); char buf[16]; std::snprintf(buf,sizeof(buf),"%d",b->x); return buf;
    }));
    reg.registerProvider("y", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const EngArrBox* b = static_cast<const EngArrBox*>(p); char buf[16]; std::snprintf(buf,sizeof(buf),"%d",b->y); return buf;
    }));
    sv::ConnectorLib lib;
    lib.add("comma", ",");
    sv::Engine engine(reg, lib);
    REQUIRE(engine.loadConfig(R"({"recipes":[
        {"name":"line","template":"${time}|${boxes}"},
        {"name":"box","template":"${x}${comma}${y}"}]})").ok);
    EngArrEv e{42, {{1,2},{3,4},{5,6},{7,8}} };
    MyDeviceCtx ctx;
    CHECK(engine.render("line", &e, ctx) == "42|1,2|3,4|5,6|7,8");
}
